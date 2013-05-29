/**
    session.c - Session data storage

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "http.h"

/********************************** Forwards  *********************************/

static void manageSession(HttpSession *sp, int flags);

/************************************* Code ***********************************/
/*
    Allocate a http session state object. This keeps a local hash for session state items.
    This is written via httpWriteSession to the backend session state store.
 */
static HttpSession *allocSession(HttpConn *conn, cchar *id, cchar *data)
{
    HttpSession *sp;

    assert(conn);
    assert(id && *id);

    if ((sp = mprAllocObj(HttpSession, manageSession)) == 0) {
        return 0;
    }
    sp->lifespan = conn->limits->sessionTimeout;
    sp->id = sclone(id);
    sp->cache = conn->http->sessionCache;
#if UNUSED
    sp->conn = conn;
#endif
    if (data) {
        sp->data = mprDeserialize(data);
    } else {
        sp->data = mprCreateHash(BIT_MAX_SESSION_HASH, 0);
    }
    return sp;
}


/*
    Create a new session. This generates a new session ID.
 */
static HttpSession *createSession(HttpConn *conn)
{
    Http            *http;
    char            *id;
    static int      nextSession = 0;

    assert(conn);
    http = conn->http;

    /* 
        Thread race here on nextSession++ not critical 
     */
    id = sfmt("%08x%08x%d", PTOI(conn->data) + PTOI(conn), (int) mprGetTicks(), nextSession++);
    id = mprGetMD5WithPrefix(id, slen(id), "::http.session::");

    lock(http);
    mprGetCacheStats(conn->http->sessionCache, &http->activeSessions, NULL);
    if (http->activeSessions >= conn->limits->sessionMax) {
        unlock(http);
        httpError(conn, HTTP_CODE_SERVICE_UNAVAILABLE, "Too many sessions %d/%d", http->activeSessions, 
            conn->limits->sessionMax);
        return 0;
    }
    unlock(http);

    return allocSession(conn, id, NULL);
}


static HttpSession *lookupSession(HttpConn *conn)
{
    cchar   *data, *id;

    if ((id = httpGetSessionID(conn)) == 0) {
        return 0;
    }
    if ((data = mprReadCache(conn->http->sessionCache, id, 0, 0)) == 0) {
        return 0;
    }
    return allocSession(conn, id, data);
}


/*
    Public API to create or re-create a session. Always returns with a new session store.
 */
PUBLIC HttpSession *httpCreateSession(HttpConn *conn)
{
    httpDestroySession(conn);
    return httpGetSession(conn, 1);
}


/*
    Destroy the session
 */
PUBLIC void httpDestroySession(HttpConn *conn)
{
    Http        *http;
    HttpSession *sp;

    http = conn->http;
    lock(http);
    if ((sp = httpGetSession(conn, 0)) != 0) {
        httpRemoveCookie(conn, HTTP_SESSION_COOKIE);
        mprExpireCache(sp->cache, sp->id, 0);
        sp->id = 0;
        http->activeSessions--;
        assert(http->activeSessions >= 0);
        conn->rx->session = 0;
    }
    conn->rx->sessionProbed = 0;
    unlock(http);
}


static void manageSession(HttpSession *sp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(sp->id);
        mprMark(sp->cache);
        mprMark(sp->data);
    }
}


/*
    Get the session. Optionally create if "create" is true. Will not re-create.
 */
PUBLIC HttpSession *httpGetSession(HttpConn *conn, int create)
{
    HttpRx      *rx;

    assert(conn);
    rx = conn->rx;
    assert(rx);

    if (!rx->session) {
        if ((rx->session = lookupSession(conn)) == 0 && create) {
            /*
                If forced create or we have a session-state cookie, then allocate a session object to manage the state.
                NOTE: the session state for this ID may already exist if data has been written to the session.
             */
            if ((rx->session = createSession(conn)) != 0) {
                httpSetCookie(conn, HTTP_SESSION_COOKIE, rx->session->id, "/", NULL, 0, 0);
            }
        }
    }
    return rx->session;
}


PUBLIC MprHash *httpGetSessionObj(HttpConn *conn, cchar *key)
{
    cchar   *str;

    if ((str = httpGetSessionVar(conn, key, 0)) != 0 && *str) {
        assert(*str == '{');
        return mprDeserialize(str);
    }
    return 0;
}


PUBLIC cchar *httpGetSessionVar(HttpConn *conn, cchar *key, cchar *defaultValue)
{
    HttpSession  *sp;
    cchar       *result;

    assert(conn);
    assert(key && *key);

    result = 0;
    if ((sp = httpGetSession(conn, 0)) != 0) {
        result = mprLookupKey(sp->data, key);
    }
    return result ? result : defaultValue;
}


PUBLIC int httpSetSessionObj(HttpConn *conn, cchar *key, MprHash *obj)
{
    return httpSetSessionVar(conn, key, mprSerialize(obj, 0));
}


/*
    Set a session variable. This will create the session store if it does not already exist.
    Note: If the headers have been emitted, the chance to set a cookie header has passed. So this value will go
    into a session that will be lost. Solution is for apps to create the session first.
    Value of null means remove the session.
 */
PUBLIC int httpSetSessionVar(HttpConn *conn, cchar *key, cchar *value)
{
    HttpSession  *sp;

    assert(conn);
    assert(key && *key);

    if ((sp = httpGetSession(conn, 1)) == 0) {
        return 0;
    }
    if (value == 0) {
        httpRemoveSessionVar(conn, key);
    } else {
        mprAddKey(sp->data, key, value);
    }
    return 0;
}


PUBLIC int httpRemoveSessionVar(HttpConn *conn, cchar *key)
{
    HttpSession  *sp;

    assert(conn);
    assert(key && *key);

    if ((sp = httpGetSession(conn, 0)) == 0) {
        return 0;
    }
    return mprRemoveKey(sp->data, key);
}


PUBLIC int httpWriteSession(HttpConn *conn)
{
    HttpSession     *sp;

    if ((sp = conn->rx->session) != 0) {
        if (mprWriteCache(sp->cache, sp->id, mprSerialize(sp->data, 0), 0, sp->lifespan, 0, MPR_CACHE_SET) == 0) {
            mprError("Cannot persist session cache");
            return MPR_ERR_CANT_WRITE;
        }
    }
    return 0;
}


PUBLIC char *httpGetSessionID(HttpConn *conn)
{
    HttpRx  *rx;
    cchar   *cookies, *cookie;
    char    *cp, *value;
    int     quoted;

    assert(conn);
    rx = conn->rx;
    assert(rx);

    if (rx->session) {
        return rx->session->id;
    }
    if (rx->sessionProbed) {
        return 0;
    }
    rx->sessionProbed = 1;
    cookies = httpGetCookies(conn);
    for (cookie = cookies; cookie && (value = strstr(cookie, HTTP_SESSION_COOKIE)) != 0; cookie = value) {
        value += strlen(HTTP_SESSION_COOKIE);
        while (isspace((uchar) *value) || *value == '=') {
            value++;
        }
        quoted = 0;
        if (*value == '"') {
            value++;
            quoted++;
        }
        for (cp = value; *cp; cp++) {
            if (quoted) {
                if (*cp == '"' && cp[-1] != '\\') {
                    break;
                }
            } else {
                if ((*cp == ',' || *cp == ';') && cp[-1] != '\\') {
                    break;
                }
            }
        }
        return snclone(value, cp - value);
    }
    return 0;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2013. All Rights Reserved.

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
