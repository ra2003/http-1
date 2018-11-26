/*
    hpack.c - Http/2 header packing.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

#if ME_HTTP_HTTP2
/*********************************** Code *************************************/

static cchar *staticStrings[] = {
    ":authority", NULL,
    ":method", "GET",
    ":method", "POST",
    ":path", "/",
    ":path", "/index.html",
    ":scheme", "http",
    ":scheme", "https",
    ":status", "200",
    ":status", "204",
    ":status", "206",
    ":status", "304",
    ":status", "400",
    ":status", "404",
    ":status", "500",
    "accept-charset", NULL,
    "accept-encoding", "gzip, deflate",
    "accept-language", NULL,
    "accept-ranges", NULL,
    "accept", NULL,
    "access-control-allow-origin", NULL,
    "age", NULL,
    "allow", NULL,
    "authorization", NULL,
    "cache-control", NULL,
    "content-disposition", NULL,
    "content-encoding", NULL,
    "content-language", NULL,
    "content-length", NULL,
    "content-location", NULL,
    "content-range", NULL,
    "content-type", NULL,
    "cookie", NULL,
    "date", NULL,
    "etag", NULL,
    "expect", NULL,
    "expires", NULL,
    "from", NULL,
    "host", NULL,
    "if-match", NULL,
    "if-modified-since", NULL,
    "if-none-match", NULL,
    "if-range", NULL,
    "if-unmodified-since", NULL,
    "last-modified", NULL,
    "link", NULL,
    "location", NULL,
    "max-forwards", NULL,
    "proxy-authenticate", NULL,
    "proxy-authorization", NULL,
    "range", NULL,
    "referer", NULL,
    "refresh", NULL,
    "retry-after", NULL,
    "server", NULL,
    "set-cookie", NULL,
    "strict-transport-security", NULL,
    "transfer-encoding", NULL,
    "user-agent", NULL,
    "vary", NULL,
    "via", NULL,
    "www-authenticate", NULL,
    NULL, NULL
};

#define HTTP2_STATIC_TABLE_ENTRIES ((sizeof(staticStrings) / sizeof(char*) / 2) - 1)

/*********************************** Code *************************************/

PUBLIC void httpCreatePackedHeaders()
{
    cchar   **cp;

    /*
        Create the static table of common headers
     */
    HTTP->staticHeaders = mprCreateList(HTTP2_STATIC_TABLE_ENTRIES, 0);
    for (cp = staticStrings; *cp; cp += 2) {
        mprAddItem(HTTP->staticHeaders, mprCreateKeyPair(cp[0], cp[1], 0));
    }
}


/*
    Lookup a key/value in the HPACK header table.
    Look in the dynamic list first as it will contain most of the headers with values.
    Set *withValue if the value matches as well as the name.
    The dynamic table uses indexes after the static table.
 */
PUBLIC int httpLookupPackedHeader(HttpHeaderTable *headers, cchar *key, cchar *value, bool *withValue)
{
    MprKeyValue     *kp;
    int             next, onext;

    assert(headers);
    assert(key && *key);
    assert(value && *value);

    *withValue = 0;

    /*
        Prefer dynamic table as we can encode more values
     */
    for (ITERATE_ITEMS(headers->list, kp, next)) {
        if (strcmp(key, kp->key) == 0) {
            onext = next;
            do {
                if (smatch(kp->value, value) && smatch(kp->key, key)) {
                    *withValue = 1;
                    return next + HTTP2_STATIC_TABLE_ENTRIES;
                }
            } while ((kp = mprGetNextItem(headers->list, &next)) != 0);
            return onext + HTTP2_STATIC_TABLE_ENTRIES;
        }
    }

    for (ITERATE_ITEMS(HTTP->staticHeaders, kp, next)) {
        if (smatch(key, kp->key)) {
            if (value && kp->value) {
                onext = next;
                do {
                    if (smatch(kp->value, value) && smatch(kp->key, key)) {
                        *withValue = 1;
                        return next;
                    }
                } while ((kp = mprGetNextItem(HTTP->staticHeaders, &next)) != 0);
                return onext;
            } else {
                return next;
            }
        }
    }
    return 0;
}


/*
    Add a header to the dynamic table.
 */
PUBLIC int httpAddPackedHeader(HttpHeaderTable *headers, cchar *key, cchar *value)
{
    MprKeyValue     *kp;
    ssize           len;
    int             index;

    /*
        Make room for the new entry if required. Evict the oldest entries first.
     */
    len = slen(key) + slen(value) + HTTP2_HEADER_OVERHEAD;
    while ((headers->size + len) >= headers->max) {
        kp = mprPopItem(headers->list);
        headers->size -= (slen(kp->key) + slen(kp->value) + HTTP2_HEADER_OVERHEAD);
    }
    /*
        New entries are inserted at the start of the table and all existing entries shuffle down
     */
    if ((index = mprInsertItemAtPos(headers->list, 0, mprCreateKeyPair(key, value, 0))) < 0) {
        return MPR_ERR_MEMORY;
    }
    index += 1 + HTTP2_STATIC_TABLE_ENTRIES;
    headers->size += len;
    return index;
}


/*
    Get a header at a specific index.
 */
PUBLIC MprKeyValue *httpGetPackedHeader(HttpHeaderTable *headers, int index)
{
    if (index <= 0 || index > (1 + HTTP2_STATIC_TABLE_ENTRIES + mprGetListLength(headers->list))) {
        return 0;
    }
    if (--index < HTTP2_STATIC_TABLE_ENTRIES) {
        return mprGetItem(HTTP->staticHeaders, index);
    }
    index = index - HTTP2_STATIC_TABLE_ENTRIES;
    if (index >= mprGetListLength(headers->list)) {
        assert(index < mprGetListLength(headers->list));
        return 0;
    }
    return mprGetItem(headers->list, index);
}


/*
    Set a new maximum header table size. Evict oldest entries if currently over budget.
 */
PUBLIC int httpSetPackedHeadersMax(HttpHeaderTable *headers, int max)
{
    MprKeyValue     *kp;

    if (max < 0) {
        return MPR_ERR_BAD_ARGS;
    }
    if (max >= headers->max) {
        headers->max = max;
        return 0;
    }
    headers->max = max;
    while (headers->size >= max) {
        kp = mprPopItem(headers->list);
        headers->size -= (slen(kp->key) + slen(kp->value) + HTTP2_HEADER_OVERHEAD);
    }
    return 0;
}
#endif /* ME_HTTP_HTTP2 */

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
