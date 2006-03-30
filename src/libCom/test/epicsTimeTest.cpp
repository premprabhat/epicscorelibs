/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/*
 * Authors: Jeff HIll and Marty Kraimer
 */
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <limits.h>
#include <string.h>

#include "epicsTime.h"
#include "epicsThread.h"
#include "errlog.h"
#include "epicsUnitTest.h"


struct l_fp { /* NTP time stamp */
    epicsUInt32 l_ui; /* sec past NTP epoch */
    epicsUInt32 l_uf; /* fractional seconds */
};

epicsTime useSomeCPU;

static const unsigned mSecPerSec = 1000u;
static const unsigned uSecPerSec = 1000u * mSecPerSec;
static const unsigned nSecPerSec = 1000u * uSecPerSec;


extern "C"
int epicsTimeTest (void)
{
    const unsigned wasteTime = 100000u;
    const int nTimes = 10;
    const double precisionEPICS = 1.0 / nSecPerSec;

    testPlan(7 + nTimes * 18);

    const epicsTime begin = epicsTime::getCurrent();
    {
        epicsTime tsi = epicsTime::getCurrent ();
        l_fp ntp = tsi;
        epicsTime tsf = ntp;
        const double diff = fabs ( tsf - tsi );
        // the difference in the precision of the two time formats
        static const double precisionNTP = 1.0 / ( 1.0 + 0xffffffff );
        testOk1(diff <= precisionEPICS + precisionNTP);
    }

    {
        static const char * pFormat = "%a %b %d %Y %H:%M:%S.%4f";
        try {
            const epicsTimeStamp badTS = {1, 1000000000};
            epicsTime ts(badTS);
            char buf [32];
            ts.strftime(buf, sizeof(buf), pFormat);
            testFail("nanosecond overflow throws");
            testDiag("nanosecond overflow result is \"%s\"", buf);
        }
        catch ( ... ) {
            testPass("nanosecond overflow throws");
        }
    }

    {
        char buf[64];
        epicsTime et;

        const char * pFormat = "%Y-%m-%d %H:%M:%S.%f";
        et.strftime(buf, sizeof(buf), pFormat);
        testOk(strcmp(buf, "<undefined>") == 0, "undefined strftime") ||
        testDiag("undefined.strftime(\"%s\") = \"%s\"", pFormat, buf);

        // This is Noon GMT, when all timezones have the same date
        const epicsTimeStamp tTS = {12*60*60, 98765432};
        et = tTS;
        pFormat = "%Y-%m-%d %S.%09f";   // %H and %M change with timezone
        et.strftime(buf, sizeof(buf), pFormat);
        testOk(strcmp(buf, "1990-01-01 00.098765432") == 0, pFormat) ||
        testDiag("t.strftime(\"%s\") = \"%s\"", pFormat, buf);

        pFormat = "%S.%04f";
        et.strftime(buf, sizeof(buf), pFormat);
        testOk(strcmp(buf, "00.0988") == 0, pFormat) ||
        testDiag("t.strftime(\"%s\") = \"%s\"", pFormat, buf);

        pFormat = "%S.%05f";
        et.strftime(buf, sizeof(buf), pFormat);
        testOk(strcmp(buf, "00.09877") == 0, pFormat) ||
        testDiag("t.strftime(\"%s\") = \"%s\"", pFormat, buf);
    }

    {
        char bigBuf [512];
        char buf [32];
        memset(bigBuf, '\a', sizeof(bigBuf ));
        bigBuf[ sizeof(bigBuf) - 1] = '\0';
        begin.strftime(buf, sizeof(buf), bigBuf);
        testOk(strcmp(buf, "<invalid format>") == 0, "strftime(huge)") ||
        testDiag("strftime(huge) = \"%s\"", buf);
    }

    testDiag("Running %d loops", nTimes);

    for (int iTimes=0; iTimes < nTimes; ++iTimes) {
        for (unsigned i=0; i<wasteTime; i++) {
            useSomeCPU = epicsTime::getCurrent();
        }

        const epicsTime end = epicsTime::getCurrent();
        const double diff = end - begin;

        if (iTimes == 0) {
            testDiag ("%d calls to epicsTime::getCurrent() "
                "averaged %6.3f usec each", wasteTime,
                diff*1e6/wasteTime);
        }

        epicsTime copy = end;
        testOk1(copy == end);
        testOk1(copy <= end);
        testOk1(copy >= end);

        testOk1(end > begin);
        testOk1(end >= begin);
        testOk1(begin != end);
        testOk1(begin < end);
        testOk1(begin <= end);

        testOk1(end - end == 0);
        testOk(fabs((end - begin) - diff) < precisionEPICS * 0.01,
               "end - begin ~= diff");

        testOk1(begin + 0 == begin);
        testOk1(begin + diff == end);
        testOk1(end - 0 == end);
        testOk1(end - diff == begin);

        epicsTime end2 = begin;
        end2 += diff;
        testOk(end2 == end, "(begin += diff) == end");

        end2 = end;
        end2 -= diff;
        testOk(end2 == begin, "(end -= diff) == begin");

        // test struct tm round-trip conversion
        local_tm_nano_sec ansiDate = begin;
        epicsTime beginANSI = ansiDate;
        testOk1(beginANSI + diff == end);

        // test struct timespec round-trip conversion
        struct timespec ts = begin;
        epicsTime beginTS = ts;
        testOk1(beginTS + diff == end);
    }

    return testDone();
}
