/**
    session.c - Session data storage

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "http.h"

/********************************** Forwards  *********************************/

static cchar *createSecurityToken(HttpStream *stream);
static void manageSession(HttpSession *sp, int flags);

/************************************* Code ***********************************/
/*
    Allocate a http session state object. This keeps a local hash for session state items.
    This is written via httpWriteSession to the backend session state store.
 */
static HttpSession *allocSessionObj(HttpStream *stream, cchar *id, cchar *data)
{
    HttpSession *sp;

    assert(stream);
    assert(id && *id);
    assert(stream->http);
    assert(stream->http->sessionCache);

    if ((sp = mprAllocObj(HttpSession, manageSession)) == 0) {
        return 0;
    }
    sp->lifespan = stream->limits->sessionTimeout;
    sp->id = sclone(id);
    sp->cache = stream->http->sessionCache;
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
    return mprLookupCache(HTTP->sessionCache, id, 0, 0) != 0;
}


/*
    Public API to create or re-create a session. Always returns with a new session store.
 */
PUBLIC HttpSession *httpCreateSession(HttpStream *stream)
{
    httpDestroySession(stream);
    return httpGetSession(stream, 1);
}


PUBLIC void httpSetSessionNotify(MprCacheProc callback)
{
    mprSetCacheNotify(HTTP->sessionCache, callback);
}


PUBLIC void httpDestroySession(HttpStream *stream)
{
    Http        *http;
    HttpRx      *rx;
    HttpSession *sp;
    cchar       *cookie;

    http = stream->http;
    rx = stream->rx;
    assert(http);

    lock(http);
    if ((sp = httpGetSession(stream, 0)) != 0) {
        cookie = rx->route->cookie ? rx->route->cookie : HTTP_SESSION_COOKIE;
        httpRemoveCookie(stream, cookie);
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
PUBLIC HttpSession *httpGetSession(HttpStream *stream, int create)
{
    Http        *http;
    HttpRx      *rx;
    HttpRoute   *route;
    MprTicks    lifespan;
    cchar       *cookie, *data, *id, *url;
    static int  seqno = 0;
    int         flags, thisSeqno, activeSessions;

    assert(stream);
    rx = stream->rx;
    route = rx->route;
    http = stream->http;
    assert(rx);

    if (!rx->session) {
        if ((id = httpGetSessionID(stream)) != 0) {
            if ((data = mprReadCache(stream->http->sessionCache, id, 0, 0)) != 0) {
                rx->session = allocSessionObj(stream, id, data);
                rx->traceId = sfmt("%d-%d-%d-%d", stream->net->address->seqno, rx->session->seqno, stream->net->seqno, rx->seqno);
            }
        }
        if (!rx->session && create) {
            lock(http);
            thisSeqno = ++seqno;
            id = sfmt("%08x%08x%d", PTOI(stream->data) + PTOI(stream), (int) mprGetTicks(), thisSeqno);
            id = mprGetMD5WithPrefix(id, slen(id), "-http.session-");
            id = sfmt("%d%s", thisSeqno, mprGetMD5WithPrefix(id, slen(id), "::http.session::"));

            mprGetCacheStats(http->sessionCache, &activeSessions, NULL);
            if (activeSessions >= stream->limits->sessionMax) {
                unlock(http);
                httpLimitError(stream, HTTP_CODE_SERVICE_UNAVAILABLE,
                    "Too many sessions %d/%d", activeSessions, stream->limits->sessionMax);
                return 0;
            }
            unlock(http);

            rx->session = allocSessionObj(stream, id, NULL);
            rx->traceId = sfmt("%d-%d-%d-%d", stream->net->address->seqno, rx->session->seqno, stream->net->seqno, rx->seqno);
            flags = (route->flags & HTTP_ROUTE_VISIBLE_SESSION) ? 0 : HTTP_COOKIE_HTTP;
            if (stream->secure) {
                flags |= HTTP_COOKIE_SECURE;
            }
            if (route->flags & HTTP_ROUTE_LAX_COOKIE) {
                flags |= HTTP_COOKIE_SAME_LAX;
            } else if (route->flags & HTTP_ROUTE_STRICT_COOKIE) {
                flags |= HTTP_COOKIE_SAME_STRICT;
            }
            cookie = route->cookie ? route->cookie : HTTP_SESSION_COOKIE;
            lifespan = (route->flags & HTTP_ROUTE_PERSIST_COOKIE) ? rx->session->lifespan : 0;
            url = (route->prefix && *route->prefix) ? route->prefix : "/";
            httpSetCookie(stream, cookie, rx->session->id, url, NULL, lifespan, flags);
            httpLog(stream->trace, "session.create", "context", "cookie:'%s', session:'%s'", cookie, rx->session->id);

            if ((route->flags & HTTP_ROUTE_XSRF) && rx->securityToken) {
                httpSetSessionVar(stream, ME_XSRF_COOKIE, rx->securityToken);
            }
        }
    }
    return rx->session;
}


PUBLIC MprHash *httpGetSessionObj(HttpStream *stream, cchar *key)
{
    HttpSession *sp;
    MprKey      *kp;

    assert(stream);
    assert(key && *key);

    if ((sp = httpGetSession(stream, 0)) != 0) {
        if ((kp = mprLookupKeyEntry(sp->data, key)) != 0) {
            return mprDeserialize(kp->data);
        }
    }
    return 0;
}


PUBLIC cchar *httpGetSessionVar(HttpStream *stream, cchar *key, cchar *defaultValue)
{
    HttpSession *sp;
    MprKey      *kp;
    cchar       *result;

    assert(stream);
    assert(key && *key);

    result = 0;
    if ((sp = httpGetSession(stream, 0)) != 0) {
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


PUBLIC int httpSetSessionObj(HttpStream *stream, cchar *key, MprHash *obj)
{
    HttpSession *sp;

    assert(stream);
    assert(key && *key);

    if ((sp = httpGetSession(stream, 1)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    if (obj == 0) {
        httpRemoveSessionVar(stream, key);
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
PUBLIC int httpSetSessionVar(HttpStream *stream, cchar *key, cchar *value)
{
    HttpSession  *sp;

    assert(stream);
    assert(key && *key);

    if ((sp = httpGetSession(stream, 1)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    if (value == 0) {
        httpRemoveSessionVar(stream, key);
    } else {
        mprAddKey(sp->data, key, sclone(value));
    }
    sp->dirty = 1;
    return 0;
}


PUBLIC int httpSetSessionLink(HttpStream *stream, void *link)
{
    HttpSession  *sp;

    assert(stream);

    if ((sp = httpGetSession(stream, 1)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    mprSetCacheLink(sp->cache, sp->id, link);
    return 0;
}


PUBLIC int httpRemoveSessionVar(HttpStream *stream, cchar *key)
{
    HttpSession  *sp;

    assert(stream);
    assert(key && *key);

    if ((sp = httpGetSession(stream, 0)) == 0) {
        return 0;
    }
    sp->dirty = 1;
    return mprRemoveKey(sp->data, key);
}


PUBLIC int httpWriteSession(HttpStream *stream)
{
    HttpSession     *sp;

    if ((sp = stream->rx->session) != 0) {
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


PUBLIC cchar *httpGetSessionID(HttpStream *stream)
{
    HttpRx  *rx;
    cchar   *cookie;

    assert(stream);
    rx = stream->rx;
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
    return httpGetCookie(stream, cookie);
}


/*
    Create a security token to use to mitiate CSRF threats. Security tokens are expected to be sent with POST requests to
    verify the request is not being forged.

    Note: the HttpSession API prevents session hijacking by pairing with the client IP
 */
static cchar *createSecurityToken(HttpStream *stream)
{
    HttpRx      *rx;

    rx = stream->rx;
    if (!rx->securityToken) {
        rx->securityToken = mprGetRandomString(32);
    }
    return rx->securityToken;
}


/*
    Get the security token from the session. Create one if one does not exist. Store the token in session store.
    Recreate if required.
 */
PUBLIC cchar *httpGetSecurityToken(HttpStream *stream, bool recreate)
{
    HttpRx      *rx;

    rx = stream->rx;

    if (recreate) {
        rx->securityToken = 0;
    } else {
        rx->securityToken = (char*) httpGetSessionVar(stream, ME_XSRF_COOKIE, 0);
    }
    if (rx->securityToken == 0) {
        createSecurityToken(stream);
        httpSetSessionVar(stream, ME_XSRF_COOKIE, rx->securityToken);
    }
    return rx->securityToken;
}


/*
    Add the security token to a XSRF cookie and response header
    Set recreate to true to force a recreation of the token.
 */
PUBLIC int httpAddSecurityToken(HttpStream *stream, bool recreate)
{
    HttpRoute   *route;
    cchar       *securityToken, *url;
    int         flags;

    route = stream->rx->route;
    securityToken = httpGetSecurityToken(stream, recreate);
    url = (route->prefix && *route->prefix) ? route->prefix : "/";
    flags = (route->flags & HTTP_ROUTE_VISIBLE_SESSION) ? 0 : HTTP_COOKIE_HTTP;
    if (stream->secure) {
        flags |= HTTP_COOKIE_SECURE;
    }
    httpSetCookie(stream, ME_XSRF_COOKIE, securityToken, url, NULL,  0, flags);
    httpSetHeaderString(stream, ME_XSRF_HEADER, securityToken);
    return 0;
}


/*
    Check the security token with the request. This must match the last generated token stored in the session state.
    It is expected the client will set the X-XSRF-TOKEN header with the token.
 */
PUBLIC bool httpCheckSecurityToken(HttpStream *stream)
{
    cchar   *requestToken, *sessionToken;

    if ((sessionToken = httpGetSessionVar(stream, ME_XSRF_COOKIE, 0)) != 0) {
        requestToken = httpGetHeader(stream, ME_XSRF_HEADER);
        if (!requestToken) {
            requestToken = httpGetParam(stream, ME_XSRF_PARAM, 0);
            if (!requestToken) {
                httpLog(stream->trace, "session.xsrf.error", "error", "msg:'Missing security token in request'");
            }
        }
        if (!smatch(sessionToken, requestToken)) {
            /*
                Potential CSRF attack. Deny request. Re-create a new security token so legitimate clients can retry.
             */
            httpLog(stream->trace, "session.xsrf.error", "error",
                "msg:'Security token in request does not match session token',xsrf:'%s',sessionXsrf:'%s'",
                requestToken, sessionToken);
            httpAddSecurityToken(stream, 1);
            return 0;
        }
    }
    return 1;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
