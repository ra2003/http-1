/**
    testHttpUri.c - tests for URIs
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "http.h"

/*********************************** Locals ***********************************/

/************************************ Code ************************************/


static bool normalize(MprTestGroup *gp, char *uri, char *expectedUri)
{
    char    *validated;

    validated = httpNormalizeUriPath(uri);
    if (smatch(expectedUri, validated)) {
        return 1;
    } else {
        mprLog(0, "\nUri \"%s\" validated to \"%s\" instead of \"%s\"\n", uri, validated, expectedUri);
        return 0;
    }
}


static bool validate(MprTestGroup *gp, char *uri, char *expectedUri)
{
    cchar   *validated;

    validated = httpValidateUriPath(uri);
    if (smatch(expectedUri, validated)) {
        return 1;
    } else {
        mprLog(0, "\nUri \"%s\" validated to \"%s\" instead of \"%s\"\n", uri, validated, expectedUri);
        return 0;
    }
}


static void testCreateUri(MprTestGroup *gp)
{
    HttpUri     *uri;

    uri = httpCreateUri("", 0);
    uri = httpCreateUri("", HTTP_COMPLETE_URI);
}


static void testNormalizeUri(MprTestGroup *gp)
{
    /*
        Note that normalize permits relative URLs
     */
    tassert(normalize(gp, "", ""));
    tassert(normalize(gp, "/", "/"));
    tassert(normalize(gp, "..", ""));
    tassert(normalize(gp, "../", ""));
    tassert(normalize(gp, "/..", ""));

    tassert(normalize(gp, "./", ""));
    tassert(normalize(gp, "./.", ""));
    tassert(normalize(gp, "././", ""));

    tassert(normalize(gp, "a", "a"));
    tassert(normalize(gp, "/a", "/a"));
    tassert(normalize(gp, "a/", "a/"));
    tassert(normalize(gp, "../a", "a"));
    tassert(normalize(gp, "/a/..", "/"));
    tassert(normalize(gp, "/a/../", "/"));
    tassert(normalize(gp, "a/..", ""));
    tassert(normalize(gp, "/../a", "a"));
    tassert(normalize(gp, "../../a", "a"));
    tassert(normalize(gp, "../a/b/..", "a"));

    tassert(normalize(gp, "/b/a", "/b/a"));
    tassert(normalize(gp, "/b/../a", "/a"));
    tassert(normalize(gp, "/a/../b/..", "/"));

    tassert(normalize(gp, "/a/./", "/a/"));
    tassert(normalize(gp, "/a/./.", "/a/"));
    tassert(normalize(gp, "/a/././", "/a/"));
    tassert(normalize(gp, "/a/.", "/a/"));

    tassert(normalize(gp, "/*a////b/", "/*a/b/"));
    tassert(normalize(gp, "/*a/////b/", "/*a/b/"));

    tassert(normalize(gp, "\\a\\b\\", "\\a\\b\\"));

    tassert(normalize(gp, "/..appweb.conf", "/..appweb.conf"));
    tassert(normalize(gp, "/..\\appweb.conf", "/..\\appweb.conf"));
}


static void testValidateUri(MprTestGroup *gp)
{
    /*
        Note that validate only accepts absolute URLs that begin with "/"
     */

    tassert(validate(gp, "", 0));
    tassert(validate(gp, "/", "/"));
    tassert(validate(gp, "..", 0));
    tassert(validate(gp, "../", 0));
    tassert(validate(gp, "/..", 0));

    tassert(validate(gp, "./", 0));
    tassert(validate(gp, "./.", 0));
    tassert(validate(gp, "././", 0));

    tassert(validate(gp, "a", 0));
    tassert(validate(gp, "/a", "/a"));
    tassert(validate(gp, "a/", 0));
    tassert(validate(gp, "../a", 0));
    tassert(validate(gp, "/a/..", "/"));
    tassert(validate(gp, "/a/../", "/"));
    tassert(validate(gp, "a/..", 0));
    tassert(validate(gp, "/../a", 0));
    tassert(validate(gp, "../../a", 0));
    tassert(validate(gp, "../a/b/..", 0));

    tassert(validate(gp, "/b/a", "/b/a"));
    tassert(validate(gp, "/b/../a", "/a"));
    tassert(validate(gp, "/a/../b/..", "/"));

    tassert(validate(gp, "/a/./", "/a/"));
    tassert(validate(gp, "/a/./.", "/a/"));
    tassert(validate(gp, "/a/././", "/a/"));
    tassert(validate(gp, "/a/.", "/a/"));

    tassert(validate(gp, "/*a////b/", "/*a/b/"));
    tassert(validate(gp, "/*a/////b/", "/*a/b/"));

    tassert(validate(gp, "\\a\\b\\", 0));

    tassert(validate(gp, "/..\\appweb.conf", 0));
    tassert(validate(gp, "/\\appweb.conf", 0));
    tassert(validate(gp, "/..%5Cappweb.conf", "/..\\appweb.conf"));

    /*
        Regression tests
     */
    tassert(validate(gp, "/extra%20long/a/..", "/extra long"));
    tassert(validate(gp, "/extra%20long/../path/a/..", "/path"));
}


MprTestDef testHttpUri = {
    "uri", 0, 0, 0,
    {
        MPR_TEST(0, testCreateUri),
        MPR_TEST(0, testNormalizeUri),
        MPR_TEST(0, testValidateUri),
        MPR_TEST(0, 0),
    },
};

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
