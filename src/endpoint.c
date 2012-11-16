/*
    endpoint.c -- Create and manage listening endpoints.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static HttpConn *acceptConn(MprSocket *sock, MprDispatcher *dispatcher, HttpEndpoint *endpoint);
static int manageEndpoint(HttpEndpoint *endpoint, int flags);
static int destroyEndpointConnections(HttpEndpoint *endpoint);

/************************************ Code ************************************/
/*
    Create a listening endpoint on ip:port. NOTE: ip may be empty which means bind to all addresses.
 */
PUBLIC HttpEndpoint *httpCreateEndpoint(cchar *ip, int port, MprDispatcher *dispatcher)
{
    HttpEndpoint    *endpoint;
    Http            *http;

    if ((endpoint = mprAllocObj(HttpEndpoint, manageEndpoint)) == 0) {
        return 0;
    }
    http = MPR->httpService;
    endpoint->http = http;
    endpoint->clientLoad = mprCreateHash(HTTP_CLIENTS_HASH, MPR_HASH_STATIC_VALUES);
    endpoint->async = 1;
    endpoint->http = MPR->httpService;
    endpoint->port = port;
    endpoint->ip = sclone(ip);
    endpoint->dispatcher = dispatcher;
    endpoint->hosts = mprCreateList(-1, 0);
    endpoint->mutex = mprCreateLock();
    httpAddEndpoint(http, endpoint);
    return endpoint;
}


PUBLIC void httpDestroyEndpoint(HttpEndpoint *endpoint)
{
    destroyEndpointConnections(endpoint);
    if (endpoint->sock) {
        mprCloseSocket(endpoint->sock, 0);
        endpoint->sock = 0;
    }
    httpRemoveEndpoint(MPR->httpService, endpoint);
}


static int manageEndpoint(HttpEndpoint *endpoint, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(endpoint->http);
        mprMark(endpoint->hosts);
        mprMark(endpoint->limits);
        mprMark(endpoint->clientLoad);
        mprMark(endpoint->ip);
        mprMark(endpoint->context);
        mprMark(endpoint->sock);
        mprMark(endpoint->dispatcher);
        mprMark(endpoint->ssl);
        mprMark(endpoint->mutex);

    } else if (flags & MPR_MANAGE_FREE) {
        httpDestroyEndpoint(endpoint);
    }
    return 0;
}


/*  
    Convenience function to create and configure a new endpoint without using a config file.
 */
PUBLIC HttpEndpoint *httpCreateConfiguredEndpoint(cchar *home, cchar *documents, cchar *ip, int port)
{
    Http            *http;
    HttpHost        *host;
    HttpEndpoint    *endpoint;
    HttpRoute       *route;

    http = MPR->httpService;

    if (ip == 0) {
        /*  
            If no IP:PORT specified, find the first endpoint
         */
        if ((endpoint = mprGetFirstItem(http->endpoints)) != 0) {
            ip = endpoint->ip;
            port = endpoint->port;
        } else {
            ip = "localhost";
            if (port <= 0) {
                port = HTTP_DEFAULT_PORT;
            }
            if ((endpoint = httpCreateEndpoint(ip, port, NULL)) == 0) {
                return 0;
            }
        }
    } else {
        if ((endpoint = httpCreateEndpoint(ip, port, NULL)) == 0) {
            return 0;
        }
    }
    if ((host = httpCreateHost(home)) == 0) {
        return 0;
    }
    if ((route = httpCreateRoute(host)) == 0) {
        return 0;
    }
    httpSetHostDefaultRoute(host, route);
    httpSetHostIpAddr(host, ip, port);
    httpAddHostToEndpoint(endpoint, host);
    httpSetRouteDir(route, documents);
    httpFinalizeRoute(route);
    return endpoint;
}


static int destroyEndpointConnections(HttpEndpoint *endpoint)
{
    HttpConn    *conn;
    Http        *http;
    int         next;

    http = endpoint->http;
    lock(http->connections);
    for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
        if (conn->endpoint == endpoint) {
            conn->endpoint = 0;
            httpDestroyConn(conn);
            next--;
        }
    }
    unlock(http->connections);
    return 0;
}


static bool validateEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;

    if ((host = mprGetFirstItem(endpoint->hosts)) == 0) {
        mprError("Missing host object on endpoint");
        return 0;
    }
    return 1;
}


PUBLIC int httpStartEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;
    cchar       *proto, *ip;
    int         next;

    if (!validateEndpoint(endpoint)) {
        return MPR_ERR_BAD_ARGS;
    }
    for (ITERATE_ITEMS(endpoint->hosts, host, next)) {
        httpStartHost(host);
    }
    if ((endpoint->sock = mprCreateSocket(endpoint->ssl)) == 0) {
        return MPR_ERR_MEMORY;
    }
    if (mprListenOnSocket(endpoint->sock, endpoint->ip, endpoint->port, MPR_SOCKET_NODELAY | MPR_SOCKET_THREAD) < 0) {
        mprError("Cannot open a socket on %s:%d", *endpoint->ip ? endpoint->ip : "*", endpoint->port);
        return MPR_ERR_CANT_OPEN;
    }
    if (endpoint->http->listenCallback && (endpoint->http->listenCallback)(endpoint) < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    if (endpoint->async && !endpoint->sock->handler) {
        mprAddSocketHandler(endpoint->sock, MPR_SOCKET_READABLE, endpoint->dispatcher, httpAcceptConn, endpoint, 
            (endpoint->dispatcher) ? 0 : MPR_WAIT_NEW_DISPATCHER);
    } else {
        mprSetSocketBlockingMode(endpoint->sock, 1);
    }
    proto = endpoint->ssl ? "HTTPS" : "HTTP ";
    ip = *endpoint->ip ? endpoint->ip : "*";
    if (mprIsSocketV6(endpoint->sock)) {
        mprLog(2, "Started %s service on \"[%s]:%d\"", proto, ip, endpoint->port);
    } else {
        mprLog(2, "Started %s service on \"%s:%d\"", proto, ip, endpoint->port);
    }
    return 0;
}


PUBLIC void httpStopEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;
    int         next;

    for (ITERATE_ITEMS(endpoint->hosts, host, next)) {
        httpStopHost(host);
    }
    if (endpoint->sock) {
        mprCloseSocket(endpoint->sock, 0);
        endpoint->sock = 0;
    }
}


/*
    OPT
 */
PUBLIC bool httpValidateLimits(HttpEndpoint *endpoint, int event, HttpConn *conn)
{
    HttpLimits      *limits;
    Http            *http;
    cchar           *action;
    int             count, level, dir;

    limits = conn->limits;
    dir = HTTP_TRACE_RX;
    action = "unknown";
    assure(conn->endpoint == endpoint);
    http = endpoint->http;

    lock(endpoint);

    switch (event) {
    case HTTP_VALIDATE_OPEN_CONN:
        /*
            This measures active client systems with unique IP addresses.
         */
        if (endpoint->clientCount >= limits->clientMax) {
            unlock(endpoint);
            /*  Abort connection */
            httpError(conn, HTTP_ABORT | HTTP_CODE_SERVICE_UNAVAILABLE, 
                "Too many concurrent clients %d/%d", endpoint->clientCount, limits->clientMax);
            return 0;
        }
        count = (int) PTOL(mprLookupKey(endpoint->clientLoad, conn->ip));
        mprAddKey(endpoint->clientLoad, conn->ip, ITOP(count + 1));
        endpoint->clientCount = (int) mprGetHashLength(endpoint->clientLoad);
        action = "open conn";
        dir = HTTP_TRACE_RX;
        break;

    case HTTP_VALIDATE_CLOSE_CONN:
        count = (int) PTOL(mprLookupKey(endpoint->clientLoad, conn->ip));
        if (count > 1) {
            mprAddKey(endpoint->clientLoad, conn->ip, ITOP(count - 1));
        } else {
            mprRemoveKey(endpoint->clientLoad, conn->ip);
        }
        endpoint->clientCount = (int) mprGetHashLength(endpoint->clientLoad);
        action = "close conn";
        dir = HTTP_TRACE_TX;
        break;
    
    case HTTP_VALIDATE_OPEN_REQUEST:
        assure(conn->rx);
        if (endpoint->requestCount >= limits->requestMax) {
            unlock(endpoint);
            httpError(conn, HTTP_CODE_SERVICE_UNAVAILABLE, "Server overloaded");
            mprLog(2, "Too many concurrent requests %d/%d", endpoint->requestCount, limits->requestMax);
            return 0;
        }
        endpoint->requestCount++;
        conn->rx->flags |= HTTP_LIMITS_OPENED;
        action = "open request";
        dir = HTTP_TRACE_RX;
        break;

    case HTTP_VALIDATE_CLOSE_REQUEST:
        if (conn->rx && conn->rx->flags & HTTP_LIMITS_OPENED) {
            /* Requests incremented only when conn->rx is assigned */
            endpoint->requestCount--;
            assure(endpoint->requestCount >= 0);
            action = "close request";
            dir = HTTP_TRACE_TX;
            conn->rx->flags &= ~HTTP_LIMITS_OPENED;
        }
        break;

    case HTTP_VALIDATE_OPEN_PROCESS:
        http->processCount++;
        if (http->processCount > limits->processMax) {
            unlock(endpoint);
            httpError(conn, HTTP_CODE_SERVICE_UNAVAILABLE, "Server overloaded");
            mprLog(2, "Too many concurrent processes %d/%d", http->processCount, limits->processMax);
            return 0;
        }
        action = "start process";
        dir = HTTP_TRACE_RX;
        break;

    case HTTP_VALIDATE_CLOSE_PROCESS:
        http->processCount--;
        assure(http->processCount >= 0);
        break;
    }
    if (event == HTTP_VALIDATE_CLOSE_CONN || event == HTTP_VALIDATE_CLOSE_REQUEST) {
        if ((level = httpShouldTrace(conn, dir, HTTP_TRACE_LIMITS, NULL)) >= 0) {
            LOG(4, "Validate request for %s. Active connections %d, active requests: %d/%d, active client IP %d/%d", 
                action, mprGetListLength(http->connections), endpoint->requestCount, limits->requestMax, 
                endpoint->clientCount, limits->clientMax);
        }
    }
#if KEEP
    LOG(0, "Validate Active connections %d, requests: %d/%d, IP %d/%d, Processes %d/%d", 
        mprGetListLength(http->connections), endpoint->requestCount, limits->requestMax, 
        endpoint->clientCount, limits->clientMax, http->processCount, limits->processMax);
#endif
    unlock(endpoint);
    return 1;
}


PUBLIC HttpConn *httpAcceptConn(HttpEndpoint *endpoint, MprEvent *event)
{
    MprSocket   *sock;

    /*
        In sync mode, this will block until a connection arrives
     */
    sock = mprAcceptSocket(endpoint->sock);

    /*
        Immediately re-enable because acceptConn can block while servicing a request. Must always re-enable even if
        the sock acceptance above fails.
     */
    if (endpoint->sock->handler) {
        mprEnableSocketEvents(endpoint->sock, MPR_READABLE);
    }
    if (sock == 0) {
        return 0;
    }
    return acceptConn(sock, event->dispatcher, endpoint);
}


/*  
    Accept a new client connection on a new socket. This will come in on a worker thread dedicated to this connection. 
 */
static HttpConn *acceptConn(MprSocket *sock, MprDispatcher *dispatcher, HttpEndpoint *endpoint)
{
    Http        *http;
    HttpConn    *conn;
    MprEvent    e;
    int         level;

    assure(dispatcher);
    assure(endpoint);
    http = endpoint->http;

    if (endpoint->ssl) {
        if (mprUpgradeSocket(sock, endpoint->ssl, 1) < 0) {
            mprCloseSocket(sock, 0);
            return 0;
        }
    }
    if (mprShouldDenyNewRequests()) {
        mprCloseSocket(sock, 0);
        return 0;
    }
#if FUTURE
    static int  warnOnceConnections = 0;
    int count;
    /* 
        Client connections are entered into http->connections. Need to split into two lists 
        Also, ejs pre-allocates connections in the Http constructor.
     */
    if ((count = mprGetListLength(http->connections)) >= endpoint->limits->requestMax) {
        /* To help alleviate DOS - we just close without responding */
        if (!warnOnceConnections) {
            warnOnceConnections = 1;
            mprLog(2, "Too many concurrent connections %d/%d", count, endpoint->limits->requestMax);
        }
        mprCloseSocket(sock, 0);
        http->underAttack = 1;
        return 0;
    }
#endif
    if ((conn = httpCreateConn(http, endpoint, dispatcher)) == 0) {
        mprCloseSocket(sock, 0);
        return 0;
    }
    conn->notifier = endpoint->notifier;
    conn->async = endpoint->async;
    conn->endpoint = endpoint;
    conn->sock = sock;
    conn->port = sock->port;
    conn->ip = sclone(sock->ip);
    conn->secure = (endpoint->ssl != 0);

    if (!httpValidateLimits(endpoint, HTTP_VALIDATE_OPEN_CONN, conn)) {
        conn->endpoint = 0;
        httpDestroyConn(conn);
        return 0;
    }
    assure(conn->state == HTTP_STATE_BEGIN);
    httpSetState(conn, HTTP_STATE_CONNECTED);

    if ((level = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_CONN, NULL)) >= 0) {
        mprLog(level, "### Incoming connection from %s:%d to %s:%d %s", 
            conn->ip, conn->port, sock->acceptIp, sock->acceptPort, conn->secure ? "(secure)" : "");
        if (endpoint->ssl) {
            mprLog(level, "Upgrade to TLS");
        }
    }
    e.mask = MPR_READABLE;
    e.timestamp = conn->http->now;
    (conn->ioCallback)(conn, &e);
    return conn;
}


PUBLIC void httpMatchHost(HttpConn *conn)
{ 
    MprSocket       *listenSock;
    HttpEndpoint    *endpoint;
    HttpHost        *host;
    Http            *http;

    http = conn->http;
    listenSock = conn->sock->listenSock;

    if ((endpoint = httpLookupEndpoint(http, listenSock->ip, listenSock->port)) == 0) {
        mprError("No listening endpoint for request from %s:%d", listenSock->ip, listenSock->port);
        mprCloseSocket(conn->sock, 0);
        return;
    }
    if (httpHasNamedVirtualHosts(endpoint)) {
        host = httpLookupHostOnEndpoint(endpoint, conn->rx->hostHeader);
    } else {
        host = mprGetFirstItem(endpoint->hosts);
    }
    if (host == 0) {
        httpSetConnHost(conn, 0);
        httpError(conn, HTTP_CODE_NOT_FOUND, "No host to serve request. Searching for %s", conn->rx->hostHeader);
        conn->host = mprGetFirstItem(endpoint->hosts);
        return;
    }
    if (conn->rx->traceLevel >= 0) {
        mprLog(conn->rx->traceLevel, "Use endpoint: %s:%d", endpoint->ip, endpoint->port);
    }
    conn->host = host;
}


PUBLIC void *httpGetEndpointContext(HttpEndpoint *endpoint)
{
    assure(endpoint);
    if (endpoint) {
        return endpoint->context;
    }
    return 0;
}


PUBLIC int httpIsEndpointAsync(HttpEndpoint *endpoint) 
{
    assure(endpoint);
    if (endpoint) {
        return endpoint->async;
    }
    return 0;
}


PUBLIC void httpSetEndpointAddress(HttpEndpoint *endpoint, cchar *ip, int port)
{
    assure(endpoint);

    if (ip) {
        endpoint->ip = sclone(ip);
    }
    if (port >= 0) {
        endpoint->port = port;
    }
    if (endpoint->sock) {
        httpStopEndpoint(endpoint);
        httpStartEndpoint(endpoint);
    }
}


PUBLIC void httpSetEndpointAsync(HttpEndpoint *endpoint, int async)
{
    if (endpoint->sock) {
        if (endpoint->async && !async) {
            mprSetSocketBlockingMode(endpoint->sock, 1);
        }
        if (!endpoint->async && async) {
            mprSetSocketBlockingMode(endpoint->sock, 0);
        }
    }
    endpoint->async = async;
}


PUBLIC void httpSetEndpointContext(HttpEndpoint *endpoint, void *context)
{
    assure(endpoint);
    endpoint->context = context;
}


PUBLIC void httpSetEndpointNotifier(HttpEndpoint *endpoint, HttpNotifier notifier)
{
    assure(endpoint);
    endpoint->notifier = notifier;
}


PUBLIC int httpSecureEndpoint(HttpEndpoint *endpoint, struct MprSsl *ssl)
{
#if BIT_PACK_SSL
    endpoint->ssl = ssl;
    return 0;
#else
    return MPR_ERR_BAD_STATE;
#endif
}


PUBLIC int httpSecureEndpointByName(cchar *name, struct MprSsl *ssl)
{
    HttpEndpoint    *endpoint;
    Http            *http;
    char            *ip;
    int             port, next, count;

    http = MPR->httpService;
    mprParseSocketAddress(name, &ip, &port, -1);
    if (ip == 0) {
        ip = "";
    }
    for (count = 0, next = 0; (endpoint = mprGetNextItem(http->endpoints, &next)) != 0; ) {
        if (endpoint->port <= 0 || port <= 0 || endpoint->port == port) {
            assure(endpoint->ip);
            if (*endpoint->ip == '\0' || *ip == '\0' || scmp(endpoint->ip, ip) == 0) {
                httpSecureEndpoint(endpoint, ssl);
                count++;
            }
        }
    }
    return (count == 0) ? MPR_ERR_CANT_FIND : 0;
}


PUBLIC void httpAddHostToEndpoint(HttpEndpoint *endpoint, HttpHost *host)
{
    mprAddItem(endpoint->hosts, host);
    if (endpoint->limits == 0) {
        endpoint->limits = host->defaultRoute->limits;
    }
}


PUBLIC bool httpHasNamedVirtualHosts(HttpEndpoint *endpoint)
{
    return endpoint->flags & HTTP_NAMED_VHOST;
}


PUBLIC void httpSetHasNamedVirtualHosts(HttpEndpoint *endpoint, bool on)
{
    if (on) {
        endpoint->flags |= HTTP_NAMED_VHOST;
    } else {
        endpoint->flags &= ~HTTP_NAMED_VHOST;
    }
}


/*
    Only used for named virtual hosts
 */
PUBLIC HttpHost *httpLookupHostOnEndpoint(HttpEndpoint *endpoint, cchar *name)
{
    HttpHost    *host;
    int         next;

    if (name == 0 || *name == '\0') {
        return mprGetFirstItem(endpoint->hosts);
    }
    for (next = 0; (host = mprGetNextItem(endpoint->hosts, &next)) != 0; ) {
        if (smatch(host->name, name)) {
            return host;
        }
        if (*host->name == '*') {
            if (host->name[1] == '\0') {
                return host;
            }
            if (scontains(name, &host->name[1])) {
                return host;
            }
        }
    }
    return 0;
}


PUBLIC int httpConfigureNamedVirtualEndpoints(Http *http, cchar *ip, int port)
{
    HttpEndpoint    *endpoint;
    int             next, count;

    if (ip == 0) {
        ip = "";
    }
    for (count = 0, next = 0; (endpoint = mprGetNextItem(http->endpoints, &next)) != 0; ) {
        if (endpoint->port <= 0 || port <= 0 || endpoint->port == port) {
            assure(endpoint->ip);
            if (*endpoint->ip == '\0' || *ip == '\0' || scmp(endpoint->ip, ip) == 0) {
                httpSetHasNamedVirtualHosts(endpoint, 1);
                count++;
            }
        }
    }
    return (count == 0) ? MPR_ERR_CANT_FIND : 0;
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
