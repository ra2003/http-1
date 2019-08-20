/*
    host.c -- Host class for all HTTP hosts

    The Host class is used for the default HTTP server and for all virtual hosts (including SSL hosts).
    Many objects are controlled at the host level. Eg. URL handlers.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"
#include    "pcre.h"

/*********************************** Locals ***********************************/

static HttpHost *defaultHost;

/********************************** Forwards **********************************/

static void manageHost(HttpHost *host, int flags);

/*********************************** Code *************************************/

PUBLIC HttpHost *httpCreateHost()
{
    HttpHost    *host;

    if ((host = mprAllocObj(HttpHost, manageHost)) == 0) {
        return 0;
    }
    if ((host->responseCache = mprCreateCache(MPR_CACHE_SHARED)) == 0) {
        return 0;
    }
    mprSetCacheLimits(host->responseCache, 0, ME_MAX_CACHE_DURATION, 0, 0);

    host->routes = mprCreateList(-1, MPR_LIST_STABLE);
    host->flags = HTTP_HOST_NO_TRACE;
    host->streaming = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STABLE);
    httpSetStreaming(host, "application/x-www-form-urlencoded", NULL, 0);
    httpSetStreaming(host, "application/json", NULL, 0);
    httpSetStreaming(host, "application/csp-report", NULL, 0);
    httpSetStreaming(host, "multipart/form-data", NULL, 0);
    httpAddHost(host);
    return host;
}


PUBLIC HttpHost *httpCloneHost(HttpHost *parent)
{
    HttpHost    *host;

    if ((host = httpCreateHost()) == 0) {
        return 0;
    }
    /*
        The dirs and routes are all copy-on-write.
        Do not clone routes, ip, port and name
     */
    host->parent = parent;
    host->flags = parent->flags & HTTP_HOST_NO_TRACE;
    host->streaming = parent->streaming;
    host->routes = mprCreateList(-1, MPR_LIST_STABLE);
    return host;
}


static void manageHost(HttpHost *host, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(host->hostname);
        mprMark(host->name);
        mprMark(host->canonical);
        mprMark(host->parent);
        mprMark(host->responseCache);
        mprMark(host->routes);
        mprMark(host->defaultRoute);
        mprMark(host->defaultEndpoint);
        mprMark(host->secureEndpoint);
        mprMark(host->streaming);

    } else if (flags & MPR_MANAGE_FREE) {
        if (host->nameCompiled) {
            free(host->nameCompiled);
        }
    }
}


PUBLIC HttpHost *httpCreateDefaultHost()
{
    HttpHost    *host;
    HttpRoute   *route;

    if (defaultHost) {
        return defaultHost;
    }
    defaultHost = host = httpCreateHost();
    route = httpCreateRoute(host);
    httpSetHostDefaultRoute(host, route);
    route->limits = route->http->serverLimits;
    return host;
}


PUBLIC int httpStartHost(HttpHost *host)
{
    HttpRoute   *route;
    int         next;

    for (ITERATE_ITEMS(host->routes, route, next)) {
        httpStartRoute(route);
    }
    for (ITERATE_ITEMS(host->routes, route, next)) {
        if (!route->trace && route->parent && route->parent->trace) {
            route->trace = route->parent->trace;
        }
    }
    return 0;
}


PUBLIC void httpStopHost(HttpHost *host)
{
    HttpRoute   *route;
    int         next;

    for (ITERATE_ITEMS(host->routes, route, next)) {
        httpStopRoute(route);
    }
}


PUBLIC HttpRoute *httpGetHostDefaultRoute(HttpHost *host)
{
    return host->defaultRoute;
}


static void printRouteHeader(HttpHost *host, int *methodsLen, int *patternLen, int *targetLen)
{
    HttpRoute   *route;
    int         next;

    *methodsLen = (int) slen("Methods");
    *patternLen = (int) slen("Route");
    *targetLen = (int) slen("$&");

    for (next = 0; (route = mprGetNextItem(host->routes, &next)) != 0; ) {
        *targetLen = (int) max(*targetLen, slen(route->target));
        *patternLen = (int) max(*patternLen, slen(route->pattern));
        *methodsLen = (int) max(*methodsLen, slen(httpGetRouteMethods(route)));
    }
    printf("\nRoutes for host: %s\n\n", host->name ? host->name : "default");
    printf("%-*s %-*s %-*s\n", *patternLen, "Route", *methodsLen, "Methods", *targetLen, "Target");
    printf("%-*s %-*s %-*s\n", *patternLen, "-----", *methodsLen, "-------", *targetLen, "------");
}


static void printRoute(HttpRoute *route, int idx, bool full, int methodsLen, int patternLen, int targetLen)
{
    HttpRouteOp *condition;
    HttpStage   *handler;
    HttpAuth    *auth;
    MprKey      *kp;
    cchar       *methods, *pattern, *target, *index;
    int         nextIndex;

    auth = route->auth;
    methods = httpGetRouteMethods(route);
    methods = methods ? methods : "*";
    pattern = (route->pattern && *route->pattern) ? route->pattern : "^.*$";
    target = (route->target && *route->target) ? route->target : "$&";

    if (full) {
        printf("\nRoutes for host: %s\n", route->host->name ? route->host->name : "default");
        printf("\n  Route [%d]. %s\n", idx, pattern);
        if (route->prefix && *route->prefix) {
            printf("    Prefix:       %s\n", route->prefix);
        }
        printf("    RegExp:       %s\n", route->optimizedPattern ? route->optimizedPattern : "");
        printf("    Methods:      %s\n", methods);
        printf("    Target:       %s\n", target);
        printf("    Auth:         %s\n", auth->type ? auth->type->name : "-");
        printf("    Home:         %s\n", route->home);
        printf("    Documents:    %s\n", route->documents);
        if (route->sourceName) {
            printf("    Source:       %s\n", route->sourceName);
        }
        if (route->tplate) {
            printf("    Template:     %s\n", route->tplate);
        }
        if (route->indexes) {
            for (ITERATE_ITEMS(route->indexes, index, nextIndex)) {
                printf("    Indexes:      %s \n", index);
            }
        }
        if (route->conditions) {
            for (nextIndex = 0; (condition = mprGetNextItem(route->conditions, &nextIndex)) != 0; ) {
                printf("    Condition:    %s %s\n", condition->name, condition->details ? condition->details : "");
            }
        }
        if (route->handler) {
            printf("    Handler:      %s\n", route->handler->name);
        }
        if (route->extensions) {
            for (ITERATE_KEYS(route->extensions, kp)) {
                handler = (HttpStage*) kp->data;
                printf("    Extension:    \"%s\" => %s\n", kp->key, handler->name);
            }
        }
        if (route->handlers) {
            for (ITERATE_ITEMS(route->handlers, handler, nextIndex)) {
                printf("    Handler:      %s\n", handler->name);
            }
        }
    } else {
        printf("%-*s %-*s %-*s\n", patternLen, pattern, methodsLen, methods, targetLen, target);
    }
}


PUBLIC void httpLogRoutes(HttpHost *host, bool full)
{
    HttpRoute   *route;
    int         index, methodsLen, patternLen, targetLen;

    if (!host) {
        host = httpGetDefaultHost();
    }
    if (mprGetListLength(host->routes) == 0 && !host->defaultRoute) {
        printf("\nRoutes for host: %s: none\n", host->name ? host->name : "default");
    } else {
        methodsLen = patternLen = targetLen = 0;
        if (!full) {
            printRouteHeader(host, &methodsLen, &patternLen, &targetLen);
        }
        for (index = 0; (route = mprGetNextItem(host->routes, &index)) != 0; ) {
            printRoute(route, index - 1, full, methodsLen, patternLen, targetLen);
        }
        if (mprLookupItem(host->routes, host->defaultRoute) < 0) {
            printRoute(host->defaultRoute, index, full, methodsLen, patternLen, targetLen);
        }
    }
    printf("\n");
}


PUBLIC int httpSetHostCanonicalName(HttpHost *host, cchar *name)
{
    if (!name || *name == '\0') {
        mprLog("error http", 0, "Empty host name");
        return MPR_ERR_BAD_ARGS;
    }
    if (schr(name, ':')) {
        host->canonical = httpCreateUri(name, 0);
    } else {
        host->canonical = httpCreateUri(sjoin(name, ":", 0), 0);
    }
    return 0;
}


PUBLIC int httpSetHostName(HttpHost *host, cchar *name)
{
    cchar   *errMsg;
    char    *cp;
    int     column;

    if (!name || *name == '\0') {
        mprLog("error http", 0, "Empty host name");
        return MPR_ERR_BAD_ARGS;
    }
    host->name = sclone(name);
    host->hostname = strim(name, "/*", MPR_TRIM_BOTH);
    if ((cp = schr(host->hostname, ':')) != 0) {
        host->hostname = ssplit((char*) host->hostname, ":", NULL);
    }
    host->flags &= ~(HTTP_HOST_WILD_STARTS | HTTP_HOST_WILD_CONTAINS | HTTP_HOST_WILD_REGEXP);
    if (sends(name, "*")) {
        host->flags |= HTTP_HOST_WILD_STARTS;

    } else if (*name == '*') {
        host->flags |= HTTP_HOST_WILD_CONTAINS;

    } else if (*name == '/') {
        host->flags |= HTTP_HOST_WILD_REGEXP;
        if (host->nameCompiled) {
            free(host->nameCompiled);
        }
        if ((host->nameCompiled = pcre_compile2(host->hostname, 0, 0, &errMsg, &column, NULL)) == 0) {
            mprLog("error http route", 0, "Cannot compile condition match pattern. Error %s at column %d", errMsg, column);
            return MPR_ERR_BAD_SYNTAX;
        }
    }
    return 0;
}


PUBLIC int httpAddRoute(HttpHost *host, HttpRoute *route)
{
    HttpRoute   *prev, *item, *lastRoute;
    int         i, thisRoute;

    assert(route);

    if (host->parent && host->routes == host->parent->routes) {
        host->routes = mprCloneList(host->parent->routes);
    }
    if (mprLookupItem(host->routes, route) < 0) {
        if (route->pattern[0] && (lastRoute = mprGetLastItem(host->routes)) && lastRoute->pattern[0] == '\0') {
            /*
                Insert non-default route before last default route
             */
            thisRoute = mprInsertItemAtPos(host->routes, mprGetListLength(host->routes) - 1, route);
        } else {
            thisRoute = mprAddItem(host->routes, route);
        }
        if (thisRoute > 0) {
            prev = mprGetItem(host->routes, thisRoute - 1);
            if (!smatch(prev->startSegment, route->startSegment)) {
                prev->nextGroup = thisRoute;
                for (i = thisRoute - 2; i >= 0; i--) {
                    item = mprGetItem(host->routes, i);
                    if (smatch(item->startSegment, prev->startSegment)) {
                        item->nextGroup = thisRoute;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    httpSetRouteHost(route, host);
    return 0;
}


PUBLIC HttpRoute *httpLookupRoute(HttpHost *host, cchar *pattern)
{
    HttpRoute   *route;
    int         next;

    if (smatch(pattern, "default")) {
        pattern = "";
    }
    if (smatch(pattern, "/") || smatch(pattern, "^/") || smatch(pattern, "^/$")) {
        pattern = "";
    }
    if (!host && (host = httpGetDefaultHost()) == 0) {
        return 0;
    }
    for (next = 0; (route = mprGetNextItem(host->routes, &next)) != 0; ) {
        assert(route->pattern);
        if (smatch(route->pattern, pattern)) {
            return route;
        }
    }
    return 0;
}


PUBLIC void httpResetRoutes(HttpHost *host)
{
    host->routes = mprCreateList(-1, MPR_LIST_STABLE);
}


PUBLIC void httpSetHostDefaultRoute(HttpHost *host, HttpRoute *route)
{
    host->defaultRoute = route;
}


PUBLIC void httpSetDefaultHost(HttpHost *host)
{
    defaultHost = host;
}


PUBLIC void httpSetHostSecureEndpoint(HttpHost *host, HttpEndpoint *endpoint)
{
    host->secureEndpoint = endpoint;
}


PUBLIC void httpSetHostDefaultEndpoint(HttpHost *host, HttpEndpoint *endpoint)
{
    host->defaultEndpoint = endpoint;
}


PUBLIC HttpHost *httpGetDefaultHost()
{
    return defaultHost;
}


PUBLIC HttpRoute *httpGetDefaultRoute(HttpHost *host)
{
    if (host) {
        return host->defaultRoute;
    } else if (defaultHost) {
        return defaultHost->defaultRoute;
    }
    return 0;
}


/*
    Determine if input body content should be streamed or buffered for requests with content of a given mime type.
 */
PUBLIC bool httpGetStreaming(HttpHost *host, cchar *mime, cchar *uri)
{
    MprKey      *kp;

    assert(host);
    assert(host->streaming);

    if (schr(mime, ';')) {
        mime = ssplit(sclone(mime), ";", 0);
    }
    if ((kp = mprLookupKeyEntry(host->streaming, mime)) != 0) {
        if (kp->data == NULL || sstarts(uri, kp->data)) {
            /* Type is set to the enable value */
            return kp->type;
        }
    }
    return 1;
}


/*
    Define streaming to be enabled for a given mime type that starts with the given URI for this host
 */
PUBLIC void httpSetStreaming(HttpHost *host, cchar *mime, cchar *uri, bool enable)
{
    MprKey  *kp;

    assert(host);
    if ((kp = mprAddKey(host->streaming, mime, uri)) != 0) {
        /*
            We store the enable value in the key type to save an allocation
         */
        kp->type = enable;
    }
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
