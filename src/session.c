/**
    session.c - Session data storage

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "http.h"

/********************************** Forwards  *********************************/

static cchar *createSecurityToken(HttpConn *conn);
static void manageSession(HttpSession *sp, int flags);

/************************************* Code ***********************************/
/*
    Allocate a http session state object. This keeps a local hash for session state items.
    This is written via httpWriteSession to the backend session state store.
 */
static HttpSession *allocSessionObj(HttpConn *conn, cchar *id, cchar *data)
{
    HttpSession *sp;

    assert(conn);
    assert(id && *id);
    assert(conn->http);
    assert(conn->http->sessionCache);

    if ((sp = mprAllocObj(HttpSession, manageSession)) == 0) {
        return 0;
    }
    sp->lifespan = conn->limits->sessionTimeout;
    sp->id = sclone(id);
    sp->cache = conn->http->sessionCache;
    if (data) {
        sp->data = mprDeserialize(data);
    }
    if (!sp->data) {
        sp->data = mprCreateHash(ME_MAX_SESSION_HASH, 0);
    }
    return sp;
}


PUBLIC bool httpLookupSessionID(cchar *id)
{
    Http    *http;

    http = MPR->httpService;
    assert(http);
    return mprLookupCache(http->sessionCache, id, 0, 0) != 0;
}


/*
    Public API to create or re-create a session. Always returns with a new session store.
 */
PUBLIC HttpSession *httpCreateSession(HttpConn *conn)
{
    httpDestroySession(conn);
    return httpGetSession(conn, 1);
}


PUBLIC void httpSetSessionNotify(MprCacheProc callback)
{
    Http        *http;

    http = MPR->httpService;
    mprSetCacheNotify(http->sessionCache, callback);
}


PUBLIC void httpDestroySession(HttpConn *conn)
{
    Http        *http;
    HttpRx      *rx;
    HttpSession *sp;
    cchar       *cookie;

    http = conn->http;
    rx = conn->rx;
    assert(http);

    lock(http);
    if ((sp = httpGetSession(conn, 0)) != 0) {
        cookie = rx->route->cookie ? rx->route->cookie : HTTP_SESSION_COOKIE;
        httpRemoveCookie(conn, cookie);
        mprExpireCacheItem(sp->cache, sp->id, 0);
        sp->id = 0;
        rx->session = 0;
    }
    rx->sessionProbed = 0;
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
    Optionally create if "create" is true. Will not re-create.
 */
PUBLIC HttpSession *httpGetSession(HttpConn *conn, int create)
{
    Http        *http;
    HttpRx      *rx;
    cchar       *cookie, *data, *id;
    static int  seqno = 0;
    int         flags, thisSeqno;

    assert(conn);
    rx = conn->rx;
    http = conn->http;
    assert(rx);

    if (!rx->session) {
        if ((id = httpGetSessionID(conn)) != 0) {
            if ((data = mprReadCache(conn->http->sessionCache, id, 0, 0)) != 0) {
                rx->session = allocSessionObj(conn, id, data);
            }
        }
        if (!rx->session && create) {
            lock(http);
            thisSeqno = seqno++;
            id = sfmt("%08x%08x%d", PTOI(conn->data) + PTOI(conn), (int) mprGetTicks(), thisSeqno);
            id = mprGetMD5WithPrefix(id, slen(id), "-http.session-");
            id = sfmt("%d%s", thisSeqno, mprGetMD5WithPrefix(id, slen(id), "::http.session::"));

            mprGetCacheStats(http->sessionCache, &http->activeSessions, NULL);
            if (http->activeSessions >= conn->limits->sessionMax) {
                unlock(http);
                httpLimitError(conn, HTTP_CODE_SERVICE_UNAVAILABLE, 
                    "Too many sessions %d/%d", http->activeSessions, conn->limits->sessionMax);
                return 0;
            }
            unlock(http);

            rx->session = allocSessionObj(conn, id, NULL);
            flags = (rx->route->flags & HTTP_ROUTE_VISIBLE_SESSION) ? 0 : HTTP_COOKIE_HTTP;
            cookie = rx->route->cookie ? rx->route->cookie : HTTP_SESSION_COOKIE;
            httpSetCookie(conn, cookie, rx->session->id, "/", NULL, rx->session->lifespan, flags);
            httpTrace(conn, "context", "Create new session cookie", "cookie=%s, session=%s", cookie, rx->session->id);

            if ((rx->route->flags & HTTP_ROUTE_XSRF) && rx->securityToken) {
                httpSetSessionVar(conn, ME_XSRF_COOKIE, rx->securityToken);
            }
        }
    }
    return rx->session;
}


PUBLIC MprHash *httpGetSessionObj(HttpConn *conn, cchar *key)
{
    HttpSession *sp;
    MprKey      *kp;

    assert(conn);
    assert(key && *key);

    if ((sp = httpGetSession(conn, 0)) != 0) {
        if ((kp = mprLookupKeyEntry(sp->data, key)) != 0) {
            return mprDeserialize(kp->data);
        }
    }
    return 0;
}


PUBLIC cchar *httpGetSessionVar(HttpConn *conn, cchar *key, cchar *defaultValue)
{
    HttpSession *sp;
    MprKey      *kp;
    cchar       *result;

    assert(conn);
    assert(key && *key);

    result = 0;
    if ((sp = httpGetSession(conn, 0)) != 0) {
        if ((kp = mprLookupKeyEntry(sp->data, key)) != 0) {
            if (kp->type == MPR_JSON_OBJ) {
                /* Wrong type */
                mprDebug("http session", 0, "Session var is an object");
                return defaultValue;
            } else {
                result = kp->data;
            }
        }
    }
    return result ? result : defaultValue;
}


PUBLIC int httpSetSessionObj(HttpConn *conn, cchar *key, MprHash *obj)
{
    HttpSession *sp;

    assert(conn);
    assert(key && *key);

    if ((sp = httpGetSession(conn, 1)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    if (obj == 0) {
        httpRemoveSessionVar(conn, key);
    } else {
        mprAddKey(sp->data, key, mprSerialize(obj, 0));
    }
    sp->dirty = 1;
    return 0;
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
        return MPR_ERR_CANT_FIND;
    }
    if (value == 0) {
        httpRemoveSessionVar(conn, key);
    } else {
        mprAddKey(sp->data, key, sclone(value));
    }
    sp->dirty = 1;
    return 0;
}


PUBLIC int httpSetSessionLink(HttpConn *conn, void *link)
{
    HttpSession  *sp;

    assert(conn);

    if ((sp = httpGetSession(conn, 1)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    mprSetCacheLink(sp->cache, sp->id, link);
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
    sp->dirty = 1;
    return mprRemoveKey(sp->data, key);
}


PUBLIC int httpWriteSession(HttpConn *conn)
{
    HttpSession     *sp;

    if ((sp = conn->rx->session) != 0) {
        if (sp->dirty) {
            if (mprWriteCache(sp->cache, sp->id, mprSerialize(sp->data, 0), 0, sp->lifespan, 0, MPR_CACHE_SET) == 0) {
                mprLog("error http session", 0, "Cannot persist session cache");
                return MPR_ERR_CANT_WRITE;
            }
            sp->dirty = 0;
        }
    }
    return 0;
}


PUBLIC cchar *httpGetSessionID(HttpConn *conn)
{
    HttpRx  *rx;
    cchar   *cookie;

    assert(conn);
    rx = conn->rx;
    assert(rx);

    if (rx->session) {
        assert(rx->session->id);
        assert(*rx->session->id);
        return rx->session->id;
    }
    if (rx->sessionProbed) {
        return 0;
    }
    rx->sessionProbed = 1;
    cookie = rx->route->cookie ? rx->route->cookie : HTTP_SESSION_COOKIE;
    return httpGetCookie(conn, cookie);
}


/*
    Create a security token to use to mitiate CSRF threats. Security tokens are expected to be sent with POST requests to 
    verify the request is not being forged.

    Note: the HttpSession API prevents session hijacking by pairing with the client IP
 */
static cchar *createSecurityToken(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (!rx->securityToken) {
        rx->securityToken = mprGetRandomString(32);
    }
    return rx->securityToken;
}


/*
    Get the security token from the session. Create one if one does not exist. Store the token in session store.
    Recreate if required.
 */
PUBLIC cchar *httpGetSecurityToken(HttpConn *conn, bool recreate)
{
    HttpRx      *rx;

    rx = conn->rx;

    if (recreate) {
        rx->securityToken = 0;
    } else {
        rx->securityToken = (char*) httpGetSessionVar(conn, ME_XSRF_COOKIE, 0);
    }
    if (rx->securityToken == 0) {
        createSecurityToken(conn);
        httpSetSessionVar(conn, ME_XSRF_COOKIE, rx->securityToken);
    }
    return rx->securityToken;
}


/*
    Add the security token to a XSRF cookie and response header
    Set recreate to true to force a recreation of the token.
 */
PUBLIC int httpAddSecurityToken(HttpConn *conn, bool recreate) 
{
    cchar   *securityToken;

    securityToken = httpGetSecurityToken(conn, recreate);
    httpSetCookie(conn, ME_XSRF_COOKIE, securityToken, "/", NULL,  0, 0);
    httpSetHeader(conn, ME_XSRF_HEADER, securityToken);
    return 0;
}


/*
    Check the security token with the request. This must match the last generated token stored in the session state.
    It is expected the client will set the X-XSRF-TOKEN header with the token.
 */
PUBLIC bool httpCheckSecurityToken(HttpConn *conn) 
{
    cchar   *requestToken, *sessionToken;

    if ((sessionToken = httpGetSessionVar(conn, ME_XSRF_COOKIE, 0)) != 0) {
        requestToken = httpGetHeader(conn, ME_XSRF_HEADER);
        if (!requestToken) {
            requestToken = httpGetParam(conn, ME_XSRF_PARAM, 0);
            if (!requestToken) {
                httpTrace(conn, "error", "Missing security token in request", 0, 0);
            }
        }
        if (!smatch(sessionToken, requestToken)) {
            /*
                Potential CSRF attack. Deny request. Re-create a new security token so legitimate clients can retry.
             */
            httpTrace(conn, "error", "Security token in request does not match session token", "xsrf=%s, sessionXsrf=%s",
                requestToken, sessionToken);
            httpAddSecurityToken(conn, 1);
            return 0;
        }
    }
    return 1;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

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
