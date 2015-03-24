/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cantProceed.h"
#include "dbDefs.h"
#include "ellLib.h"
#include "epicsAssert.h"
#include "epicsMutex.h"
#include "epicsPrint.h"
#include "epicsStdio.h"
#include "epicsThread.h"
#include "epicsSpin.h"
#include "epicsAtomic.h"
#include "errMdef.h"

#define epicsExportSharedSymbols
#include "dbAccessDefs.h"
#include "dbAddr.h"
#include "dbBase.h"
#include "dbLink.h"
#include "dbCommon.h"
#include "dbFldTypes.h"
#include "dbLockPvt.h"
#include "dbStaticLib.h"
#include "link.h"

typedef struct dbScanLockNode dbScanLockNode;

static epicsThreadOnceId dbLockOnceInit = EPICS_THREAD_ONCE_INIT;

static ELLLIST lockSetsActive; /* in use */
#ifndef LOCKSET_FREE
static ELLLIST lockSetsFree; /* free list */
#endif

/* Guard the global list */
static epicsMutexId lockSetsGuard;

#ifndef LOCKSET_NOCNT
/* Counter which is incremented whenever
 * any lockRecord::plockSet is changed.
 * An optimization to avoid a re-sort
 * when no links have changed.
 */
static size_t recomputeCnt;
#endif

/*private routines */
static void dbLockOnce(void* ignore)
{
    lockSetsGuard = epicsMutexMustCreate();
}

/* global ID number assigned to each lockSet on creation.
 * Will never exceed the number of records +1
 */
static size_t next_id = 1;

static lockSet* makeSet(void)
{
    lockSet *ls;
    int iref;
    epicsMutexMustLock(lockSetsGuard);
#ifndef LOCKSET_FREE
    ls = (lockSet*)ellGet(&lockSetsFree);
    if(!ls) {
        epicsMutexUnlock(lockSetsGuard);
#endif

        ls=dbCalloc(1,sizeof(*ls));
        ellInit(&ls->lockRecordList);
        ls->lock = epicsMutexMustCreate();
        ls->id = epicsAtomicIncrSizeT(&next_id);

#ifndef LOCKSET_FREE
        epicsMutexMustLock(lockSetsGuard);
    }
#endif
    /* the initial reference for the first lockRecord */
    iref = epicsAtomicIncrIntT(&ls->refcount);
    ellAdd(&lockSetsActive, &ls->node);
    epicsMutexUnlock(lockSetsGuard);

    assert(ls->id>0);
    assert(iref>0);
    assert(ellCount(&ls->lockRecordList)==0);

    return ls;
}

unsigned long dbLockGetRefs(struct dbCommon* prec)
{
    return (unsigned long)epicsAtomicGetIntT(&prec->lset->plockSet->refcount);
}

unsigned long dbLockCountSets(void)
{
    unsigned long count;
    epicsMutexMustLock(lockSetsGuard);
    count = (unsigned long)ellCount(&lockSetsActive);
    epicsMutexUnlock(lockSetsGuard);
    return count;
}

/* caller must lock accessLock.*/
void dbLockIncRef(lockSet* ls)
{
    int cnt = epicsAtomicIncrIntT(&ls->refcount);
    if(cnt<=0) {
        errlogPrintf("dbLockIncRef(%p) on dead lockSet\n", ls);
        cantProceed(NULL);
    }
}

/* caller must lock accessLock.
 * lockSet must *not* be locked
 */
void dbLockDecRef(lockSet *ls)
{
    int cnt = epicsAtomicDecrIntT(&ls->refcount);
    assert(cnt>=0);

    if(cnt)
        return;

    if(ellCount(&ls->lockRecordList)!=0) {
        errlogPrintf("dbLockDecRef(%p) would free lockSet with %d records\n", ls, ellCount(&ls->lockRecordList));
        cantProceed(NULL);
    }

    epicsMutexMustLock(lockSetsGuard);
    ellDelete(&lockSetsActive, &ls->node);
#ifndef LOCKSET_FREE
    ellAdd(&lockSetsFree, &ls->node);
#else
    epicsMutexDestroy(ls->lock);
    memset(ls, 0, sizeof(*ls));
    free(ls);
#endif
    epicsMutexUnlock(lockSetsGuard);
}

lockSet* dbLockGetRef(lockRecord *lr)
{
    lockSet *ls;
    epicsSpinLock(lr->spin);
    ls = lr->plockSet;
    dbLockIncRef(ls);
    epicsSpinUnlock(lr->spin);
    return ls;
}

unsigned long dbLockGetLockId(dbCommon *precord)
{
    unsigned long id=0;
    epicsSpinLock(precord->lset->spin);
    id = precord->lset->plockSet->id;
    epicsSpinUnlock(precord->lset->spin);
    return id;
}

void dbScanLock(dbCommon *precord)
{
    int cnt;
    lockRecord * const lr = precord->lset;
    lockSet *ls;

    ls = dbLockGetRef(lr);
    assert(ls->refcount>0);

retry:
    epicsMutexMustLock(ls->lock);

    epicsSpinLock(lr->spin);
    if(ls!=lr->plockSet) {
        /* oops, collided with recompute.
         * take a reference to the new lockSet.
         */
        lockSet *ls2 = lr->plockSet;
        int newcnt = epicsAtomicIncrIntT(&ls2->refcount);
        assert(newcnt>=2); /* lockRecord and us */
        epicsSpinUnlock(lr->spin);

        epicsMutexUnlock(ls->lock);
        dbLockDecRef(ls);

        ls = ls2;
        goto retry;
    }
    epicsSpinUnlock(lr->spin);

    /* Release reference taken within this
     * function.  The count will *never* fall to zero
     * as the lockRecords can't be changed while
     * we hold the lock.
     */
    cnt = epicsAtomicDecrIntT(&ls->refcount);
    assert(cnt>0);
    /* Caller does *not* hold a reference to ls.
     * However, the lockRecord does, but can't
     * be changed while we hold the lockSet.
     */
#ifdef LOCKSET_DEBUG
    if(ls->owner) {
        assert(ls->owner==epicsThreadGetIdSelf());
        assert(ls->ownercount>=1);
        ls->ownercount++;
    } else {
        assert(ls->ownercount==0);
        ls->owner = epicsThreadGetIdSelf();
        ls->ownercount = 1;
    }
#endif
    /* no references kept */
}

void dbScanUnlock(dbCommon *precord)
{
    lockSet *ls = precord->lset->plockSet;
    dbLockIncRef(ls);
#ifdef LOCKSET_DEBUG
    assert(ls->owner==epicsThreadGetIdSelf());
    assert(ls->ownercount>=1);
    ls->ownercount--;
    if(ls->ownercount==0)
        ls->owner = NULL;
#endif
    epicsMutexUnlock(ls->lock);
    dbLockDecRef(ls);
}

static
int lrrcompare(const void *rawA, const void *rawB)
{
    const lockRecordRef *refA=rawA, *refB=rawB;
    const lockSet *A=refA->plockSet, *B=refB->plockSet;
    if(!A && !B)
        return 0; /* NULL == NULL */
    else if(!A)
        return 1; /* NULL > !NULL */
    else if(!B)
        return -1; /* !NULL < NULL */
    else if(A < B)
        return -1;
    else if(A > B)
        return 1;
    else
        return 0;
}

/* Call w/ update=1 before locking to update cached lockSet entries.
 * Call w/ update=0 after locking to verify that lockRecord weren't updated
 */
static
int dbLockUpdateRefs(dbLocker *locker, int update)
{
    int changed = 0;
    size_t i, nlock = locker->maxrefs;

#ifndef LOCKSET_NOCNT
    const size_t recomp = epicsAtomicGetSizeT(&recomputeCnt);
    if(locker->recomp!=recomp) {
#endif
        /* some lockset recompute happened.
         * must re-check our references.
         */

        for(i=0; i<nlock; i++) {
            lockRecordRef *ref = &locker->refs[i];
            lockSet *oldref = NULL;
            if(!ref->plr) {
                assert(!ref->plockSet);
                continue;
            }

            epicsSpinLock(ref->plr->spin);
            if(ref->plockSet!=ref->plr->plockSet) {
                changed = 1;
                if(update) {
                    /* exchange saved lockSet reference */
                    oldref = ref->plockSet;
                    ref->plockSet = ref->plr->plockSet;
                    dbLockIncRef(ref->plockSet);
                }
            }
            epicsSpinUnlock(ref->plr->spin);
            if(oldref)
                dbLockDecRef(oldref);
            if(!update && changed)
                return changed;
        }
#ifndef LOCKSET_NOCNT
        if(update)
            locker->recomp = recomp;
    }
#endif

    if(changed && update) {
        qsort(locker->refs, nlock, sizeof(lockRecordRef),
                                          &lrrcompare);
    }
    return changed;
}

void dbLockerPrepare(struct dbLocker *locker,
                struct dbCommon **precs,
                size_t nrecs)
{
    size_t i;
    locker->maxrefs = nrecs;
    /* intentionally spoil the recomp count to ensure that
     * references will be updated this first time
     */
#ifndef LOCKSET_NOCNT
    locker->recomp = epicsAtomicGetSizeT(&recomputeCnt)-1;
#endif

    for(i=0; i<nrecs; i++) {
        locker->refs[i].plr = precs[i] ? precs[i]->lset : NULL;
    }

    /* acquire a reference to all lockRecords */
    dbLockUpdateRefs(locker, 1);
}

dbLocker *dbLockerAlloc(dbCommon **precs,
                        size_t nrecs,
                        unsigned int flags)
{
    size_t Nextra = nrecs>DBLOCKER_NALLOC ? nrecs-DBLOCKER_NALLOC : 0;
    dbLocker *locker = calloc(1, sizeof(*locker)+Nextra*sizeof(lockRecordRef));

    if(!locker)
        return NULL;

    dbLockerPrepare(locker, precs, nrecs);

    return locker;
}

void dbLockerFinalize(dbLocker *locker)
{
    size_t i;
    assert(ellCount(&locker->locked)==0);

    for(i=0; i<locker->maxrefs; i++) {
        if(locker->refs[i].plockSet)
            dbLockDecRef(locker->refs[i].plockSet);
    }
}

void dbLockerFree(dbLocker *locker)
{
    dbLockerFinalize(locker);
    free(locker);
}

/* Lock the given list of records.
 * This function modifies its arguments.
 */
void dbScanLockMany(dbLocker* locker)
{
    size_t i, nlock = locker->maxrefs;
    lockSet *plock;
#ifdef LOCKSET_DEBUG
    const epicsThreadId myself = epicsThreadGetIdSelf();
#endif

retry:
    assert(ellCount(&locker->locked)==0);
    dbLockUpdateRefs(locker, 1);

    for(i=0, plock=NULL; i<nlock; i++) {
        lockRecordRef *ref = &locker->refs[i];

        /* skip duplicates (same lockSet
         * referenced by more than one lockRecord).
         */
        if(!ref->plr || (plock && plock==ref->plockSet))
            continue;
        plock = ref->plockSet;

        epicsMutexMustLock(plock->lock);
        assert(plock->ownerlocker==NULL);
        plock->ownerlocker = locker;
        ellAdd(&locker->locked, &plock->lockernode);
        /* An extra ref for the locked list */
        dbLockIncRef(plock);

#ifdef LOCKSET_DEBUG
        if(plock->owner) {
            if(plock->owner!=myself || plock->ownercount<1) {
                errlogPrintf("dbScanLockMany(%p) ownership violation %p (%p) %u\n",
                             locker, plock->owner, myself, plock->ownercount);
                cantProceed(NULL);
            }
            plock->ownercount++;
        } else {
            assert(plock->ownercount==0);
            plock->owner = myself;
            plock->ownercount = 1;
        }
#endif

    }

    if(dbLockUpdateRefs(locker, 0)) {
        /* oops, collided with recompute */
        dbScanUnlockMany(locker);
        goto retry;
    }
    if(nlock!=0 && ellCount(&locker->locked)<=0) {
        /* if we have at least one lockRecord, then we will always lock
         * at least its present lockSet
         */
        errlogPrintf("dbScanLockMany(%p) didn't lock anything\n", locker);
        cantProceed(NULL);
    }
}

void dbScanUnlockMany(dbLocker* locker)
{
    ELLNODE *cur;
#ifdef LOCKSET_DEBUG
    const epicsThreadId myself = epicsThreadGetIdSelf();
#endif

    while((cur=ellGet(&locker->locked))!=NULL) {
        lockSet *plock = CONTAINER(cur, lockSet, lockernode);

        assert(plock->ownerlocker==locker);
        plock->ownerlocker = NULL;
#ifdef LOCKSET_DEBUG
        assert(plock->owner==myself);
        assert(plock->ownercount>=1);
        plock->ownercount--;
        if(plock->ownercount==0)
            plock->owner = NULL;
#endif

        epicsMutexUnlock(plock->lock);
        /* release ref for locked list */
        dbLockDecRef(plock);
    }
}

typedef int (*reciter)(void*, DBENTRY*);
static int forEachRecord(void *priv, dbBase *pdbbase, reciter fn)
{
    long status;
    int ret = 0;
    DBENTRY dbentry;
    dbInitEntry(pdbbase,&dbentry);
    status = dbFirstRecordType(&dbentry);
    while(!status)
    {
        status = dbFirstRecord(&dbentry);
        while(!status)
        {
            /* skip alias names */
            if(!dbentry.precnode->recordname[0] || dbentry.precnode->flags & DBRN_FLAGS_ISALIAS) {
                /* skip */
            } else {
                ret = fn(priv, &dbentry);
                if(ret)
                    goto done;
            }

            status = dbNextRecord(&dbentry);
        }

        status = dbNextRecordType(&dbentry);
    }
done:
    dbFinishEntry(&dbentry);
    return ret;
}

static int createLockRecord(void* junk, DBENTRY* pdbentry)
{
    dbCommon *prec = pdbentry->precnode->precord;
    size_t i, no_links=prec->rdes->no_links;
    lockRecord *lrec;
    size_t arrsize=(no_links-1)*sizeof(linkRef);
    assert(no_links>=1); /* dbCommon has TSEL */
    assert(!prec->lset);

    lrec = callocMustSucceed(1, sizeof(*lrec)+arrsize, "lockRecord");
    if(!lrec)
        cantProceed("no memory for lockRecord");
    lrec->spin = epicsSpinCreate();
    if(!lrec->spin)
        cantProceed("no memory for spinlock in lockRecord");

    lrec->precord = prec;
    ellInit(&lrec->backlinks);

    for(i=0; i<no_links; i++) {
        lrec->links[i].psrc = lrec;
    }

    prec->lset = lrec;
    return 0;
}

static int initPVLinks(void* junk, DBENTRY* pdbentry)
{
    size_t i;
    dbRecordType *rtype=pdbentry->precordType;
    dbCommon *prec = pdbentry->precnode->precord;
    lockSet *A=prec->lset->plockSet;

    /* for each link originating from this record */
    for(i=0; i<rtype->no_links; i++) {
        linkRef *bref = &prec->lset->links[i];
        DBADDR *paddr;
        dbFldDes *pdesc = rtype->papFldDes[rtype->link_ind[i]];
        DBLINK *plink = (DBLINK*)((char*)prec + pdesc->offset);
        lockSet *B;

        if(plink->type!=PV_LINK)
            continue;

        dbInitLink(prec, plink, pdesc->field_type);

        if(plink->type!=DB_LINK)
            continue;

        paddr = (DBADDR*)plink->value.pv_link.pvt;
        B = paddr->precord->lset->plockSet;


        /* Initial population of lockSets happens here. */
        if(!A && !B) { /* neither side has a lockSet */
            A = prec->lset->plockSet = paddr->precord->lset->plockSet = makeSet();
            dbLockIncRef(A); /* ref for psecond */
            ellAdd(&A->lockRecordList, &prec->lset->node);
            ellAdd(&A->lockRecordList, &paddr->precord->lset->node);

        } else if(!B) { /* fast merge paddr->precord into A */
            paddr->precord->lset->plockSet = A;
            dbLockIncRef(A);
            ellAdd(&A->lockRecordList, &paddr->precord->lset->node);

        } else if(!A) { /* fast merge prec into B */
            A = prec->lset->plockSet = B;
            dbLockIncRef(B);
            ellAdd(&B->lockRecordList, &prec->lset->node);
        }

        /* initialize backward link tracking */
        bref->ptarget = paddr->precord->lset;
        ellAdd(&bref->ptarget->backlinks, &bref->backlinksnode);
    }
    return 0;
}

static int initSingleSets(void* junk, DBENTRY* pdbentry)
{
    dbCommon *prec = pdbentry->precnode->precord;
    lockSet *ls;

    if(!prec->lset->plockSet) {
        ls = prec->lset->plockSet = makeSet();
        ellAdd(&ls->lockRecordList, &prec->lset->node);
    }

    return 0;
}

void dbLockInitRecords(dbBase *pdbbase)
{
    epicsThreadOnce(&dbLockOnceInit, &dbLockOnce, NULL);

    /* create all lockRecords */
    forEachRecord(NULL, pdbbase, &createLockRecord);
    /* create lockSets for pairs of records with DB_LINKs */
    forEachRecord(NULL, pdbbase, &initPVLinks);
    /* create lockSets for all records with no DB_LINKs */
    forEachRecord(NULL, pdbbase, &initSingleSets);
}

static int freeLockRecord(void* junk, DBENTRY* pdbentry)
{
    dbCommon *prec = pdbentry->precnode->precord;
    lockRecord *lr = prec->lset;
    lockSet *ls = lr->plockSet;

    prec->lset = NULL;

    assert(ls->refcount>0);
    assert(ellCount(&ls->lockRecordList)>0);
    ellDelete(&ls->lockRecordList, &lr->node);
    dbLockDecRef(ls);

    epicsSpinDestroy(lr->spin);
    free(lr);
    return 0;
}

void dbLockCleanupRecords(dbBase *pdbbase)
{
#ifndef LOCKSET_FREE
    ELLNODE *cur;
#endif
    epicsThreadOnce(&dbLockOnceInit, &dbLockOnce, NULL);

    forEachRecord(NULL, pdbbase, &freeLockRecord);
    if(ellCount(&lockSetsActive)) {
        errlogMessage("Warning: dbLockCleanupRecords() leaking lockSets\n");
        dblsr(NULL,2);
    }

    assert(ellCount(&lockSetsActive)==0);

#ifndef LOCKSET_FREE
    while((cur=ellGet(&lockSetsFree))!=NULL) {
        lockSet *ls = (lockSet*)cur;

        assert(ls->refcount==0);
        assert(ellCount(&ls->lockRecordList)==0);
        epicsMutexDestroy(ls->lock);
        free(ls);
    }
#endif
}

/* update backwards link tracking */
static void updateBackRefs(dbCommon *prec)
{
    size_t i;
    /* for each link */
    for(i=0; i<prec->rdes->no_links; i++) {
        linkRef *bref = &prec->lset->links[i];
        dbFldDes *pdesc = prec->rdes->papFldDes[prec->rdes->link_ind[i]];
        DBLINK *plink = (DBLINK*)((char*)prec + pdesc->offset);

        if(plink->type!=DB_LINK && bref->ptarget)
        {
            /* removed link */
            ellDelete(&bref->ptarget->backlinks, &bref->backlinksnode);
            bref->ptarget = NULL;
        } else if(plink->type==DB_LINK)
        {
            DBADDR *paddr = (DBADDR*)plink->value.pv_link.pvt;

            if(paddr->precord->lset != bref->ptarget) {
                /* changed link */
                if(bref->ptarget) {
                    /* clear old */
                    ellDelete(&bref->ptarget->backlinks, &bref->backlinksnode);
                    bref->ptarget = NULL;
                }

                bref->ptarget = paddr->precord->lset;
                ellAdd(&bref->ptarget->backlinks, &bref->backlinksnode);
            }
        }
    }
}

/* Caller must lock both pfirst and psecond.
 * Assumes that pfirst has been modified
 * to link to psecond.
 */
void dbLockSetMerge(dbLocker *locker, dbCommon *pfirst, dbCommon *psecond)
{
    ELLNODE *cur;
    lockSet *A=pfirst->lset->plockSet,
            *B=psecond->lset->plockSet;
    int Nb;
#ifdef LOCKSET_DEBUG
    const epicsThreadId myself = epicsThreadGetIdSelf();
#endif

    assert(A && B);

#ifdef LOCKSET_DEBUG
    if(A->owner!=myself || B->owner!=myself) {
        errlogPrintf("dbLockSetMerge(%p,\"%s\",\"%s\") ownership violation %p %p (%p)\n",
                     locker, pfirst->name, psecond->name,
                     A->owner, B->owner, myself);
        cantProceed(NULL);
    }
#endif
    if(A->ownerlocker!=locker || B->ownerlocker!=locker) {
        errlogPrintf("dbLockSetMerge(%p,\"%s\",\"%s\") locker ownership violation %p %p (%p)\n",
                     locker, pfirst->name, psecond->name,
                     A->ownerlocker, B->ownerlocker, locker);
        cantProceed(NULL);
    }

    if(A==B)
        return; /* already in the same lockSet */

    updateBackRefs(pfirst); /* not required */

    Nb = ellCount(&B->lockRecordList);
    assert(Nb>0);

    /* move all records from B to A */
    while((cur=ellGet(&B->lockRecordList))!=NULL)
    {
        lockRecord *lr = CONTAINER(cur, lockRecord, node);
        assert(lr->plockSet==B);
        epicsSpinLock(lr->spin);
        ellAdd(&A->lockRecordList, cur);
        lr->plockSet = A;
#ifndef LOCKSET_NOCNT
        epicsAtomicIncrSizeT(&recomputeCnt);
#endif
        epicsSpinUnlock(lr->spin);
    }

    /* there is at least 1 ref for each lockRecord,
     * and at least one for the locker's locked list
     * (perhaps another for its refs cache
     */
    assert(epicsAtomicGetIntT(&B->refcount)>=Nb+(locker?1:0));

    /* update ref counters. for lockRecords */
    epicsAtomicAddIntT(&A->refcount, Nb);
    epicsAtomicAddIntT(&B->refcount, -Nb+1); /* drop all but one ref, see below */

    if(locker) {
        /* at least two ref, possibly three remain.
         * # One ref from above
         * # locker->locked list, which is released now.
         * # locker->refs array, assuming it is directly referenced,
         *   and not added as the result of a dbLockSetSplit,
         *   which will be cleaned when the locker is free'd (not here).
         */
#ifdef LOCKSET_DEBUG
        B->owner = NULL;
        B->ownercount = 0;
#endif
        assert(B->ownerlocker==locker);
        ellDelete(&locker->locked, &B->lockernode);
        B->ownerlocker = NULL;
        epicsAtomicDecrIntT(&B->refcount);
    }

    epicsMutexUnlock(B->lock);

    dbLockDecRef(B); /* last ref from above */

    assert(A==psecond->lset->plockSet);
}

/* recompute assuming a link from pfirst to psecond
 * may have been removed.
 * pfirst and psecond must currently be in the same lockset,
 * which the caller must lock before calling this function.
 * If a new lockset is created, then it is locked
 * when this function returns.
 */
void dbLockSetSplit(dbLocker *locker, dbCommon *pfirst, dbCommon *psecond)
{
    lockSet *ls = pfirst->lset->plockSet;
    ELLLIST toInspect, newLS;
#ifdef LOCKSET_DEBUG
    const epicsThreadId myself = epicsThreadGetIdSelf();
#endif

#ifdef LOCKSET_DEBUG
    if(ls->owner!=myself || psecond->lset->plockSet->owner!=myself) {
        errlogPrintf("dbLockSetSplit(%p,\"%s\",\"%s\") ownership violation %p %p (%p)\n",
                     locker, pfirst->name, psecond->name,
                     ls->owner, psecond->lset->plockSet->owner, myself);
        cantProceed(NULL);
    }
#endif

    /* lockset consistency violation */
    if(ls!=psecond->lset->plockSet) {
        errlogPrintf("dbLockSetSplit(%p,\"%s\",\"%s\") consistency violation %p %p\n",
                     locker, pfirst->name, psecond->name,
                     pfirst->lset->plockSet, psecond->lset->plockSet);
        cantProceed(NULL);
    }

    updateBackRefs(pfirst);

    if(pfirst==psecond)
        return;

    /* at least 1 ref for each lockRecord,
     * and one for the locker
     */
    assert(ls->refcount>=ellCount(&ls->lockRecordList)+1);

    ellInit(&toInspect);
    ellInit(&newLS);

    /* strategy is to start with psecond and do
     * a breadth first traversal until all records are
     * visited.  If we encounter pfirst, then there
     * is no need to create a new lockset so we abort
     * early.
     */
    ellAdd(&toInspect, &psecond->lset->compnode);
    psecond->lset->compflag = 1;

    {
        lockSet *splitset;
        ELLNODE *cur;
        while((cur=ellGet(&toInspect))!=NULL)
        {
            lockRecord *lr=CONTAINER(cur,lockRecord,compnode);
            dbCommon *prec=lr->precord;
            dbRecordType *rtype = prec->rdes;
            size_t i;
            ELLNODE *bcur;

            ellAdd(&newLS, cur);
            prec->lset->compflag = 2;

            /* Visit all the links originating from prec */
            for(i=0; i<rtype->no_links; i++) {
                linkRef *bref=&lr->links[i];
                lockRecord *lr = bref->ptarget;

                if(!lr)
                    continue; /* not DB_LINK */

                if(lr->precord==pfirst) {
                    /* so pfirst is still reachable from psecond,
                     * no new lock set should be created.
                     */
                    goto nosplit;
                }

                /* have we already visited this record? */
                if(lr->compflag)
                    continue;

                ellAdd(&toInspect, &lr->compnode);
                lr->compflag = 1;
            }

            /* Visit all links terminating at prec */
            for(bcur=ellFirst(&lr->backlinks); bcur; bcur=ellNext(bcur))
            {
                linkRef *bref=CONTAINER(bcur, linkRef, backlinksnode);
                lockRecord *lr = bref->psrc;

                if(lr->precord==pfirst) {
                    goto nosplit;
                }

                if(lr->compflag)
                    continue;

                ellAdd(&toInspect, &lr->compnode);
                lr->compflag = 1;
            }
        }
        /* All links from psecond were traversed without finding
         * pfirst.  So we must create a new lockset.
         * newLS contains the nodes which will
         * make up this new lockset.
         */
        /* newLS will have at least psecond in it */
        assert(ellCount(&newLS) > 0);
        /* If we didn't find pfirst, then it must be in the
         * original lockset, and not the new one
         */
        assert(ellCount(&newLS) < ellCount(&ls->lockRecordList));
        assert(ellCount(&newLS) < ls->refcount);

        splitset = makeSet(); /* reference for locker->locked */

        epicsMutexMustLock(splitset->lock);

        assert(splitset->ownerlocker==NULL);
        ellAdd(&locker->locked, &splitset->lockernode);
        splitset->ownerlocker = locker;

        assert(splitset->refcount==1);

#ifdef LOCKSET_DEBUG
        splitset->owner = ls->owner;
        splitset->ownercount = 1;
        assert(ls->ownercount==1);
#endif

        while((cur=ellGet(&newLS))!=NULL)
        {
            lockRecord *lr=CONTAINER(cur,lockRecord,compnode);

            lr->compflag = 0; /* reset for next time */

            assert(lr->plockSet == ls);

            /* lockset is "live" at this point
             * as other threads may find it.
             */
            epicsSpinLock(lr->spin);
            ellDelete(&ls->lockRecordList, &lr->node);
            ellAdd(&splitset->lockRecordList, &lr->node);
            lr->plockSet = splitset;
#ifndef LOCKSET_NOCNT
            epicsAtomicIncrSizeT(&recomputeCnt);
#endif
            epicsSpinUnlock(lr->spin);
        }

        /* refcount of ls can't go to zero as the locker
         * holds at least one reference (its locked list)
         */
        epicsAtomicAddIntT(&ls->refcount, -ellCount(&splitset->lockRecordList));
        assert(ls->refcount>0);
        epicsAtomicAddIntT(&splitset->refcount, ellCount(&splitset->lockRecordList));

        assert(splitset->refcount>=ellCount(&splitset->lockRecordList)+1);

        assert(psecond->lset->plockSet==splitset);

        /* must have refs from pfirst lockRecord,
         * and the locked list.
         */
        assert(epicsAtomicGetIntT(&ls->refcount)>=2);

        return;
    }

nosplit:
    {
        /* reset compflag for all nodes visited
         * during the aborted search
         */
        ELLNODE *cur;
        while((cur=ellGet(&toInspect))!=NULL)
        {
            lockRecord *lr=CONTAINER(cur,lockRecord,compnode);
            lr->compflag = 0;
        }
        while((cur=ellGet(&newLS))!=NULL)
        {
            lockRecord *lr=CONTAINER(cur,lockRecord,compnode);
            lr->compflag = 0;
        }
        return;
    }
}

static char *msstring[4]={"NMS","MS","MSI","MSS"};

long dblsr(char *recordname,int level)
{
    int			link;
    DBENTRY		dbentry;
    DBENTRY		*pdbentry=&dbentry;
    long		status;
    dbCommon		*precord;
    lockSet		*plockSet;
    lockRecord		*plockRecord;
    dbRecordType	*pdbRecordType;
    dbFldDes		*pdbFldDes;
    DBLINK		*plink;

    if (recordname && ((*recordname == '\0') || !strcmp(recordname,"*")))
        recordname = NULL;
    if(recordname) {
        dbInitEntry(pdbbase,pdbentry);
        status = dbFindRecord(pdbentry,recordname);
        if(status) {
            errlogPrintf("Record not found\n");
            dbFinishEntry(pdbentry);
            goto done;
        }
        precord = pdbentry->precnode->precord;
        dbFinishEntry(pdbentry);
        plockRecord = precord->lset;
        if(!plockRecord) goto done; /* too early (before iocInit) */
        plockSet = plockRecord->plockSet;
    } else {
        plockSet = (lockSet *)ellFirst(&lockSetsActive);
    }
    for( ; plockSet; plockSet = (lockSet *)ellNext(&plockSet->node)) {
        errlogPrintf("Lock Set %lu %d members %d refs epicsMutexId %p\n",
            plockSet->id,ellCount(&plockSet->lockRecordList),plockSet->refcount,plockSet->lock);

        if(level==0) { if(recordname) break; continue; }
        for(plockRecord = (lockRecord *)ellFirst(&plockSet->lockRecordList);
        plockRecord; plockRecord = (lockRecord *)ellNext(&plockRecord->node)) {
            precord = plockRecord->precord;
            pdbRecordType = precord->rdes;
            errlogPrintf("%s\n",precord->name);
            if(level<=1) continue;
            for(link=0; (link<pdbRecordType->no_links) ; link++) {
                DBADDR	*pdbAddr;
                pdbFldDes = pdbRecordType->papFldDes[pdbRecordType->link_ind[link]];
                plink = (DBLINK *)((char *)precord + pdbFldDes->offset);
                if(plink->type != DB_LINK) continue;
                pdbAddr = (DBADDR *)(plink->value.pv_link.pvt);
                errlogPrintf("\t%s",pdbFldDes->name);
                if(pdbFldDes->field_type==DBF_INLINK) {
                    errlogPrintf("\t INLINK");
                } else if(pdbFldDes->field_type==DBF_OUTLINK) {
                    errlogPrintf("\tOUTLINK");
                } else if(pdbFldDes->field_type==DBF_FWDLINK) {
                    errlogPrintf("\tFWDLINK");
                }
                errlogPrintf(" %s %s",
                    ((plink->value.pv_link.pvlMask&pvlOptPP)?" PP":"NPP"),
                    msstring[plink->value.pv_link.pvlMask&pvlOptMsMode]);
                errlogPrintf(" %s\n",pdbAddr->precord->name);
            }
        }
        if(recordname) break;
    }
done:
    errlogFlush();
    return(0);
}

long dbLockShowLocked(int level)
{
    int     indListType;
    lockSet *plockSet;

    errlogPrintf("lockSets %d listTypeFree %d\n",
        ellCount(&lockSetsActive),
#ifndef LOCKSET_FREE
        ellCount(&lockSetsFree)
#else
                 -1
#endif
                 );

    /*Even if failure on lockSetModifyLock will continue */
    for(indListType=0; indListType <= 1; ++indListType) {
        plockSet = (lockSet *)ellFirst(&lockSetsActive);
        if(plockSet) {
            if(indListType==0) errlogPrintf("listTypeScanLock\n");
            else errlogPrintf("listTypeRecordLock\n");
        }
        while(plockSet) {
            epicsMutexLockStatus status;

            status = epicsMutexTryLock(plockSet->lock);
            if(status==epicsMutexLockOK) epicsMutexUnlock(plockSet->lock);
            if(status!=epicsMutexLockOK || indListType==1) {

                epicsMutexShow(plockSet->lock,level);
            }
            plockSet = (lockSet *)ellNext(&plockSet->node);
        }
    }
    errlogFlush();
    return(0);
}

int * dbLockSetAddrTrace(dbCommon *precord)
{
    lockRecord	*plockRecord = precord->lset;
    lockSet	*plockSet = plockRecord->plockSet;

    return(&plockSet->trace);
}
