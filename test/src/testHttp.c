/**
    testHttp.c - program for the Http unit tests
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "mpr.h"

/****************************** Test Definitions ******************************/

extern MprTestDef testHttpGen;
extern MprTestDef testHttpUri;

static MprTestDef *testGroups[] = 
{
    &testHttpUri,
    &testHttpGen,
    0
};
 
static MprTestDef master = {
    "api",
    testGroups,
    0, 0, 
    { { 0 } },
};


/************************************* Code ***********************************/

MAIN(testMain, int argc, char **argv, char **envp) 
{
    Mpr             *mpr;
    MprTestService  *ts;
    int             rc;

    mpr = mprCreate(argc, argv, MPR_USER_EVENTS_THREAD);

    if ((ts = mprCreateTestService(mpr)) == 0) {
        mprLog("http test", 0, "Cannot create test service");
        exit(2);
    }
    if (mprParseTestArgs(ts, argc, argv, 0) < 0) {
        exit(3);
    }
    if (mprAddTestGroup(ts, &master) == 0) {
        exit(4);
    }

#if BIT_FEATURE_SSL && (BIT_FEATURE_MATRIXSSL || BIT_FEATURE_OPENSSL)
    if (!mprLoadSsl(0)) {
        exit(5);
    }
#endif

    /*
        Need a background event thread as we use the main thread to run the tests.
     */
    if (mprStart(mpr)) {
        mprLog("http test", 0, "Cannot start mpr services");
        exit(4);
    }

    /*
        Run the tests and return zero if 100% success
     */
    rc = mprRunTests(ts);
    mprReportTestResults(ts);
    mprDestroy();
    return (rc == 0) ? 0 : 6;
}

/*
    @copy   default
    
    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.
    Copyright (c) Michael O'Brien, 1993-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
    
    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
