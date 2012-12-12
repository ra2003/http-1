/**
    httpSession.c - Session data storage

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "http.h"

/********************************** Forwards  *********************************/

static char *makeKey(HttpSession *sp, cchar *key);
static char *makeSessionID(HttpConn *conn);
static void manageSession(HttpSession *sp, int flags);

/************************************* Code ***********************************/

PUBLIC HttpSession *httpAllocSession(HttpConn *conn, cchar *id, MprTicks lifespan)
{
    HttpSession *sp;

    assure(conn);
#if UNUSED && FUTURE
    Http        *http;
    http = conn->http;

    //  OPT less contentions mutex
    lock(http);
    if (http->activeSessions >= conn->limits->sessionMax) {
        httpError(conn, HTTP_CODE_SERVICE_UNAVAILABLE,
            "Too many sessions %d/%d", http->activeSessions, conn->limits->sessionMax);
        unlock(http);
        return 0;
    }
    http->activeSessions++;
    unlock(http);
#endif

    if ((sp = mprAllocObj(HttpSession, manageSession)) == 0) {
        return 0;
    }
    mprSetName(sp, "session");
    sp->lifespan = lifespan;
    if (id == 0) {
        id = makeSessionID(conn);
    }
    sp->id = sclone(id);
    sp->cache = conn->http->sessionCache;
    sp->conn = conn;
    return sp;
}


PUBLIC void httpDestroySession(HttpSession *sp)
{
    Http    *http;

    http = MPR->httpService;

    assure(sp);
    //  OPT less contentions mutex
    lock(http);
    http->activeSessions--;
    assure(http->activeSessions >= 0);
    unlock(http);
    sp->id = 0;
}


static void manageSession(HttpSession *sp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(sp->id);
        mprMark(sp->cache);
    }
}


PUBLIC HttpSession *httpCreateSession(HttpConn *conn)
{
    return httpGetSession(conn, 1);
}


PUBLIC HttpSession *httpGetSession(HttpConn *conn, int create)
{
    HttpRx      *rx;
    char        *id;

    assure(conn);
    rx = conn->rx;
    assure(rx);
    if (rx->session || !conn) {
        return rx->session;
    }
    id = httpGetSessionID(conn);
    if (id || create) {
        rx->session = httpAllocSession(conn, id, conn->limits->sessionTimeout);
        if (rx->session && !id) {
            httpSetCookie(conn, HTTP_SESSION_COOKIE, rx->session->id, "/", NULL, 0, 0);
        }
    }
    return rx->session;
}


PUBLIC MprHash *httpGetSessionObj(HttpConn *conn, cchar *key)
{
    cchar   *str;

    if ((str = httpGetSessionVar(conn, key, 0)) != 0 && *str) {
        assure(*str == '{');
        return mprDeserialize(str);
    }
    return 0;
}


PUBLIC cchar *httpGetSessionVar(HttpConn *conn, cchar *key, cchar *defaultValue)
{
    HttpSession  *sp;
    cchar       *result;

    assure(conn);
    assure(key && *key);

    result = 0;
    if ((sp = httpGetSession(conn, 0)) != 0) {
        result = mprReadCache(sp->cache, makeKey(sp, key), 0, 0);
    }
    return result ? result : defaultValue;
}


PUBLIC int httpSetSessionObj(HttpConn *conn, cchar *key, MprHash *obj)
{
    httpSetSessionVar(conn, key, mprSerialize(obj, 0));
    return 0;
}


PUBLIC int httpSetSessionVar(HttpConn *conn, cchar *key, cchar *value)
{
    HttpSession  *sp;

    assure(conn);
    assure(key && *key);
    assure(value);

    if ((sp = httpGetSession(conn, 1)) == 0) {
        return 0;
    }
    if (mprWriteCache(sp->cache, makeKey(sp, key), value, 0, sp->lifespan, 0, MPR_CACHE_SET) == 0) {
        return MPR_ERR_CANT_WRITE;
    }
    return 0;
}


PUBLIC int httpRemoveSessionVar(HttpConn *conn, cchar *key)
{
    HttpSession  *sp;

    assure(conn);
    assure(key && *key);

    if ((sp = httpGetSession(conn, 1)) == 0) {
        return 0;
    }
    return mprRemoveCache(sp->cache, makeKey(sp, key)) ? 0 : MPR_ERR_CANT_FIND;
}


PUBLIC char *httpGetSessionID(HttpConn *conn)
{
    HttpRx  *rx;
    cchar   *cookies, *cookie;
    char    *cp, *value;
    int     quoted;

    assure(conn);
    rx = conn->rx;
    assure(rx);

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


static char *makeSessionID(HttpConn *conn)
{
    char        idBuf[64];
    static int  nextSession = 0;

    assure(conn);

    /* Thread race here on nextSession++ not critical */
    fmt(idBuf, sizeof(idBuf), "%08x%08x%d", PTOI(conn->data) + PTOI(conn), (int) mprGetTime(), nextSession++);
    return mprGetMD5WithPrefix(idBuf, sizeof(idBuf), "::http.session::");
}


/*
    Make a session cache key. This includes the session cookie, the connection IP address and the variable key
    The IP address is added to prevent hijacking.
    Use ./configure --set sessionWithoutIp=true to create sessions without encoding the client IP
 */
static char *makeKey(HttpSession *sp, cchar *key)
{
#if BIT_SESSION_WITHOUT_IP
    return sfmt("session-%s-%s", sp->id, key);
#else
    return sfmt("session-%s-%s-%s", sp->id, sp->conn->ip, key);
#endif
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

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
