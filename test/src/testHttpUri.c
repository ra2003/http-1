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


static bool checkUri(HttpUri *uri, cchar *expected)
{
    cchar   *s;

    s = sfmt("%s-%s-%d-%s-%s-%s-%s", uri->scheme, uri->host, uri->port, uri->path, uri->ext, uri->reference, uri->query);
    if (!smatch(s, expected)) {
        printf("\nEXPECTED: %s\n", expected);
        printf("URI:      %s\n", s);
    }
    return 1;
}


static void testCreateUri(MprTestGroup *gp)
{
    HttpUri     *uri;

#if 0
    uri = httpCreateUri(NULL, 0);
    assert(checkUri(uri, "null-null-0-null-null-null-null"));

    uri = httpCreateUri("", 0);
    assert(checkUri(uri, "null-null-0-null-null-null-null"));

    uri = httpCreateUri("http", 0);
    assert(checkUri(uri, "null-http-0-null-null-null-null"));

    uri = httpCreateUri("https", 0);
    assert(checkUri(uri, "null-https-0-null-null-null-null"));
    
    uri = httpCreateUri("http://", 0);
    assert(checkUri(uri, "http-null-0-null-null-null-null"));

    uri = httpCreateUri("https://", 0);
    assert(checkUri(uri, "https-null-0-null-null-null-null"));

    uri = httpCreateUri("http://:8080/", 0);
    assert(checkUri(uri, "http-null-8080-/-null-null-null"));

    uri = httpCreateUri("http://:8080", 0);
    assert(checkUri(uri, "http-null-8080-null-null-null-null"));

    uri = httpCreateUri("http:///", 0);
    assert(checkUri(uri, "http-null-0-/-null-null-null"));

    uri = httpCreateUri("http://localhost", 0);
    assert(checkUri(uri, "http-localhost-0-null-null-null-null"));

    uri = httpCreateUri("http://localhost/", 0);
    assert(checkUri(uri, "http-localhost-0-/-null-null-null"));

    uri = httpCreateUri("http://[::]/", 0);
    assert(checkUri(uri, "http-::-0-/-null-null-null"));

    uri = httpCreateUri("http://[::]:8080", 0);
    assert(checkUri(uri, "http-::-8080-null-null-null-null"));

    uri = httpCreateUri("http://[::]:8080/", 0);
    assert(checkUri(uri, "http-::-8080-/-null-null-null"));

    uri = httpCreateUri("http://localhost/path", 0);
    assert(checkUri(uri, "http-localhost-0-/path-null-null-null"));

    uri = httpCreateUri("http://localhost/path.txt", 0);
    assert(checkUri(uri, "http-localhost-0-/path.txt-txt-null-null"));

    uri = httpCreateUri("http://localhost/path.txt?query", 0);
    assert(checkUri(uri, "http-localhost-0-/path.txt-txt-null-query"));

    uri = httpCreateUri("http://localhost/path.txt?query#ref", 0);
    assert(checkUri(uri, "http-localhost-0-/path.txt-txt-null-query#ref"));

    uri = httpCreateUri("http://localhost/path.txt#ref?query", 0);
    assert(checkUri(uri, "http-localhost-0-/path.txt-txt-ref-query"));

    uri = httpCreateUri("http://localhost/path.txt#ref/extra", 0);
    assert(checkUri(uri, "http-localhost-0-/path.txt-txt-ref/extra-null"));

    uri = httpCreateUri("http://localhost/path.txt#ref/extra?query", 0);
    assert(checkUri(uri, "http-localhost-0-/path.txt-txt-ref/extra-null"));

    uri = httpCreateUri(":4100", 0);
    assert(checkUri(uri, "null-null-4100-null-null-null-null"));

    uri = httpCreateUri(":4100/path", 0);
    assert(checkUri(uri, "null-null-4100-/path-null-null-null"));

    //  MOB - should this routine reject invalid URLs Uri(spaces, illegal chars ..., 0)
    uri = httpCreateUri("http:/", 0);
    assert(checkUri(uri, "null-http-0-/-null-null-null"));

    uri = httpCreateUri("http://:/", 0);
    assert(checkUri(uri, "http-null-0-/-null-null-null"));

    uri = httpCreateUri("http://:", 0);
    assert(checkUri(uri, "http-null-0-null-null-null-null"));

    uri = httpCreateUri("http://localhost:", 0);
    assert(checkUri(uri, "http-localhost-0-null-null-null-null"));
    
    uri = httpCreateUri("http://local#host/", 0);
    assert(checkUri(uri, "http-local-0-null-null-host/-null"));

    uri = httpCreateUri("http://local?host/", 0);
    assert(checkUri(uri, "http-local-0-null-null-null-host/"));

    uri = httpCreateUri("http://local host/", 0);
    assert(checkUri(uri, "http-local host-0-/-null-null-null"));

    uri = httpCreateUri("http://localhost/long path", 0);
    assert(checkUri(uri, "http-localhost-0-/long path-null-null-null"));
#endif

    uri = httpCreateUri("", HTTP_COMPLETE_URI);
    assert(checkUri(uri, "http-localhost-80-/-null-null-null"));
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
