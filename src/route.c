/*
    route.c -- Http request routing

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"
#include    "pcre.h"

/********************************** Forwards **********************************/

#undef  GRADUATE_LIST
#define GRADUATE_LIST(route, field) \
    if (route->field == 0) { \
        route->field = mprCreateList(-1, 0); \
    } else if (route->parent && route->field == route->parent->field) { \
        route->field = mprCloneList(route->parent->field); \
    }

#undef  GRADUATE_HASH
#define GRADUATE_HASH(route, field) \
    if (!route->field || (route->parent && route->field == route->parent->field)) { \
        route->field = mprCloneHash(route->parent->field); \
    }

/********************************** Forwards **********************************/

static void addUniqueItem(MprList *list, HttpRouteOp *op);
static int checkRoute(HttpConn *conn, HttpRoute *route);
static HttpLang *createLangDef(cchar *path, cchar *suffix, int flags);
static HttpRouteOp *createRouteOp(cchar *name, int flags);
static void definePathVars(HttpRoute *route);
static void defineHostVars(HttpRoute *route);
static char *expandTokens(HttpConn *conn, cchar *path);
static char *expandPatternTokens(cchar *str, cchar *replacement, int *matches, int matchCount);
static char *expandRequestTokens(HttpConn *conn, char *targetKey);
static void finalizePattern(HttpRoute *route);
static char *finalizeReplacement(HttpRoute *route, cchar *str);
static char *finalizeTemplate(HttpRoute *route);
static bool opPresent(MprList *list, HttpRouteOp *op);
static void manageRoute(HttpRoute *route, int flags);
static void manageLang(HttpLang *lang, int flags);
static void manageRouteOp(HttpRouteOp *op, int flags);
static int matchRequestUri(HttpConn *conn, HttpRoute *route);
static int matchRoute(HttpConn *conn, HttpRoute *route);
static int selectHandler(HttpConn *conn, HttpRoute *route);
static int testCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *condition);
static char *trimQuotes(char *str);
static int updateRequest(HttpConn *conn, HttpRoute *route, HttpRouteOp *update);

/************************************ Code ************************************/
/*
    Host may be null
 */
PUBLIC HttpRoute *httpCreateRoute(HttpHost *host)
{
    Http        *http;
    HttpRoute   *route;

    http = HTTP;
    if ((route = mprAllocObj(HttpRoute, manageRoute)) == 0) {
        return 0;
    }
    route->auth = httpCreateAuth();
    route->defaultLanguage = sclone("en");
    route->home = route->documents = mprGetCurrentPath();
    route->flags = HTTP_ROUTE_STEALTH;

    route->flags |= HTTP_ROUTE_ENV_ESCAPE;
    route->envPrefix = sclone("CGI_");

    route->host = host;
    route->http = HTTP;
    route->lifespan = ME_MAX_CACHE_DURATION;
    route->pattern = MPR->emptyString;
    route->targetRule = sclone("run");
    route->autoDelete = 1;
    route->workers = -1;
    route->prefix = MPR->emptyString;
    route->trace = http->trace;
#if DEPRECATE
    route->serverPrefix = MPR->emptyString;
#endif
    route->headers = mprCreateList(-1, MPR_LIST_STABLE);
    route->handlers = mprCreateList(-1, MPR_LIST_STABLE);
    route->indexes = mprCreateList(-1, MPR_LIST_STABLE);
    route->inputStages = mprCreateList(-1, MPR_LIST_STABLE);
    route->outputStages = mprCreateList(-1, MPR_LIST_STABLE);

    route->extensions = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
    route->errorDocuments = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STABLE);
    route->methods = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
    route->vars = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);

    httpAddRouteMethods(route, NULL);
    httpAddRouteFilter(route, http->rangeFilter->name, NULL, HTTP_STAGE_TX);
    httpAddRouteFilter(route, http->chunkFilter->name, NULL, HTTP_STAGE_RX | HTTP_STAGE_TX);

    /*
        Standard headers for all routes. These should not break typical content
        Users then vary via header directives
     */
    httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "Vary", "Accept-Encoding");
    httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-XSS-Protection", "1; mode=block");
    httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-Frame-Options", "SAMEORIGIN");
    httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-Content-Type-Options", "nosniff");

    if (MPR->httpService) {
        route->limits = mprMemdup(http->serverLimits ? http->serverLimits : http->clientLimits, sizeof(HttpLimits));
    }
    route->mimeTypes = MPR->mimeTypes;
    definePathVars(route);
    return route;
}


/*
    Create a new location block. Inherit from the parent. We use a copy-on-write scheme if these are modified later.
 */
PUBLIC HttpRoute *httpCreateInheritedRoute(HttpRoute *parent)
{
    HttpRoute  *route;

    if (!parent && (parent = httpGetDefaultRoute(0)) == 0) {
        return 0;
    }
    if ((route = mprAllocObj(HttpRoute, manageRoute)) == 0) {
        return 0;
    }
    route->auth = httpCreateInheritedAuth(parent->auth);
    route->autoDelete = parent->autoDelete;
    route->caching = parent->caching;
    route->clientConfig = parent->clientConfig;
    route->conditions = parent->conditions;
    route->config = parent->config;
    route->connector = parent->connector;
    route->cookie = parent->cookie;
    route->corsAge = parent->corsAge;
    route->corsCredentials = parent->corsCredentials;
    route->corsHeaders = parent->corsHeaders;
    route->corsMethods = parent->corsMethods;
    route->corsOrigin = parent->corsOrigin;
    route->data = parent->data;
    route->database = parent->database;
    route->defaultLanguage = parent->defaultLanguage;
    route->documents = parent->documents;
    route->envPrefix = parent->envPrefix;
    route->eroute = parent->eroute;
    route->errorDocuments = parent->errorDocuments;
    route->extensions = parent->extensions;
    route->flags = parent->flags & ~(HTTP_ROUTE_FREE_PATTERN);
    route->handler = parent->handler;
    route->handlers = parent->handlers;
    route->headers = parent->headers;
    route->home = parent->home;
    route->host = parent->host;
    route->http = HTTP;
    route->indexes = parent->indexes;
    route->inputStages = parent->inputStages;
    route->json = parent->json;
    route->languages = parent->languages;
    route->lifespan = parent->lifespan;
    route->limits = parent->limits;
    route->map = parent->map;
    route->methods = parent->methods;
    route->mimeTypes = parent->mimeTypes;
    route->mode = parent->mode;
    route->optimizedPattern = parent->optimizedPattern;
    route->outputStages = parent->outputStages;
    route->params = parent->params;
    route->parent = parent;
    route->pattern = parent->pattern;
    route->patternCompiled = parent->patternCompiled;
    route->prefix = parent->prefix;
    route->prefixLen = parent->prefixLen;
    route->renameUploads = parent->renameUploads;
    route->requestHeaders = parent->requestHeaders;
    route->responseFormat = parent->responseFormat;
    route->responseStatus = parent->responseStatus;
    route->script = parent->script;
    route->scriptPath = parent->scriptPath;
    route->sourceName = parent->sourceName;
    route->ssl = parent->ssl;
    route->target = parent->target;
    route->targetRule = parent->targetRule;
    route->tokens = parent->tokens;
    route->trace = parent->trace;
    route->updates = parent->updates;
    route->vars = parent->vars;
    route->workers = parent->workers;
#if DEPRECATE
    route->serverPrefix = parent->serverPrefix;
#endif
    return route;
}


static void manageRoute(HttpRoute *route, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(route->auth);
        mprMark(route->caching);
        mprMark(route->clientConfig);
        mprMark(route->conditions);
        mprMark(route->config);
        mprMark(route->connector);
        mprMark(route->context);
        mprMark(route->cookie);
        mprMark(route->corsHeaders);
        mprMark(route->corsMethods);
        mprMark(route->corsOrigin);
        mprMark(route->data);
        mprMark(route->database);
        mprMark(route->defaultLanguage);
        mprMark(route->documents);
        mprMark(route->envPrefix);
        mprMark(route->eroute);
        mprMark(route->errorDocuments);
        mprMark(route->extensions);
        mprMark(route->handler);
        mprMark(route->handlers);
        mprMark(route->headers);
        mprMark(route->home);
        mprMark(route->host);
        mprMark(route->http);
        mprMark(route->indexes);
        mprMark(route->inputStages);
        mprMark(route->languages);
        mprMark(route->limits);
        mprMark(route->map);
        mprMark(route->methods);
        mprMark(route->mimeTypes);
        mprMark(route->mode);
        mprMark(route->optimizedPattern);
        mprMark(route->outputStages);
        mprMark(route->params);
        mprMark(route->parent);
        mprMark(route->pattern);
        mprMark(route->prefix);
        mprMark(route->requestHeaders);
        mprMark(route->responseFormat);
        mprMark(route->script);
        mprMark(route->scriptPath);
        mprMark(route->sourceName);
        mprMark(route->ssl);
        mprMark(route->startSegment);
        mprMark(route->startWith);
        mprMark(route->target);
        mprMark(route->targetRule);
        mprMark(route->tokens);
        mprMark(route->trace);
        mprMark(route->tplate);
        mprMark(route->updates);
        mprMark(route->vars);
        mprMark(route->webSocketsProtocol);
#if DEPRECATE
        mprMark(route->serverPrefix);
#endif

    } else if (flags & MPR_MANAGE_FREE) {
        if (route->patternCompiled && (route->flags & HTTP_ROUTE_FREE_PATTERN)) {
            free(route->patternCompiled);
        }
    }
}


PUBLIC HttpRoute *httpCreateDefaultRoute(HttpHost *host)
{
    HttpRoute   *route;

    assert(host);
    if ((route = httpCreateRoute(host)) == 0) {
        return 0;
    }
    httpFinalizeRoute(route);
    return route;
}


/*
    Create and configure a basic route. This is used for client side and Ejscript routes. Host may be null.
 */
PUBLIC HttpRoute *httpCreateConfiguredRoute(HttpHost *host, int serverSide)
{
    HttpRoute   *route;
    Http        *http;

    /*
        Create default incoming and outgoing pipelines. Order matters.
     */
    route = httpCreateRoute(host);
    http = route->http;
#if ME_HTTP_WEB_SOCKETS
    httpAddRouteFilter(route, http->webSocketFilter->name, NULL, HTTP_STAGE_RX | HTTP_STAGE_TX);
#endif
    if (serverSide) {
        httpAddRouteFilter(route, http->uploadFilter->name, NULL, HTTP_STAGE_RX);
    }
    return route;
}


PUBLIC HttpRoute *httpCreateAliasRoute(HttpRoute *parent, cchar *pattern, cchar *path, int status)
{
    HttpRoute   *route;

    assert(parent);
    assert(pattern && *pattern);

    if ((route = httpCreateInheritedRoute(parent)) == 0) {
        return 0;
    }
    httpSetRoutePattern(route, pattern, 0);
    if (path) {
        httpSetRouteDocuments(route, path);
    }
    route->responseStatus = status;
    return route;
}


/*
    This routine binds a new route to a URI. It creates a handler, route and binds a callback to that route.
 */
PUBLIC HttpRoute *httpCreateActionRoute(HttpRoute *parent, cchar *pattern, HttpAction action)
{
    HttpRoute   *route;
    cchar       *name;

    if (!pattern || !action) {
        return 0;
    }
    if ((route = httpCreateInheritedRoute(parent)) != 0) {
        route->handler = route->http->actionHandler;
        httpSetRoutePattern(route, pattern, 0);
        name = strim(pattern, "^$", 0);
        httpDefineAction(name, action);
        httpFinalizeRoute(route);
    }
    return route;
}


PUBLIC int httpStartRoute(HttpRoute *route)
{
#if !ME_ROM
    if (!(route->flags & HTTP_ROUTE_STARTED)) {
        route->flags |= HTTP_ROUTE_STARTED;
        if (route->trace != route->trace->parent) {
            httpOpenTraceLogFile(route->trace);
        }
    }
#endif
    return 0;
}


PUBLIC void httpStopRoute(HttpRoute *route)
{
}


/*
    Find the matching route and handler for a request. If any errors occur, the pass handler is used to
    pass errors via the net/sendfile connectors onto the client. This process may rewrite the request
    URI and may redirect the request.
 */
PUBLIC void httpRouteRequest(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    int         next, rewrites, match;

    rx = conn->rx;
    tx = conn->tx;
    route = 0;
    rewrites = 0;

    if (conn->error) {
        tx->handler = conn->http->passHandler;
        route = rx->route = conn->host->defaultRoute;

    } else {
        for (next = rewrites = 0; rewrites < ME_MAX_REWRITE; ) {
            if (next >= conn->host->routes->length) {
                break;
            }
            route = conn->host->routes->items[next++];
            if (route->startSegment && strncmp(rx->pathInfo, route->startSegment, route->startSegmentLen) != 0) {
                /* Failed to match the first URI segment, skip to the next group */
                if (next < route->nextGroup) {
                    next = route->nextGroup;
                }

            } else if (route->startWith && strncmp(rx->pathInfo, route->startWith, route->startWithLen) != 0) {
                /* Failed to match starting literal segment of the route pattern, advance to test the next route */
                continue;

            } else if ((match = matchRoute(conn, route)) == HTTP_ROUTE_REROUTE) {
                next = 0;
                route = 0;
                rewrites++;

            } else if (match == HTTP_ROUTE_OK) {
                break;
            }
        }
    }
    if (route == 0 || tx->handler == 0) {
        rx->route = conn->host->defaultRoute;
        httpError(conn, HTTP_CODE_BAD_METHOD, "Cannot find suitable route for request method");
        return;
    }
    rx->route = route;
    conn->limits = route->limits;
    conn->trace = route->trace;

    if (rewrites >= ME_MAX_REWRITE) {
        httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Too many request rewrites");
    }
    if (tx->finalized) {
        /* Pass handler can transmit the error */
        tx->handler = conn->http->passHandler;
    }
    if (tx->handler->module) {
        tx->handler->module->lastActivity = conn->lastActivity;
    }
}


static int matchRoute(HttpConn *conn, HttpRoute *route)
{
    HttpRx      *rx;
    char        *savePathInfo, *pathInfo;
    int         rc;

    assert(conn);
    assert(route);

    rx = conn->rx;
    savePathInfo = 0;

    assert(route->prefix);
    if (route->prefix && *route->prefix) {
        if (!sstarts(rx->pathInfo, route->prefix)) {
            return HTTP_ROUTE_REJECT;
        }
        savePathInfo = rx->pathInfo;
        pathInfo = &rx->pathInfo[route->prefixLen];
        if (*pathInfo == '\0') {
            pathInfo = "/";
        }
        rx->pathInfo = sclone(pathInfo);
        rx->scriptName = route->prefix;
    }
    if ((rc = matchRequestUri(conn, route)) == HTTP_ROUTE_OK) {
        rc = checkRoute(conn, route);
    }
    if (rc == HTTP_ROUTE_REJECT && savePathInfo) {
        /* Keep the modified pathInfo if OK or REWRITE */
        rx->pathInfo = savePathInfo;
        rx->scriptName = 0;
    }
    return rc;
}


static int matchRequestUri(HttpConn *conn, HttpRoute *route)
{
    HttpRx      *rx;

    assert(conn);
    assert(route);
    rx = conn->rx;

    if (route->patternCompiled) {
        rx->matchCount = pcre_exec(route->patternCompiled, NULL, rx->pathInfo, (int) slen(rx->pathInfo), 0, 0,
            rx->matches, sizeof(rx->matches) / sizeof(int));
        if (route->flags & HTTP_ROUTE_NOT) {
            if (rx->matchCount > 0) {
                return HTTP_ROUTE_REJECT;
            }
            rx->matchCount = 1;
            rx->matches[0] = 0;
            rx->matches[1] = (int) slen(rx->pathInfo);

        } else if (rx->matchCount <= 0) {
            return HTTP_ROUTE_REJECT;
        }
    } else if (route->pattern && *route->pattern) {
        /* Pattern compilation failed */
        return HTTP_ROUTE_REJECT;
    }
    if (!mprLookupKey(route->methods, rx->method)) {
        if (!mprLookupKey(route->methods, "*")) {
            if (!(rx->flags & HTTP_HEAD && mprLookupKey(route->methods, "GET"))) {
                return HTTP_ROUTE_REJECT;
            }
        }
    }
    rx->route = route;
    return HTTP_ROUTE_OK;
}


static int checkRoute(HttpConn *conn, HttpRoute *route)
{
    HttpRouteOp     *op, *condition, *update;
    HttpRouteProc   *proc;
    HttpRx          *rx;
    HttpTx          *tx;
    cchar           *token, *value, *header, *field;
    int             next, rc, matched[ME_MAX_ROUTE_MATCHES * 2], count, result;

    assert(conn);
    assert(route);
    rx = conn->rx;
    tx = conn->tx;
    assert(rx->pathInfo[0]);

    rx->target = route->target ? expandTokens(conn, route->target) : sclone(&rx->pathInfo[1]);

    if (route->requestHeaders) {
        for (next = 0; (op = mprGetNextItem(route->requestHeaders, &next)) != 0; ) {
            if ((header = httpGetHeader(conn, op->name)) != 0) {
                count = pcre_exec(op->mdata, NULL, header, (int) slen(header), 0, 0,
                    matched, sizeof(matched) / sizeof(int));
                result = count > 0;
                if (op->flags & HTTP_ROUTE_NOT) {
                    result = !result;
                }
                if (!result) {
                    return HTTP_ROUTE_REJECT;
                }
            }
        }
    }
    if (route->params) {
        for (next = 0; (op = mprGetNextItem(route->params, &next)) != 0; ) {
            if ((field = httpGetParam(conn, op->name, "")) != 0) {
                count = pcre_exec(op->mdata, NULL, field, (int) slen(field), 0, 0,
                    matched, sizeof(matched) / sizeof(int));
                result = count > 0;
                if (op->flags & HTTP_ROUTE_NOT) {
                    result = !result;
                }
                if (!result) {
                    return HTTP_ROUTE_REJECT;
                }
            }
        }
    }
    if (route->conditions) {
        for (next = 0; (condition = mprGetNextItem(route->conditions, &next)) != 0; ) {
            rc = testCondition(conn, route, condition);
            if (rc == HTTP_ROUTE_REROUTE) {
                return rc;
            }
            if (condition->flags & HTTP_ROUTE_NOT) {
                rc = !rc;
            }
            if (rc == HTTP_ROUTE_REJECT) {
                return rc;
            }
        }
    }
    if (route->updates) {
        for (next = 0; (update = mprGetNextItem(route->updates, &next)) != 0; ) {
            if ((rc = updateRequest(conn, route, update)) == HTTP_ROUTE_REROUTE) {
                return rc;
            }
        }
    }
    if (route->prefix[0]) {
        httpSetParam(conn, "prefix", route->prefix);
    }
    if ((rc = selectHandler(conn, route)) != HTTP_ROUTE_OK) {
        return rc;
    }
    if (route->tokens) {
        for (next = 0; (token = mprGetNextItem(route->tokens, &next)) != 0; ) {
            int index = rx->matches[next * 2];
            if (index >= 0) {
                value = snclone(&rx->pathInfo[index], rx->matches[(next * 2) + 1] - index);
                httpSetParam(conn, token, value);
            }
        }
    }
    if ((proc = mprLookupKey(conn->http->routeTargets, route->targetRule)) == 0) {
        httpError(conn, -1, "Cannot find route target rule \"%s\"", route->targetRule);
        return HTTP_ROUTE_REJECT;
    }
    if ((rc = (*proc)(conn, route, 0)) != HTTP_ROUTE_OK) {
        return rc;
    }
    if (tx->finalized) {
        tx->handler = conn->http->passHandler;
    } else if (tx->handler->rewrite) {
        rc = tx->handler->rewrite(conn);
    }
    return rc;
}


static int selectHandler(HttpConn *conn, HttpRoute *route)
{
    HttpRx      *rx;
    HttpTx      *tx;
    int         next, rc;

    assert(conn);
    assert(route);

    rx = conn->rx;
    tx = conn->tx;
    if (route->handler) {
        tx->handler = route->handler;
        return HTTP_ROUTE_OK;
    }
    for (next = 0; (tx->handler = mprGetNextStableItem(route->handlers, &next)) != 0; ) {
        rc = tx->handler->match(conn, route, 0);
        if (rc == HTTP_ROUTE_OK || rc == HTTP_ROUTE_REROUTE) {
            return rc;
        }
    }
    if (!tx->handler) {
        /*
            Now match by extensions
         */
        if (!tx->ext || (tx->handler = mprLookupKey(route->extensions, tx->ext)) == 0) {
            tx->handler = mprLookupKey(route->extensions, "");
        }
    }
    if (rx->flags & HTTP_TRACE) {
        /*
            Trace method always processed for all requests by the passHandler
         */
        tx->handler = conn->http->passHandler;
    }
    if (tx->finalized) {
        tx->handler = conn->http->passHandler;
    }
    return tx->handler ? HTTP_ROUTE_OK : HTTP_ROUTE_REJECT;
}


PUBLIC void httpSetHandler(HttpConn *conn, HttpStage *handler)
{
    conn->tx->handler = handler;
}


PUBLIC cchar *httpMapContent(HttpConn *conn, cchar *filename)
{
    HttpRoute   *route;
    HttpRx      *rx;
    HttpTx      *tx;
    MprKey      *kp;
    MprList     *extensions;
    MprPath     info;
    bool        acceptGzip, zipped;
    cchar       *ext, *path;
    int         next;

    tx = conn->tx;
    rx = conn->rx;
    route = rx->route;

    if (route->map && !(tx->flags & HTTP_TX_NO_MAP)) {
        if ((kp = mprLookupKeyEntry(route->map, tx->ext)) == 0) {
            kp = mprLookupKeyEntry(route->map, "");
        }
        if (kp) {
            extensions = (MprList*) kp->data;
            acceptGzip = scontains(rx->acceptEncoding, "gzip") != 0;
            for (ITERATE_ITEMS(extensions, ext, next)) {
                zipped = sends(ext, "gz") != 0;
                if (zipped && !acceptGzip) {
                    continue;
                }
                if (kp->key[0]) {
                    path = mprReplacePathExt(filename, ext);
                } else {
                    path = sjoin(filename, ext, NULL);
                }
                if (mprGetPathInfo(path, &info) == 0) {
                    httpTrace(conn, "request.map", "context", "originalFilename:'%s',filename:'%s'", filename, path);
                    filename = path;
                    if (zipped) {
                        httpSetHeader(conn, "Content-Encoding", "gzip");
                    }
                    tx->fileInfo = info;
                    break;
                }
            }
        }
    }
    return filename;
}


PUBLIC void httpMapFile(HttpConn *conn)
{
    HttpTx      *tx;
    HttpLang    *lang;
    cchar       *filename;

    tx = conn->tx;
    if (tx->filename) {
        return;
    }
    filename = conn->rx->target;
    lang = conn->rx->lang;
    if (lang && lang->path) {
        filename = mprJoinPath(lang->path, filename);
    }
    filename = mprJoinPath(conn->rx->route->documents, filename);
    filename = httpMapContent(conn, filename);
    httpSetFilename(conn, filename, 0);
}


/************************************ API *************************************/

PUBLIC int httpAddRouteCondition(HttpRoute *route, cchar *name, cchar *details, int flags)
{
    HttpRouteOp *op;
    cchar       *errMsg;
    char        *pattern, *value;
    int         column;

    assert(route);

    GRADUATE_LIST(route, conditions);
    if ((op = createRouteOp(name, flags)) == 0) {
        return MPR_ERR_MEMORY;
    }
    if (scaselessmatch(name, "auth") || scaselessmatch(name, "unauthorized")) {
        /* Nothing to do. Route->auth has it all */

    } else if (scaselessmatch(name, "missing")) {
        op->details = finalizeReplacement(route, "${request:filename}");

    } else if (scaselessmatch(name, "directory")) {
        op->details = finalizeReplacement(route, details);

    } else if (scaselessmatch(name, "exists")) {
        op->details = finalizeReplacement(route, details);

    } else if (scaselessmatch(name, "match")) {
        /*
            Condition match string pattern
            String can contain matching ${tokens} from the route->pattern and can contain request ${tokens}
         */
        if (!httpTokenize(route, details, "%S %S", &value, &pattern)) {
            return MPR_ERR_BAD_SYNTAX;
        }
        if ((op->mdata = pcre_compile2(pattern, 0, 0, &errMsg, &column, NULL)) == 0) {
            mprLog("error http route", 0, "Cannot compile condition match pattern. Error %s at column %d", errMsg, column);
            return MPR_ERR_BAD_SYNTAX;
        }
        op->details = finalizeReplacement(route, value);
        op->flags |= HTTP_ROUTE_FREE;

    } else if (scaselessmatch(name, "secure")) {
        if (!details || *details == '\0') {
            mprLog("error http config", 0, "Secure route condition is missing a redirect target in route \"%s\"",
                route->pattern);
        }
        op->details = finalizeReplacement(route, details);
    }
    addUniqueItem(route->conditions, op);
    return 0;
}


PUBLIC int httpAddRouteFilter(HttpRoute *route, cchar *name, cchar *extensions, int direction)
{
    HttpStage   *stage;
    HttpStage   *filter;
    char        *extlist, *word, *tok;
    int         pos, next;

    assert(route);

    for (ITERATE_ITEMS(route->outputStages, stage, next)) {
        if (smatch(stage->name, name)) {
            mprLog("warn http route", 0, "Stage \"%s\" is already configured for the route \"%s\". Ignoring.",
                name, route->pattern);
            return 0;
        }
    }
    if ((stage = httpLookupStage(name)) == 0) {
        mprLog("error http route", 0, "Cannot find filter %s", name);
        return MPR_ERR_CANT_FIND;
    }
    /*
        Clone an existing stage because each filter stores its own set of extensions to match against
     */
    filter = httpCloneStage(stage);

    if (extensions && *extensions) {
        filter->extensions = mprCreateHash(0, MPR_HASH_CASELESS | MPR_HASH_STABLE);
        extlist = sclone(extensions);
        word = stok(extlist, " \t\r\n", &tok);
        while (word) {
            if (*word == '*' && word[1] == '.') {
                word += 2;
            } else if (*word == '.') {
                word++;
            } else if (*word == '\"' && word[1] == '\"') {
                word = "";
            } else if (*word == '*' && word[1] == '\0') {
                word = "";
            }
            mprAddKey(filter->extensions, word, filter);
            word = stok(NULL, " \t\r\n", &tok);
        }
    }
    if (direction & HTTP_STAGE_RX && filter->incoming) {
        GRADUATE_LIST(route, inputStages);
        mprAddItem(route->inputStages, filter);
    }
    if (direction & HTTP_STAGE_TX && filter->outgoing) {
        GRADUATE_LIST(route, outputStages);
        if (smatch(name, "cacheFilter") &&
                (pos = mprGetListLength(route->outputStages) - 1) >= 0 &&
                smatch(((HttpStage*) mprGetLastItem(route->outputStages))->name, "chunkFilter")) {
            mprInsertItemAtPos(route->outputStages, pos, filter);
        } else {
            mprAddItem(route->outputStages, filter);
        }
    }
    return 0;
}


PUBLIC int httpAddRouteHandler(HttpRoute *route, cchar *name, cchar *extensions)
{
    HttpStage   *handler;
    char        *extlist, *word, *tok;

    assert(route);

    if ((handler = httpLookupStage(name)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    if (route->handler) {
        mprLog("error http route", 0, "Cannot add handler \"%s\" to route \"%s\" once SetHandler used.",
            handler->name, route->pattern);
    }
    if (!extensions && !handler->match) {
        mprLog("info http route", 2, "Adding handler \"%s\" without extensions to match", handler->name);
    }
    if (extensions) {
        /*
            Add to the handler extension hash. Skip over "*." and "."
         */
        GRADUATE_HASH(route, extensions);
        extlist = sclone(extensions);
        if ((word = stok(extlist, " \t\r\n", &tok)) == 0) {
            mprAddKey(route->extensions, "", handler);
        } else {
            while (word) {
                if (*word == '*' && word[1] == '\0') {
                    word++;
                } else if (*word == '*' && word[1] == '.') {
                    word += 2;
                } else if (*word == '.') {
                    word++;
                } else if (*word == '\"' && word[1] == '\"') {
                    word = "";
                }
                mprAddKey(route->extensions, word, handler);
                word = stok(NULL, " \t\r\n", &tok);
            }
        }
    }
    if (handler->match && mprLookupItem(route->handlers, handler) < 0) {
        GRADUATE_LIST(route, handlers);
        if (smatch(name, "cacheHandler")) {
            mprInsertItemAtPos(route->handlers, 0, handler);
        } else {
            mprAddItem(route->handlers, handler);
        }
    }
    return 0;
}


PUBLIC void httpAddRouteMapping(HttpRoute *route, cchar *extensions, cchar *mappings)
{
    MprList     *mapList;
    cchar       *map;
    char        *etok, *ext, *mtok;
    ssize       len;

    if (extensions == 0) {
        return;
    }
    if (*extensions == '[') {
        extensions = strim(extensions, "[]", 0);
    }
    if (smatch(extensions, "*") || *extensions == '\0') {
        extensions = ".";
    }
    if (!route->map) {
        route->map = mprCreateHash(ME_MAX_ROUTE_MAP_HASH, MPR_HASH_STABLE);
    }
    for (ext = stok(sclone(extensions), ", \t", &etok); ext; ext = stok(NULL, ", \t", &etok)) {
        if (*ext == '.' || *ext == '"' || *ext == '*') {
            ext++;
        }
        len = slen(ext);
        if (ext[len - 1] == '"') {
            ext[len - 1] = '\0';
        }
        mapList = mprCreateList(0, MPR_LIST_STABLE);
        for (map = stok(sclone(mappings), ", \t", &mtok); map; map = stok(NULL, ", \t", &mtok)) {
            mprAddItem(mapList, sreplace(map, "${1}", ext));
        }
        mprAddKey(route->map, ext, mapList);
    }
}


/*
    Param field valuePattern
 */
PUBLIC void httpAddRouteParam(HttpRoute *route, cchar *field, cchar *value, int flags)
{
    HttpRouteOp     *op;
    cchar           *errMsg;
    int             column;

    assert(route);
    assert(field && *field);
    assert(value && *value);

    GRADUATE_LIST(route, params);
    if ((op = createRouteOp(field, flags | HTTP_ROUTE_FREE)) == 0) {
        return;
    }
    if ((op->mdata = pcre_compile2(value, 0, 0, &errMsg, &column, NULL)) == 0) {
        mprLog("error http route", 0, "Cannot compile field pattern. Error %s at column %d", errMsg, column);
    } else {
        op->flags |= HTTP_ROUTE_FREE;
        mprAddItem(route->params, op);
    }
}


/*
    RequestHeader [!] header pattern
 */
PUBLIC void httpAddRouteRequestHeaderCheck(HttpRoute *route, cchar *header, cchar *pattern, int flags)
{
    HttpRouteOp     *op;
    cchar           *errMsg;
    int             column;

    assert(route);
    assert(header && *header);
    assert(pattern && *pattern);

    GRADUATE_LIST(route, requestHeaders);
    if ((op = createRouteOp(header, flags | HTTP_ROUTE_FREE)) == 0) {
        return;
    }
    if ((op->mdata = pcre_compile2(pattern, 0, 0, &errMsg, &column, NULL)) == 0) {
        mprLog("error http route", 0, "Cannot compile header pattern. Error %s at column %d", errMsg, column);
    } else {
        op->flags |= HTTP_ROUTE_FREE;
        mprAddItem(route->requestHeaders, op);
    }
}


/*
    Header [add|append|remove|set] header [value]
 */
PUBLIC void httpAddRouteResponseHeader(HttpRoute *route, int cmd, cchar *header, cchar *value)
{
    MprKeyValue     *pair;
    int             next;

    assert(route);
    assert(header && *header);

    GRADUATE_LIST(route, headers);
    if (cmd == HTTP_ROUTE_REMOVE_HEADER) {
        /*
            Remove existing route headers, but keep the remove record so that user headers will be removed too
         */
        for (ITERATE_ITEMS(route->headers, pair, next)) {
            if (smatch(pair->key, header)) {
                mprRemoveItem(route->headers, pair);
                next--;
            }
        }
    }
    mprAddItem(route->headers, mprCreateKeyPair(header, value, cmd));
}


/*
    Add a route update record. These run to modify a request.
        Update rule var value
        rule == "cmd|param"
        details == "var value"
    Value can contain pattern and request tokens.
 */
PUBLIC int httpAddRouteUpdate(HttpRoute *route, cchar *rule, cchar *details, int flags)
{
    HttpRouteOp *op;
    char        *value;

    assert(route);
    assert(rule && *rule);

    GRADUATE_LIST(route, updates);
    if ((op = createRouteOp(rule, flags)) == 0) {
        return MPR_ERR_MEMORY;
    }
    if (scaselessmatch(rule, "cmd")) {
        op->details = sclone(details);

    } else if (scaselessmatch(rule, "lang")) {
        /* Nothing to do */;

    } else if (scaselessmatch(rule, "param")) {
        if (!httpTokenize(route, details, "%S %S", &op->var, &value)) {
            return MPR_ERR_BAD_SYNTAX;
        }
        op->value = finalizeReplacement(route, value);

    } else {
        return MPR_ERR_BAD_SYNTAX;
    }
    addUniqueItem(route->updates, op);
    return 0;
}


PUBLIC void httpClearRouteStages(HttpRoute *route, int direction)
{
    assert(route);

    if (direction & HTTP_STAGE_RX) {
        route->inputStages = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (direction & HTTP_STAGE_TX) {
        route->outputStages = mprCreateList(-1, MPR_LIST_STABLE);
    }
}


PUBLIC void httpDefineRouteTarget(cchar *key, HttpRouteProc *proc)
{
    assert(key && *key);
    assert(proc);

    mprAddKey(HTTP->routeTargets, key, proc);
}


PUBLIC void httpDefineRouteCondition(cchar *key, HttpRouteProc *proc)
{
    assert(key && *key);
    assert(proc);

    mprAddKey(HTTP->routeConditions, key, proc);
}


PUBLIC void httpDefineRouteUpdate(cchar *key, HttpRouteProc *proc)
{
    assert(key && *key);
    assert(proc);

    mprAddKey(HTTP->routeUpdates, key, proc);
}


PUBLIC void *httpGetRouteData(HttpRoute *route, cchar *key)
{
    assert(route);
    assert(key && *key);

    if (!route->data) {
        return 0;
    }
    return mprLookupKey(route->data, key);
}


PUBLIC cchar *httpGetRouteDocuments(HttpRoute *route)
{
    assert(route);
    return route->documents;
}


PUBLIC cchar *httpGetRouteHome(HttpRoute *route)
{
    assert(route);
    return route->home;
}


PUBLIC cchar *httpGetRouteMethods(HttpRoute *route)
{
    assert(route);
    assert(route->methods);
    return mprHashKeysToString(route->methods, ",");
}


PUBLIC void httpResetRoutePipeline(HttpRoute *route)
{
    assert(route);

    if (!route->parent || route->caching != route->parent->caching) {
        route->caching = 0;
    }
    if (!route->parent || route->errorDocuments != route->parent->errorDocuments) {
        route->errorDocuments = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STABLE);
    }
    if (!route->parent || route->extensions != route->parent->extensions) {
        route->extensions = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
    }
    if (!route->parent || route->handlers != route->parent->handlers) {
        route->handlers = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (!route->parent || route->inputStages != route->parent->inputStages) {
        route->inputStages = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (!route->parent || route->indexes != route->parent->indexes) {
        route->indexes = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (!route->parent || route->outputStages != route->parent->outputStages) {
        route->outputStages = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (!route->parent || route->methods != route->parent->methods) {
        route->methods = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        httpAddRouteMethods(route, NULL);
    }
    if (!route->parent || route->requestHeaders != route->parent->requestHeaders) {
        route->requestHeaders = 0;
    }
    if (!route->parent || route->params != route->parent->params) {
        route->params = 0;
    }
    if (!route->parent || route->updates != route->parent->updates) {
        route->updates = 0;
    }
    if (!route->parent || route->conditions != route->parent->conditions) {
        route->conditions = 0;
    }
    if (!route->parent || route->map != route->parent->map) {
        route->map = 0;
    }
    if (!route->parent || route->languages != route->parent->languages) {
        route->languages = 0;
    }
    if (!route->parent || route->headers != route->parent->headers) {
        route->headers = 0;
#if FUTURE
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "Content-Security-Policy", "default-src 'self'");
#endif
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-XSS-Protection", "1; mode=block");
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-Frame-Options", "SAMEORIGIN");
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-Content-Type-Options", "nosniff");
    }
}


PUBLIC void httpResetHandlers(HttpRoute *route)
{
    assert(route);
    route->handlers = mprCreateList(-1, MPR_LIST_STABLE);
}


PUBLIC void httpSetRouteAuth(HttpRoute *route, HttpAuth *auth)
{
    assert(route);
    route->auth = auth;
}


PUBLIC void httpSetRouteAutoDelete(HttpRoute *route, bool enable)
{
    assert(route);
    route->autoDelete = enable;
}


PUBLIC void httpSetRouteRenameUploads(HttpRoute *route, bool enable)
{
    assert(route);
    route->renameUploads = enable;
}


PUBLIC int httpSetRouteConnector(HttpRoute *route, cchar *name)
{
    HttpStage     *stage;

    assert(route);

    stage = httpLookupStage(name);
    if (stage == 0) {
        mprLog("error http route", 0, "Cannot find connector %s", name);
        return MPR_ERR_CANT_FIND;
    }
    route->connector = stage;
    return 0;
}


PUBLIC void httpSetRouteData(HttpRoute *route, cchar *key, void *data)
{
    assert(route);
    assert(key && *key);
    assert(data);

    if (route->data == 0) {
        route->data = mprCreateHash(-1, 0);
    } else {
        GRADUATE_HASH(route, data);
    }
    mprAddKey(route->data, key, data);
}


PUBLIC void httpSetRouteDocuments(HttpRoute *route, cchar *path)
{
    httpSetDir(route, "DOCUMENTS", path);
}


PUBLIC void httpSetRouteFlags(HttpRoute *route, int flags)
{
    assert(route);
    route->flags = flags;
}


PUBLIC void httpSetRouteEnvEscape(HttpRoute *route, bool on)
{
    route->flags &= ~(HTTP_ROUTE_ENV_ESCAPE);
    if (on) {
        route->flags |= HTTP_ROUTE_ENV_ESCAPE;
    }
}


PUBLIC void httpSetRouteEnvPrefix(HttpRoute *route, cchar *prefix)
{
    route->envPrefix = sclone(prefix);
}


PUBLIC int httpSetRouteHandler(HttpRoute *route, cchar *name)
{
    HttpStage     *handler;

    assert(route);
    assert(name && *name);

    if ((handler = httpLookupStage(name)) == 0) {
        mprLog("error http route", 0, "Cannot find handler %s", name);
        return MPR_ERR_CANT_FIND;
    }
    route->handler = handler;
    return 0;
}


PUBLIC void httpSetRouteHome(HttpRoute *route, cchar *path)
{
    httpSetDir(route, "HOME", path);
}


/*
    WARNING: internal API only.
 */
PUBLIC void httpSetRouteHost(HttpRoute *route, HttpHost *host)
{
    assert(route);
    assert(host);

    route->host = host;
    defineHostVars(route);
}


PUBLIC void httpSetRouteIgnoreEncodingErrors(HttpRoute *route, bool on)
{
    route->ignoreEncodingErrors = on;
}


PUBLIC void httpAddRouteIndex(HttpRoute *route, cchar *index)
{
    cchar   *item;
    int     next;

    assert(route);
    assert(index && *index);

    GRADUATE_LIST(route, indexes);
    for (ITERATE_ITEMS(route->indexes, item, next)) {
        if (smatch(index, item)) {
            return;
        }
    }
    mprAddItem(route->indexes, sclone(index));
}


PUBLIC void httpAddRouteMethods(HttpRoute *route, cchar *methods)
{
    char    *method, *tok;

    assert(route);

    if (methods == NULL || *methods == '\0') {
        methods = ME_HTTP_DEFAULT_METHODS;
    } else if (scaselessmatch(methods, "ALL")) {
       methods = "*";
    } else if (*methods == '[') {
        methods = strim(methods, "[]", 0);
    }
    if (!route->methods || (route->parent && route->methods == route->parent->methods)) {
        GRADUATE_HASH(route, methods);
    }
    tok = sclone(methods);
    while ((method = stok(tok, ", \t\n\r", &tok)) != 0) {
        mprAddKey(route->methods, method, LTOP(1));
    }
}


PUBLIC void httpRemoveRouteMethods(HttpRoute *route, cchar *methods)
{
    char    *method, *tok;

    assert(route);
    tok = sclone(methods);
    while ((method = stok(tok, ", \t\n\r", &tok)) != 0) {
        mprRemoveKey(route->methods, method);
    }
}


PUBLIC void httpResetRouteIndexes(HttpRoute *route)
{
    route->indexes = mprCreateList(-1, MPR_LIST_STABLE);
}


PUBLIC void httpSetRouteMethods(HttpRoute *route, cchar *methods)
{
    route->methods = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
    httpAddRouteMethods(route, methods);
}


PUBLIC void httpSetRouteCookie(HttpRoute *route, cchar *cookie)
{
    assert(route);
    assert(cookie && *cookie);
    route->cookie = sclone(cookie);
}


PUBLIC void httpSetRouteCookiePersist(HttpRoute *route, int enable)
{
    route->flags &= ~HTTP_ROUTE_PERSIST_COOKIE;
    if (enable) {
        route->flags |= HTTP_ROUTE_PERSIST_COOKIE;
    }
}


PUBLIC void httpSetRoutePattern(HttpRoute *route, cchar *pattern, int flags)
{
    assert(route);
    assert(pattern);

    route->flags |= (flags & HTTP_ROUTE_NOT);
    route->pattern = sclone(pattern);
    finalizePattern(route);
}


/*
    Set the prefix to empty if no prefix
 */
PUBLIC void httpSetRoutePrefix(HttpRoute *route, cchar *prefix)
{
    assert(route);

    if (prefix && *prefix) {
        if (smatch(prefix, "/")) {
            route->prefix = MPR->emptyString;
            route->prefixLen = 0;
        } else {
            route->prefix = sclone(prefix);
            route->prefixLen = slen(prefix);
            httpSetRouteVar(route, "PREFIX", prefix);
        }
    } else {
        route->prefix = MPR->emptyString;
        route->prefixLen = 0;
        httpSetRouteVar(route, "PREFIX", "");
    }
    if (route->pattern) {
        finalizePattern(route);
    }
    assert(route->prefix);
}


PUBLIC void httpSetRoutePreserveFrames(HttpRoute *route, bool on)
{
    route->flags &= ~HTTP_ROUTE_PRESERVE_FRAMES;
    if (on) {
        route->flags |= HTTP_ROUTE_PRESERVE_FRAMES;
    }
}


#if DEPRECATE
PUBLIC void httpSetRouteServerPrefix(HttpRoute *route, cchar *prefix)
{
    assert(route);
    assert(!smatch(prefix, "/"));

    if (prefix && *prefix) {
        if (smatch(prefix, "/")) {
            route->serverPrefix = MPR->emptyString;
        } else {
            route->serverPrefix = sclone(prefix);
        }
    } else {
        route->serverPrefix = MPR->emptyString;
    }
    assert(route->serverPrefix);
}
#endif


PUBLIC void httpSetRouteSessionVisibility(HttpRoute *route, bool visible)
{
    route->flags &= ~HTTP_ROUTE_VISIBLE_SESSION;
    if (visible) {
        route->flags |= HTTP_ROUTE_VISIBLE_SESSION;
    }
}


PUBLIC void httpSetRouteShowErrors(HttpRoute *route, bool on)
{
    route->flags &= ~HTTP_ROUTE_SHOW_ERRORS;
    if (on) {
        route->flags |= HTTP_ROUTE_SHOW_ERRORS;
    }
}


PUBLIC void httpSetRouteSource(HttpRoute *route, cchar *source)
{
    assert(route);
    assert(source);

    /* Source can be empty */
    route->sourceName = sclone(source);
}


PUBLIC void httpSetRouteScript(HttpRoute *route, cchar *script, cchar *scriptPath)
{
    assert(route);

    if (script) {
        assert(*script);
        route->script = sclone(script);
    }
    if (scriptPath) {
        assert(*scriptPath);
        route->scriptPath = sclone(scriptPath);
    }
}


PUBLIC void httpSetRouteStealth(HttpRoute *route, bool on)
{
    route->flags &= ~HTTP_ROUTE_STEALTH;
    if (on) {
        route->flags |= HTTP_ROUTE_STEALTH;
    }
}


/*
    Target names are extensible and hashed in http->routeTargets.

        Target close
        Target redirect status [URI]
        Target run ${DOCUMENTS}/${request:uri}.gz
        Target run ${controller}-${action}
        Target write [-r] status "Hello World\r\n"
 */
PUBLIC int httpSetRouteTarget(HttpRoute *route, cchar *rule, cchar *details)
{
    char    *redirect, *msg;

    assert(route);
    assert(rule && *rule);

    route->targetRule = sclone(rule);
    route->target = sclone(details);

    if (scaselessmatch(rule, "close")) {
        route->target = sclone(details);

    } else if (scaselessmatch(rule, "redirect")) {
        if (!httpTokenize(route, details, "%N ?S", &route->responseStatus, &redirect)) {
            return MPR_ERR_BAD_SYNTAX;
        }
        route->target = finalizeReplacement(route, redirect);
        return 0;

    } else if (scaselessmatch(rule, "run")) {
        route->target = finalizeReplacement(route, details);

    } else if (scaselessmatch(rule, "write")) {
        /*
            Write [-r] status Message
         */
        if (sncmp(details, "-r", 2) == 0) {
            route->flags |= HTTP_ROUTE_RAW;
            details = &details[2];
        }
        if (!httpTokenize(route, details, "%N %S", &route->responseStatus, &msg)) {
            return MPR_ERR_BAD_SYNTAX;
        }
        route->target = finalizeReplacement(route, msg);

    } else {
        return MPR_ERR_BAD_SYNTAX;
    }
    return 0;
}


PUBLIC void httpSetRouteTemplate(HttpRoute *route, cchar *tplate)
{
    assert(route);
    assert(tplate && *tplate);

    route->tplate = sclone(tplate);
}


PUBLIC void httpSetRouteUploadDir(HttpRoute *route, cchar *dir)
{
    httpSetDir(route, "UPLOAD", dir);
}


PUBLIC void httpSetRouteWorkers(HttpRoute *route, int workers)
{
    assert(route);
    route->workers = workers;
}


PUBLIC void httpAddRouteErrorDocument(HttpRoute *route, int status, cchar *url)
{
    char    *code;

    assert(route);
    GRADUATE_HASH(route, errorDocuments);
    code = itos(status);
    mprAddKey(route->errorDocuments, code, sclone(url));
}


PUBLIC cchar *httpLookupRouteErrorDocument(HttpRoute *route, int code)
{
    char   *num;

    assert(route);
    if (route->errorDocuments == 0) {
        return 0;
    }
    num = itos(code);
    return (cchar*) mprLookupKey(route->errorDocuments, num);
}


/*
    Finalize the pattern.
        - Change "\{n[:m]}" to "{n[:m]}"
        - Change "\~" to "~"
        - Change "(~ PAT ~)" to "(?: PAT )?"
        - Extract the tokens and change tokens: "{word}" to "([^/]*)"
 */
static void finalizePattern(HttpRoute *route)
{
    MprBuf      *pattern;
    cchar       *errMsg;
    char        *startPattern, *cp, *ep, *token, *field;
    ssize       len;
    int         column;

    assert(route);
    route->tokens = mprCreateList(-1, MPR_LIST_STABLE);
    pattern = mprCreateBuf(-1, -1);
    startPattern = route->pattern[0] == '^' ? &route->pattern[1] : route->pattern;

    if (route->tplate == 0) {
        /* Do this while the prefix is still in the route pattern */
        route->tplate = finalizeTemplate(route);
    }
    /*
        Create an simple literal startWith string to optimize route rejection.
     */
    len = strcspn(startPattern, "^$*+?.(|{[\\");
    if (len) {
        /* Handle /pattern / * */
        if (startPattern[len] == '*' && len > 0) {
            len--;
        }
        route->startWith = snclone(startPattern, len);
        route->startWithLen = len;
        if ((cp = strchr(&route->startWith[1], '/')) != 0) {
            route->startSegment = snclone(route->startWith, cp - route->startWith);
        } else {
            route->startSegment = route->startWith;
        }
        route->startSegmentLen = slen(route->startSegment);
    } else {
        /* Pattern has special characters */
        route->startWith = 0;
        route->startWithLen = 0;
        route->startSegmentLen = 0;
        route->startSegment = 0;
    }

    /*
        Remove the route prefix from the start of the compiled pattern.
     */
    if (route->prefix && *route->prefix && sstarts(startPattern, route->prefix)) {
        assert(route->prefixLen <= route->startWithLen);
        startPattern = sfmt("^%s", &startPattern[route->prefixLen]);
    } else {
        startPattern = sjoin("^", startPattern, NULL);
    }
    for (cp = startPattern; *cp; cp++) {
        /* Alias for optional, non-capturing pattern:  "(?: PAT )?" */
        if (*cp == '(' && cp[1] == '~') {
            mprPutStringToBuf(pattern, "(?:");
            cp++;

        } else if (*cp == '(') {
            mprPutCharToBuf(pattern, *cp);
        } else if (*cp == '~' && cp[1] == ')') {
            mprPutStringToBuf(pattern, ")?");
            cp++;

        } else if (*cp == ')') {
            mprPutCharToBuf(pattern, *cp);

        } else if (*cp == '{') {
            if (cp > startPattern&& cp[-1] == '\\') {
                mprAdjustBufEnd(pattern, -1);
                mprPutCharToBuf(pattern, *cp);
            } else {
                if ((ep = schr(cp, '}')) != 0) {
                    /* Trim {} off the token and replace in pattern with "([^/]*)"  */
                    token = snclone(&cp[1], ep - cp - 1);
                    if ((field = schr(token, '=')) != 0) {
                        *field++ = '\0';
                        field = sfmt("(%s)", field);
                    } else {
                        field = "([^/]*)";
                    }
                    mprPutStringToBuf(pattern, field);
                    mprAddItem(route->tokens, token);
                    /* Params ends up looking like "$1:$2:$3:$4" */
                    cp = ep;
                }
            }
        } else if (*cp == '\\' && *cp == '~') {
            mprPutCharToBuf(pattern, *++cp);

        } else {
            mprPutCharToBuf(pattern, *cp);
        }
    }
    mprAddNullToBuf(pattern);
    route->optimizedPattern = sclone(mprGetBufStart(pattern));
    if (mprGetListLength(route->tokens) == 0) {
        route->tokens = 0;
    }
    if (route->patternCompiled && (route->flags & HTTP_ROUTE_FREE_PATTERN)) {
        free(route->patternCompiled);
    }
    if ((route->patternCompiled = pcre_compile2(route->optimizedPattern, 0, 0, &errMsg, &column, NULL)) == 0) {
        mprLog("error http route", 0, "Cannot compile route. Error %s at column %d", errMsg, column);
    }
    route->flags |= HTTP_ROUTE_FREE_PATTERN;
}


static char *finalizeReplacement(HttpRoute *route, cchar *str)
{
    MprBuf      *buf;
    cchar       *item;
    cchar       *tok, *cp, *ep, *token;
    int         next, braced;

    assert(route);

    /*
        Prepare a replacement string. Change $token to $N
     */
    buf = mprCreateBuf(-1, -1);
    if (str && *str) {
        for (cp = str; *cp; cp++) {
            if ((tok = schr(cp, '$')) != 0 && (tok == str || tok[-1] != '\\')) {
                if (tok > cp) {
                    mprPutBlockToBuf(buf, cp, tok - cp);
                }
                if ((braced = (*++tok == '{')) != 0) {
                    tok++;
                }
                if (*tok == '&' || *tok == '\'' || *tok == '`' || *tok == '$') {
                    mprPutCharToBuf(buf, '$');
                    mprPutCharToBuf(buf, *tok);
                    ep = tok + 1;
                } else {
                    if (braced) {
                        for (ep = tok; *ep && *ep != '}'; ep++) ;
                    } else {
                        for (ep = tok; *ep && isdigit((uchar) *ep); ep++) ;
                    }
                    token = snclone(tok, ep - tok);
                    if (schr(token, ':') || schr(token, '.')) {
                        /* Double quote to get through two levels of expansion in writeTarget */
                        mprPutStringToBuf(buf, "$${");
                        mprPutStringToBuf(buf, token);
                        mprPutCharToBuf(buf, '}');
                    } else {
                        for (next = 0; (item = mprGetNextItem(route->tokens, &next)) != 0; ) {
                            if (scmp(item, token) == 0) {
                                break;
                            }
                        }
                        /*  Insert "$" in front of "{token}" */
                        if (item) {
                            mprPutCharToBuf(buf, '$');
                            mprPutIntToBuf(buf, next);
                        } else if (snumber(token)) {
                            mprPutCharToBuf(buf, '$');
                            mprPutStringToBuf(buf, token);
                        } else {
                            mprLog("error http route", 0, "Cannot find token \"%s\" in template \"%s\"",
                                token, route->pattern);
                        }
                    }
                }
                if (braced) {
                    ep++;
                }
                cp = ep - 1;

            } else {
                if (*cp == '\\') {
                    if (cp[1] == 'r') {
                        mprPutCharToBuf(buf, '\r');
                        cp++;
                    } else if (cp[1] == 'n') {
                        mprPutCharToBuf(buf, '\n');
                        cp++;
                    } else {
                        mprPutCharToBuf(buf, *cp);
                    }
                } else {
                    mprPutCharToBuf(buf, *cp);
                }
            }
        }
    }
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


/*
    Convert a route pattern into a usable template to construct URI links
    NOTE: this is heuristic and not perfect. Users can define the template via the httpSetTemplate API or in appweb via the
    EspURITemplate configuration directive.
 */
static char *finalizeTemplate(HttpRoute *route)
{
    MprBuf  *buf;
    char    *sp, *tplate;

    if ((buf = mprCreateBuf(0, 0)) == 0) {
        return 0;
    }
    /*
        Note: the route->pattern includes the prefix
     */
    for (sp = route->pattern; *sp; sp++) {
        switch (*sp) {
        default:
            mprPutCharToBuf(buf, *sp);
            break;
        case '$':
            if (sp[1]) {
                mprPutCharToBuf(buf, *sp);
            }
            break;
        case '^':
            if (sp > route->pattern) {
                mprPutCharToBuf(buf, *sp);
            }
            break;
        case '+':
        case '?':
        case '|':
        case '[':
        case ']':
        case '*':
        case '.':
            break;
        case '(':
            if (sp[1] == '~') {
                sp++;
            }
            break;
        case '~':
            if (sp[1] == ')') {
                sp++;
            } else {
                mprPutCharToBuf(buf, *sp);
            }
            break;
        case ')':
            break;
        case '\\':
            if (sp[1] == '\\') {
                mprPutCharToBuf(buf, *sp++);
            } else {
                mprPutCharToBuf(buf, *++sp);
            }
            break;
        case '{':
            mprPutCharToBuf(buf, '$');
            while (sp[1] && *sp != '}') {
                if (*sp == '=') {
                    while (sp[1] && *sp != '}') sp++;
                } else {
                    mprPutCharToBuf(buf, *sp++);
                }
            }
            mprPutCharToBuf(buf, '}');
            break;
        }
    }
    if (mprLookAtLastCharInBuf(buf) == '/') {
        mprAdjustBufEnd(buf, -1);
    }
    mprAddNullToBuf(buf);
    if (mprGetBufLength(buf) > 0) {
        tplate = sclone(mprGetBufStart(buf));
    } else {
        tplate = sclone("/");
    }
    return tplate;
}


PUBLIC void httpFinalizeRoute(HttpRoute *route)
{
    /*
        Add the route to the owning host. When using an Appweb configuration file, the order of route finalization
        will be from the inside out. This ensures that nested routes are defined BEFORE outer/enclosing routes.
        This is important as requests process routes in-order.
     */
    assert(route);
    if (mprGetListLength(route->indexes) == 0) {
        mprAddItem(route->indexes,  sclone("index.html"));
    }
    httpAddRoute(route->host, route);
}


PUBLIC cchar *httpGetRouteTop(HttpConn *conn)
{
    HttpRx  *rx;
    char   *pp, *top;
    int     count;

    rx = conn->rx;
    if (sstarts(rx->pathInfo, rx->route->prefix)) {
        pp = &rx->pathInfo[rx->route->prefixLen];
    } else {
        pp = rx->pathInfo;
    }
    top = MPR->emptyString;
    for (count = 0; *pp; pp++) {
        if (*pp == '/' && count++ > 0) {
            top = sjoin(top, "../", NULL);
        }
    }
    if (*top) {
        top[slen(top) - 1] = '\0';
    }
    return top;
}


/*
    Expect a template with embedded tokens of the form: "/${controller}/${action}/${other}"
    Understands the following aliases:
        ~   For ${PREFIX}
    The options is a hash of token values.
 */
PUBLIC char *httpTemplate(HttpConn *conn, cchar *template, MprHash *options)
{
    MprBuf      *buf;
    HttpRx      *rx;
    HttpRoute   *route;
    cchar       *cp, *ep, *value;
    char        key[ME_MAX_BUFFER];

    rx = conn->rx;
    route = rx->route;
    if (template == 0 || *template == '\0') {
        return MPR->emptyString;
    }
    buf = mprCreateBuf(-1, -1);
    for (cp = template; *cp; cp++) {
        if (cp == template && *cp == '~') {
            mprPutStringToBuf(buf, httpGetRouteTop(conn));
#if DEPRECATE
        } else if (cp == template && *cp == '|') {
            mprPutStringToBuf(buf, route->prefix);
            mprPutStringToBuf(buf, route->serverPrefix);
#endif

        } else if (*cp == '$' && cp[1] == '{' && (cp == template || cp[-1] != '\\')) {
            cp += 2;
            if ((ep = strchr(cp, '}')) != 0) {
                sncopy(key, sizeof(key), cp, ep - cp);
                if (options && (value = httpGetOption(options, key, 0)) != 0) {
                    mprPutStringToBuf(buf, value);

                } else if ((value = mprReadJson(rx->params, key)) != 0) {
                    mprPutStringToBuf(buf, value);

                } else if ((value = mprLookupKey(route->vars, key)) != 0) {
                    mprPutStringToBuf(buf, value);
                }
                if (value == 0) {
                    /* Just emit the token name if the token cannot be found */
                    mprPutStringToBuf(buf, key);
                }
                cp = ep;
            }
        } else {
            mprPutCharToBuf(buf, *cp);
        }
    }
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


PUBLIC void httpSetRouteVar(HttpRoute *route, cchar *key, cchar *value)
{
    assert(route);
    assert(key);

    GRADUATE_HASH(route, vars);
    if (schr(value, '$')) {
        value = stemplate(value, route->vars);
    }
    mprAddKey(route->vars, key, sclone(value));
}


PUBLIC cchar *httpGetRouteVar(HttpRoute *route, cchar *key)
{
    return mprLookupKey(route->vars, key);
}


PUBLIC cchar *httpExpandRouteVars(HttpRoute *route, cchar *str)
{
    return stemplate(str, route->vars);
}


/*
    Make a path name. This replaces $references, converts to an absolute path name, cleans the path and maps delimiters.
    Paths are resolved relative to the given directory or route home if "dir" is null.
 */
PUBLIC char *httpMakePath(HttpRoute *route, cchar *dir, cchar *path)
{
    assert(route);
    assert(path);

    if ((path = stemplate(path, route->vars)) == 0) {
        return 0;
    }
    if (mprIsPathRel(path)) {
        path = mprJoinPath(dir ? dir : route->home, path);
    }
    return mprGetAbsPath(path);
}


PUBLIC void httpSetRouteXsrf(HttpRoute *route, bool enable)
{
    route->flags &= ~HTTP_ROUTE_XSRF;
    if (enable) {
        route->flags |= HTTP_ROUTE_XSRF;
    }
}

/********************************* Language ***********************************/
/*
    Language can be an empty string
 */
PUBLIC int httpAddRouteLanguageSuffix(HttpRoute *route, cchar *language, cchar *suffix, int flags)
{
    HttpLang    *lp;

    assert(route);
    assert(language);
    assert(suffix && *suffix);

    if (route->languages == 0) {
        route->languages = mprCreateHash(-1, MPR_HASH_STABLE);
    } else {
        GRADUATE_HASH(route, languages);
    }
    if ((lp = mprLookupKey(route->languages, language)) != 0) {
        lp->suffix = sclone(suffix);
        lp->flags = flags;
    } else {
        mprAddKey(route->languages, language, createLangDef(0, suffix, flags));
    }
    return httpAddRouteUpdate(route, "lang", 0, 0);
}


PUBLIC int httpAddRouteLanguageDir(HttpRoute *route, cchar *language, cchar *path)
{
    HttpLang    *lp;

    assert(route);
    assert(language && *language);
    assert(path && *path);

    if (route->languages == 0) {
        route->languages = mprCreateHash(-1, MPR_HASH_STABLE);
    } else {
        GRADUATE_HASH(route, languages);
    }
    if ((lp = mprLookupKey(route->languages, language)) != 0) {
        lp->path = sclone(path);
    } else {
        mprAddKey(route->languages, language, createLangDef(path, 0, 0));
    }
    return httpAddRouteUpdate(route, "lang", 0, 0);
}


PUBLIC void httpSetRouteDefaultLanguage(HttpRoute *route, cchar *language)
{
    assert(route);
    assert(language && *language);

    route->defaultLanguage = sclone(language);
}


/********************************* Conditions *********************************/

static int testCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *condition)
{
    HttpRouteProc   *proc;

    assert(conn);
    assert(route);
    assert(condition);

    if ((proc = mprLookupKey(conn->http->routeConditions, condition->name)) == 0) {
        httpError(conn, -1, "Cannot find route condition rule %s", condition->name);
        return 0;
    }
    return (*proc)(conn, route, condition);
}


/*
    Allow/Deny authorization
 */
static int allowDenyCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpRx      *rx;
    HttpAuth    *auth;
    int         allow, deny;

    assert(conn);
    assert(route);

    rx = conn->rx;
    auth = rx->route->auth;
    if (auth == 0) {
        return HTTP_ROUTE_OK;
    }
    allow = 0;
    deny = 0;
    if (auth->flags & HTTP_ALLOW_DENY) {
        if (auth->allow && mprLookupKey(auth->allow, conn->ip)) {
            allow++;
        } else {
            allow++;
        }
        if (auth->deny && mprLookupKey(auth->deny, conn->ip)) {
            deny++;
        }
        if (!allow || deny) {
            httpError(conn, HTTP_CODE_FORBIDDEN, "Access denied for this server %s", conn->ip);
            return HTTP_ROUTE_OK;
        }
    } else {
        if (auth->deny && mprLookupKey(auth->deny, conn->ip)) {
            deny++;
        }
        if (auth->allow && !mprLookupKey(auth->allow, conn->ip)) {
            deny = 0;
            allow++;
        } else {
            allow++;
        }
        if (deny || !allow) {
            httpError(conn, HTTP_CODE_FORBIDDEN, "Access denied for this server %s", conn->ip);
            return HTTP_ROUTE_OK;
        }
    }
    return HTTP_ROUTE_OK;
}


/*
    This condition is used to implement all user authentication for routes
 */
static int authCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpAuth    *auth;
    cchar       *username, *password;

    assert(conn);
    assert(route);

    auth = route->auth;
    if (!auth || !auth->type) {
        /* Authentication not required */
        return HTTP_ROUTE_OK;
    }
    if (!httpIsAuthenticated(conn)) {
        if (!httpGetCredentials(conn, &username, &password) || !httpLogin(conn, username, password)) {
            if (!conn->tx->finalized) {
                if (auth && auth->type) {
                    (auth->type->askLogin)(conn);
                } else {
                    httpError(conn, HTTP_CODE_UNAUTHORIZED, "Access Denied, login required");
                }
            }
            /* 
                Request has been denied and a response generated. So OK to accept this route. 
             */
            return HTTP_ROUTE_OK;
        }
    }
    if (!httpCanUser(conn, NULL)) {
        httpTrace(conn, "auth.check", "error", "msg:'Access denied, user is not authorized for access'");
        if (!conn->tx->finalized) {
            httpError(conn, HTTP_CODE_FORBIDDEN, "Access denied. User is not authorized for access.");
            /* Request has been denied and a response generated. So OK to accept this route. */
        }
    }
    /* 
        OK to accept route. This does not mean the request was authenticated - an error may have been already generated 
     */
    return HTTP_ROUTE_OK;
}


/*
    This condition is used for "Condition unauthorized"
 */
static int unauthorizedCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpAuth    *auth;
    cchar       *username, *password;

    auth = route->auth;
    if (!auth || !auth->type) {
        return HTTP_ROUTE_REJECT;
    }
    if (httpIsAuthenticated(conn)) {
        return HTTP_ROUTE_REJECT;
    }
    if (httpGetCredentials(conn, &username, &password) && httpLogin(conn, username, password)) {
        return HTTP_ROUTE_REJECT;
    }
    return HTTP_ROUTE_OK;
}


/*
    Test if the condition parameters evaluate to a directory
 */
static int directoryCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpTx      *tx;
    MprPath     info;
    char        *path;

    assert(conn);
    assert(route);
    assert(op);
    tx = conn->tx;

    /*
        Must have tx->filename set when expanding op->details, so map target now.
        Then reset the filename and extension.
     */
    httpMapFile(conn);
    path = mprJoinPath(route->documents, expandTokens(conn, op->details));
    tx->ext = tx->filename = 0;

    mprGetPathInfo(path, &info);
    if (info.isDir) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


/*
    Test if a file exists
 */
static int existsCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpTx  *tx;
    char    *path;

    assert(conn);
    assert(route);
    assert(op);

    /*
        Must have tx->filename set when expanding op->details, so map target now
     */
    tx = conn->tx;
    httpMapFile(conn);
    path = mprJoinPath(route->documents, expandTokens(conn, op->details));
    tx->ext = tx->filename = 0;

    if (mprPathExists(path, R_OK)) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


static int matchCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    char    *str;
    int     matched[ME_MAX_ROUTE_MATCHES * 2], count;

    assert(conn);
    assert(route);
    assert(op);

    str = expandTokens(conn, op->details);
    count = pcre_exec(op->mdata, NULL, str, (int) slen(str), 0, 0, matched, sizeof(matched) / sizeof(int));
    if (count > 0) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


/*
    Test if the connection is secure
    Set op->details to a non-zero "age" to emit a Strict-Transport-Security header
    A negative age signifies to "includeSubDomains"
 */
static int secureCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    int64       age;

    assert(conn);
    if (op->flags & HTTP_ROUTE_STRICT_TLS) {
        /* Negative age means subDomains == true */
        age = stoi(op->details);
        if (age < 0) {
            httpAddHeader(conn, "Strict-Transport-Security", "max-age=%lld; includeSubDomains", -age / TPS);
        } else if (age > 0) {
            httpAddHeader(conn, "Strict-Transport-Security", "max-age=%lld", age / TPS);
        }
    }
    if (op->flags & HTTP_ROUTE_REDIRECT) {
        if (!conn->secure) {
            assert(op->details && *op->details);
            httpRedirect(conn, HTTP_CODE_MOVED_PERMANENTLY, expandTokens(conn, op->details));
        }
        return HTTP_ROUTE_OK;
    }
    if (!conn->secure) {
        return HTTP_ROUTE_REJECT;
    }
    return HTTP_ROUTE_OK;
}

/********************************* Updates ******************************/

static int updateRequest(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpRouteProc   *proc;

    assert(conn);
    assert(route);
    assert(op);

    if ((proc = mprLookupKey(conn->http->routeUpdates, op->name)) == 0) {
        httpError(conn, -1, "Cannot find route update rule %s", op->name);
        return HTTP_ROUTE_OK;
    }
    return (*proc)(conn, route, op);
}


static int cmdUpdate(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    MprCmd  *cmd;
    char    *command, *out, *err;
    int     status;

    assert(conn);
    assert(route);
    assert(op);

    command = expandTokens(conn, op->details);
    cmd = mprCreateCmd(conn->dispatcher);
    httpTrace(conn, "request.run", "context", "command:'%s'", command);
    if ((status = mprRunCmd(cmd, command, NULL, NULL, &out, &err, -1, 0)) != 0) {
        /* Don't call httpError, just set errorMsg which can be retrieved via: ${request:error} */
        conn->errorMsg = sfmt("Command failed: %s\nStatus: %d\n%s\n%s", command, status, out, err);
        httpTrace(conn, "request.run.error", "error", "command:'%s',error:'%s'", command, conn->errorMsg);
        /* Continue */
    }
    mprDestroyCmd(cmd);
    return HTTP_ROUTE_OK;
}


static int paramUpdate(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    assert(conn);
    assert(route);
    assert(op);

    httpSetParam(conn, op->var, expandTokens(conn, op->value));
    return HTTP_ROUTE_OK;
}


static int langUpdate(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpUri     *prior;
    HttpRx      *rx;
    HttpLang    *lang;
    char        *ext, *pathInfo, *uri;

    assert(conn);
    assert(route);

    rx = conn->rx;
    prior = rx->parsedUri;
    assert(route->languages);

    if ((lang = httpGetLanguage(conn, route->languages, 0)) != 0) {
        rx->lang = lang;
        if (lang->suffix) {
            pathInfo = 0;
            if (lang->flags & HTTP_LANG_AFTER) {
                pathInfo = sjoin(rx->pathInfo, ".", lang->suffix, NULL);
            } else if (lang->flags & HTTP_LANG_BEFORE) {
                ext = httpGetExt(conn);
                if (ext && *ext) {
                    pathInfo = sjoin(mprJoinPathExt(mprTrimPathExt(rx->pathInfo), lang->suffix), ".", ext, NULL);
                } else {
                    pathInfo = mprJoinPathExt(mprTrimPathExt(rx->pathInfo), lang->suffix);
                }
            }
            if (pathInfo) {
                uri = httpFormatUri(prior->scheme, prior->host, prior->port, pathInfo, prior->reference, prior->query, 0);
                httpSetUri(conn, uri);
            }
        }
    }
    return HTTP_ROUTE_OK;
}


/*********************************** Targets **********************************/

static int closeTarget(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    assert(conn);
    assert(route);

    httpError(conn, HTTP_CODE_RESET | HTTP_ABORT, "Route target \"close\" is closing request");
    return HTTP_ROUTE_OK;
}


static int redirectTarget(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    cchar       *target;

    assert(conn);
    assert(route);
    assert(route->target);

    target = expandTokens(conn, route->target);
    httpRedirect(conn, route->responseStatus ? route->responseStatus : HTTP_CODE_MOVED_TEMPORARILY, target);
    return HTTP_ROUTE_OK;
}


static int runTarget(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    conn->rx->target = route->target ? expandTokens(conn, route->target) : sclone(&conn->rx->pathInfo[1]);
    return HTTP_ROUTE_OK;
}


static int writeTarget(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    char    *str;

    assert(conn);
    assert(route);

    /*
        Need to re-compute output string as updates may have run to define params which affect the route->target tokens
     */
    str = route->target ? expandTokens(conn, route->target) : sclone(&conn->rx->pathInfo[1]);
    if (!(route->flags & HTTP_ROUTE_RAW)) {
        str = mprEscapeHtml(str);
    }
    httpSetStatus(conn, route->responseStatus);
    httpFormatResponse(conn, "%s", str);
    httpFinalize(conn);
    return HTTP_ROUTE_OK;
}


/************************************************** Route Convenience ****************************************************/

PUBLIC HttpRoute *httpDefineRoute(HttpRoute *parent, cchar *methods, cchar *pattern, cchar *target, cchar *source)
{
    HttpRoute   *route;

    if ((route = httpCreateInheritedRoute(parent)) == 0) {
        return 0;
    }
#if DEPRECATE
    if (schr(target, '-')) {
        char   *controller, *action;
        controller = ssplit(sclone(target), "-", &action);
        target = sjoin(controller, "/", action, NULL);
    }
#endif
    httpSetRoutePattern(route, pattern, 0);
    if (methods) {
        httpSetRouteMethods(route, methods);
    }
    if (source) {
        httpSetRouteSource(route, source);
    }
    httpSetRouteTarget(route, "run", target);
    httpFinalizeRoute(route);
    return route;
}


PUBLIC HttpRoute *httpAddRestfulRoute(HttpRoute *parent, cchar *methods, cchar *pattern, cchar *target, cchar *resource)
{
    cchar   *source;

#if DEPRECATE
    if (*resource == '{') {
        pattern = sfmt("^%s%s/%s%s", parent->prefix, parent->serverPrefix, resource, pattern);
    } else {
        pattern = sfmt("^%s%s/{controller=%s}%s", parent->prefix, parent->serverPrefix, resource, pattern);
    }
#else
    if (*resource == '{') {
        pattern = sfmt("^%s/%s%s", parent->prefix, resource, pattern);
    } else {
        pattern = sfmt("^%s/{controller=%s}%s", parent->prefix, resource, pattern);
    }
#endif
    if (target && *target) {
        target = sjoin("/", target, NULL);
    }
    if (*resource == '{') {
        target = sfmt("$%s%s", resource, target);
        source = sfmt("$%s.c", resource);
    } else {
        target = sfmt("%s%s", resource, target);
        source = sfmt("%s.c", resource);
    }
    return httpDefineRoute(parent, methods, pattern, target, source);
}


PUBLIC void httpAddResourceGroup(HttpRoute *parent, cchar *resource)
{
    /* Delete is a POST method alternative to remove */
    httpAddRestfulRoute(parent, "GET",     "$",                           "",          resource);
    httpAddRestfulRoute(parent, "POST",    "/{id=[0-9]+}/delete$",        "delete",    resource);
    httpAddRestfulRoute(parent, "POST",    "(/)*$",                       "create",    resource);
    httpAddRestfulRoute(parent, "GET",     "/{id=[0-9]+}/edit$",          "edit",      resource);
    httpAddRestfulRoute(parent, "GET",     "/{id=[0-9]+}$",               "get",       resource);
    httpAddRestfulRoute(parent, "GET",     "/init$",                      "init",      resource);
    httpAddRestfulRoute(parent, "GET",     "/list$",                      "list",      resource);
    httpAddWebSocketsRoute(parent, "stream");
    httpAddRestfulRoute(parent, "DELETE",  "/{id=[0-9]+}$",               "remove",    resource);
    httpAddRestfulRoute(parent, "POST",    "/{id=[0-9]+}$",               "update",    resource);
    httpAddRestfulRoute(parent, "GET,POST","/{id=[0-9]+}/{action}(/)*$",  "${action}", resource);
    httpAddRestfulRoute(parent, "GET,POST","/{action}(/)*$",              "${action}", resource);
}


/*
    Singleton resource
 */
PUBLIC void httpAddResource(HttpRoute *parent, cchar *resource)
{
    /* Delete is a POST method alternative to remove */
    httpAddRestfulRoute(parent, "GET",     "$",              "",           resource);
    httpAddRestfulRoute(parent, "POST",    "/delete$",       "delete",     resource);
    httpAddRestfulRoute(parent, "POST",    "(/)*$",          "create",     resource);
    httpAddRestfulRoute(parent, "GET",     "/edit$",         "edit",       resource);
    httpAddRestfulRoute(parent, "GET",     "(/)*$",          "get",        resource);
    httpAddRestfulRoute(parent, "GET",     "/init$",         "init",       resource);
    httpAddRestfulRoute(parent, "POST",    "(/)*$",          "update",     resource);
    httpAddRestfulRoute(parent, "DELETE",  "(/)*$",          "remove",     resource);
    httpAddWebSocketsRoute(parent, "stream");
    httpAddRestfulRoute(parent, "GET,POST","/{action}(/)*$", "${action}",  resource);
}


/*
    Add routes for a permanent resource. Cannot create or remove.
 */
PUBLIC void httpAddPermResource(HttpRoute *parent, cchar *resource)
{
    httpAddRestfulRoute(parent, "GET",     "$",              "",           resource);
    httpAddRestfulRoute(parent, "GET",     "(/)*$",          "get",        resource);
    httpAddRestfulRoute(parent, "POST",    "(/)*$",          "update",     resource);
    httpAddWebSocketsRoute(parent, "stream");
    httpAddRestfulRoute(parent, "GET,POST","/{action}(/)*$", "${action}",  resource);
}


PUBLIC HttpRoute *httpAddWebSocketsRoute(HttpRoute *parent, cchar *action)
{
    HttpRoute   *route;
    HttpLimits  *limits;
    cchar       *path, *pattern;

#if DEPRECATE
    pattern = sfmt("^%s%s/{controller}/%s", parent->prefix, parent->serverPrefix, action);
#else
    pattern = sfmt("^%s/{controller}/%s", parent->prefix, action);
#endif
    path = sjoin("$1/", action, NULL);
    route = httpDefineRoute(parent, "GET", pattern, path, "${controller}.c");
    httpAddRouteFilter(route, "webSocketFilter", "", HTTP_STAGE_RX | HTTP_STAGE_TX);

    /*
        Set some reasonable defaults. 5 minutes for inactivity and no request timeout limit.
     */
    limits = httpGraduateLimits(route, 0);
    limits->inactivityTimeout = ME_MAX_INACTIVITY_DURATION * 10;
    limits->requestTimeout = HTTP_UNLIMITED;
    limits->rxBodySize = HTTP_UNLIMITED;
    limits->txBodySize = HTTP_UNLIMITED;
    return route;
}

/*************************************************** Support Routines ****************************************************/
/*
    Route operations are used per-route for headers and fields
 */
static HttpRouteOp *createRouteOp(cchar *name, int flags)
{
    HttpRouteOp   *op;

    assert(name && *name);

    if ((op = mprAllocObj(HttpRouteOp, manageRouteOp)) == 0) {
        return 0;
    }
    op->name = sclone(name);
    op->flags = flags;
    return op;
}


static void manageRouteOp(HttpRouteOp *op, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(op->name);
        mprMark(op->details);
        mprMark(op->var);
        mprMark(op->value);

    } else if (flags & MPR_MANAGE_FREE) {
        if (op->flags & HTTP_ROUTE_FREE) {
            free(op->mdata);
        }
    }
}


static bool opPresent(MprList *list, HttpRouteOp *op)
{
    HttpRouteOp   *last;

    if ((last = mprGetLastItem(list)) == 0) {
        return 0;
    }
    if (smatch(last->name, op->name) &&
        smatch(last->details, op->details) &&
        smatch(last->var, op->var) &&
        smatch(last->value, op->value) &&
        last->mdata == op->mdata &&
        last->flags == op->flags) {
        return 1;
    }
    return 0;
}


static void addUniqueItem(MprList *list, HttpRouteOp *op)
{
    int     index;

    assert(list);
    assert(op);

    if (!opPresent(list, op)) {
        index = smatch(op->name, "secure") ? 0 : list->length;
        mprInsertItemAtPos(list, index, op);
    }
}


static HttpLang *createLangDef(cchar *path, cchar *suffix, int flags)
{
    HttpLang    *lang;

    if ((lang = mprAllocObj(HttpLang, manageLang)) == 0) {
        return 0;
    }
    if (path) {
        lang->path = sclone(path);
    }
    if (suffix) {
        lang->suffix = sclone(suffix);
    }
    lang->flags = flags;
    return lang;
}


static void manageLang(HttpLang *lang, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(lang->path);
        mprMark(lang->suffix);
    }
}


static void definePathVars(HttpRoute *route)
{
    assert(route);

    mprAddKey(route->vars, "PRODUCT", sclone(ME_NAME));
    mprAddKey(route->vars, "OS", sclone(ME_OS));
    mprAddKey(route->vars, "VERSION", sclone(ME_VERSION));
    mprAddKey(route->vars, "PLATFORM", sclone(ME_PLATFORM));
    mprAddKey(route->vars, "BIN_DIR", mprGetAppDir());
    if (route->host) {
        defineHostVars(route);
    }
}


static void defineHostVars(HttpRoute *route)
{
    assert(route);
    mprAddKey(route->vars, "DOCUMENTS", route->documents);
    mprAddKey(route->vars, "HOME", route->home);
    mprAddKey(route->vars, "HOST", route->host->name);
#if DEPRECATE
    mprAddKey(route->vars, "SERVER_NAME", route->host->name);
#endif
}


static char *expandTokens(HttpConn *conn, cchar *str)
{
    HttpRx      *rx;

    assert(conn);
    assert(str);

    rx = conn->rx;
    return expandRequestTokens(conn, expandPatternTokens(rx->pathInfo, str, rx->matches, rx->matchCount));
}


/*
    WARNING: str is modified. Result is allocated string.
 */
static char *expandRequestTokens(HttpConn *conn, char *str)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    HttpUri     *uri;
    MprBuf      *buf;
    HttpLang    *lang;
    char        *tok, *cp, *key, *value, *field, *header, *defaultValue, *state, *v, *p;

    assert(conn);
    assert(str);

    rx = conn->rx;
    route = rx->route;
    tx = conn->tx;
    buf = mprCreateBuf(-1, -1);
    tok = 0;
    uri = rx->parsedUri;

    for (cp = str; cp && *cp; ) {
        if ((tok = strstr(cp, "${")) == 0) {
            break;
        }
        if (tok > cp) {
            mprPutBlockToBuf(buf, cp, tok - cp);
        }
        if ((key = stok(&tok[2], ".:}", &value)) == 0) {
            continue;
        }
        if ((stok(value, "}", &cp)) == 0) {
            continue;
        }
        if (smatch(key, "header")) {
            header = stok(value, "=", &defaultValue);
            if ((value = (char*) httpGetHeader(conn, header)) == 0) {
                value = defaultValue ? defaultValue : "";
            }
            mprPutStringToBuf(buf, value);

        } else if (smatch(key, "param")) {
            field = stok(value, "=", &defaultValue);
            if (defaultValue == 0) {
                defaultValue = "";
            }
            mprPutStringToBuf(buf, httpGetParam(conn, field, defaultValue));

        } else if (smatch(key, "request")) {
            value = stok(value, "=", &defaultValue);
            //  OPT with switch on first char

            if (smatch(value, "authenticated")) {
                mprPutStringToBuf(buf, rx->authenticated ? "true" : "false");

            } else if (smatch(value, "clientAddress")) {
                mprPutStringToBuf(buf, conn->ip);

            } else if (smatch(value, "clientPort")) {
                mprPutIntToBuf(buf, conn->port);

            } else if (smatch(value, "error")) {
                mprPutStringToBuf(buf, conn->errorMsg);

            } else if (smatch(value, "ext")) {
                mprPutStringToBuf(buf, uri->ext);

            } else if (smatch(value, "extraPath")) {
                mprPutStringToBuf(buf, rx->extraPath);

            } else if (smatch(value, "filename")) {
                mprPutStringToBuf(buf, tx->filename);

            } else if (scaselessmatch(value, "language")) {
                if (!defaultValue) {
                    defaultValue = route->defaultLanguage;
                }
                if ((lang = httpGetLanguage(conn, route->languages, defaultValue)) != 0) {
                    mprPutStringToBuf(buf, lang->suffix);
                } else {
                    mprPutStringToBuf(buf, defaultValue);
                }

            } else if (scaselessmatch(value, "languageDir")) {
                lang = httpGetLanguage(conn, route->languages, 0);
                if (!defaultValue) {
                    defaultValue = ".";
                }
                mprPutStringToBuf(buf, lang ? lang->path : defaultValue);

            } else if (smatch(value, "host")) {
                mprPutStringToBuf(buf, httpFormatUri(0, uri->host, uri->port, 0, 0, 0, 0));

            } else if (smatch(value, "method")) {
                mprPutStringToBuf(buf, rx->method);

            } else if (smatch(value, "originalUri")) {
                mprPutStringToBuf(buf, rx->originalUri);

            } else if (smatch(value, "pathInfo")) {
                mprPutStringToBuf(buf, rx->pathInfo);

            } else if (smatch(value, "prefix")) {
                mprPutStringToBuf(buf, route->prefix);

            } else if (smatch(value, "query")) {
                mprPutStringToBuf(buf, uri->query);

            } else if (smatch(value, "reference")) {
                mprPutStringToBuf(buf, uri->reference);

            } else if (smatch(value, "scheme")) {
                if (uri->scheme) {
                    mprPutStringToBuf(buf, uri->scheme);
                }  else {
                    mprPutStringToBuf(buf, (conn->secure) ? "https" : "http");
                }

            } else if (smatch(value, "scriptName")) {
                mprPutStringToBuf(buf, rx->scriptName);

            } else if (smatch(value, "serverAddress")) {
                /* Pure IP address, no port. See "serverPort" */
                mprPutStringToBuf(buf, conn->sock->acceptIp);

            } else if (smatch(value, "serverPort")) {
                mprPutIntToBuf(buf, conn->sock->acceptPort);

            } else if (smatch(value, "uri")) {
                mprPutStringToBuf(buf, rx->uri);
            }
        } else if (smatch(key, "ssl")) {
            value = stok(value, "=", &defaultValue);
            if (smatch(value, "state")) {
                mprPutStringToBuf(buf, mprGetSocketState(conn->sock));
            } else {
                state = mprGetSocketState(conn->sock);
                if ((p = scontains(state, value)) != 0) {
                    stok(p, "=", &v);
                    mprPutStringToBuf(buf, stok(v, ", ", NULL));
                }
            }
        }
    }
    assert(cp);
    if (tok) {
        if (tok > cp) {
            mprPutBlockToBuf(buf, tok, tok - cp);
        }
    } else {
        mprPutStringToBuf(buf, cp);
    }
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


PUBLIC char *httpExpandVars(HttpConn *conn, cchar *str)
{
    return expandRequestTokens(conn, stemplate(str, conn->rx->route->vars));
}


/*
    Replace text using pcre regular expression match indexes
 */
static char *expandPatternTokens(cchar *str, cchar *replacement, int *matches, int matchCount)
{
    MprBuf  *result;
    cchar   *end, *cp, *lastReplace;
    int     submatch;

    assert(str);
    assert(replacement);
    assert(matches);

    result = mprCreateBuf(-1, -1);
    lastReplace = replacement;
    end = &replacement[slen(replacement)];

    for (cp = replacement; cp < end; ) {
        if (*cp == '$') {
            if (lastReplace < cp) {
                mprPutSubStringToBuf(result, lastReplace, (int) (cp - lastReplace));
            }
            switch (*++cp) {
            case '$':
                mprPutCharToBuf(result, '$');
                break;
            case '&':
                /* Replace the matched string */
                if (matchCount > 0) {
                    mprPutSubStringToBuf(result, &str[matches[0]], matches[1] - matches[0]);
                }
                break;
            case '`':
                /* Insert the portion that preceeds the matched string */
                if (matchCount > 0) {
                    mprPutSubStringToBuf(result, str, matches[0]);
                }
                break;
            case '\'':
                /* Insert the portion that follows the matched string */
                if (matchCount > 0) {
                    mprPutSubStringToBuf(result, &str[matches[1]], slen(str) - matches[1]);
                }
                break;
            default:
                /* Insert the nth submatch */
                if (isdigit((uchar) *cp)) {
                    submatch = (int) atoi(cp);
                    while (isdigit((uchar) *++cp))
                        ;
                    cp--;
                    if (submatch < matchCount) {
                        submatch *= 2;
                        mprPutSubStringToBuf(result, &str[matches[submatch]], matches[submatch + 1] - matches[submatch]);
                    }
                } else {
                    mprDebug("http route", 5, "Bad replacement $ specification in page");
                    return 0;
                }
            }
            lastReplace = cp + 1;
        }
        cp++;
    }
    if (lastReplace < cp && lastReplace < end) {
        mprPutSubStringToBuf(result, lastReplace, (int) (cp - lastReplace));
    }
    mprAddNullToBuf(result);
    return sclone(mprGetBufStart(result));
}


PUBLIC void httpDefineRouteBuiltins()
{
    /*
        These are the conditions that can be selected. Use httpAddRouteCondition to add to a route.
        The allow and auth conditions are internal and are configured via various Auth APIs.
     */
    httpDefineRouteCondition("allowDeny", allowDenyCondition);
    httpDefineRouteCondition("auth", authCondition);
    httpDefineRouteCondition("directory", directoryCondition);
    httpDefineRouteCondition("exists", existsCondition);
    httpDefineRouteCondition("match", matchCondition);
    httpDefineRouteCondition("secure", secureCondition);
    httpDefineRouteCondition("unauthorized", unauthorizedCondition);

    httpDefineRouteUpdate("param", paramUpdate);
    httpDefineRouteUpdate("cmd", cmdUpdate);
    httpDefineRouteUpdate("lang", langUpdate);

    httpDefineRouteTarget("close", closeTarget);
    httpDefineRouteTarget("redirect", redirectTarget);
    httpDefineRouteTarget("run", runTarget);
    httpDefineRouteTarget("write", writeTarget);
}


/*
    Tokenizes a line using %formats. Mandatory tokens can be specified with %. Optional tokens are specified with ?.
    Supported tokens:
        %B - Boolean. Parses: on/off, true/false, yes/no.
        %N - Number. Parses numbers in base 10.
        %S - String. Removes quotes.
        %T - Template String. Removes quotes and expand ${PathVars}.
        %P - Path string. Removes quotes and expands ${PathVars}. Resolved relative to host->dir (Home).
        %W - Parse words into a list
        %! - Optional negate. Set value to HTTP_ROUTE_NOT present, otherwise zero.
    Values wrapped in quotes will have the outermost quotes trimmed.
 */
PUBLIC bool httpTokenize(HttpRoute *route, cchar *line, cchar *fmt, ...)
{
    va_list     args;
    bool        rc;

    assert(route);
    assert(line);
    assert(fmt);

    va_start(args, fmt);
    rc =  httpTokenizev(route, line, fmt, args);
    va_end(args);
    return rc;
}


PUBLIC bool httpTokenizev(HttpRoute *route, cchar *line, cchar *fmt, va_list args)
{
    MprList     *list;
    cchar       *f;
    char        *tok, *etok, *value, *word, *end;
    int         quote;

    assert(route);
    assert(fmt);

    if (line == 0) {
        line = "";
    }
    tok = sclone(line);
    end = &tok[slen(line)];

    for (f = fmt; *f && tok < end; f++) {
        for (; isspace((uchar) *tok); tok++) ;
        if (*tok == '\0' || *tok == '#') {
            break;
        }
        if (isspace((uchar) *f)) {
            continue;
        }
        if (*f == '%' || *f == '?') {
            f++;
            quote = 0;
            if (*f != '*' && (*tok == '"' || *tok == '\'')) {
                quote = *tok++;
            }
            if (*f == '!') {
                etok = &tok[1];
            } else {
                if (quote) {
                    for (etok = tok; *etok && !(*etok == quote && etok[-1] != '\\'); etok++) ;
                    *etok++ = '\0';
                } else if (*f == '*') {
                    for (etok = tok; *etok; etok++) {
                        if (*etok == '#') {
                            *etok = '\0';
                        }
                    }
                } else {
                    for (etok = tok; *etok && !isspace((uchar) *etok); etok++) ;
                }
                *etok++ = '\0';
            }
            if (*f == '*') {
                f++;
                tok = trimQuotes(tok);
                * va_arg(args, char**) = tok;
                tok = etok;
                break;
            }

            switch (*f) {
            case '!':
                if (*tok == '!') {
                    *va_arg(args, int*) = HTTP_ROUTE_NOT;
                } else {
                    *va_arg(args, int*) = 0;
                    continue;
                }
                break;
            case 'B':
                *va_arg(args, bool*) = httpGetBoolToken(tok);;
                break;
            case 'N':
                *va_arg(args, int*) = (int) stoi(tok);
                break;
            case 'P':
                *va_arg(args, char**) = httpMakePath(route, route->home, strim(tok, "\"", MPR_TRIM_BOTH));
                break;
            case 'S':
                *va_arg(args, char**) = strim(tok, "\"", MPR_TRIM_BOTH);
                break;
            case 'T':
                value = strim(tok, "\"", MPR_TRIM_BOTH);
                *va_arg(args, char**) = stemplate(value, route->vars);
                break;
            case 'W':
                list = va_arg(args, MprList*);
                word = stok(tok, " \t\r\n", &tok);
                while (word) {
                    mprAddItem(list, word);
                    word = stok(0, " \t\r\n", &tok);
                }
                break;
            default:
                mprDebug("http route", 5, "Unknown token pattern %%\"%c\"", *f);
                break;
            }
            tok = etok;
        }
    }
    if (tok < end) {
        /*
            Extra unparsed text
         */
        for (; tok < end && isspace((uchar) *tok); tok++) ;
        if (*tok && *tok != '#') {
            mprDebug("http route", 5, "Extra unparsed text: \"%s\"", tok);
            return 0;
        }
    }
    if (*f) {
        /*
            Extra unparsed format tokens
         */
        for (; *f; f++) {
            if (*f == '%') {
                break;
            } else if (*f == '?') {
                switch (*++f) {
                case '!':
                case 'N':
                    *va_arg(args, int*) = 0;
                    break;
                case 'B':
                    *va_arg(args, bool*) = 0;
                    break;
                case 'D':
                case 'P':
                case 'S':
                case 'T':
                case '*':
                    *va_arg(args, char**) = 0;
                    break;
                case 'W':
                    break;
                default:
                    mprDebug("http route", 5, "Unknown token pattern %%\"%c\"", *f);
                    break;
                }
            }
        }
        if (*f) {
            mprDebug("http route", 5, "Missing directive parameters");
            return 0;
        }
    }
    va_end(args);
    return 1;
}


PUBLIC bool httpGetBoolToken(cchar *tok)
{
    return scaselessmatch(tok, "on") || scaselessmatch(tok, "true") || scaselessmatch(tok, "yes") || smatch(tok, "1");
}


static char *trimQuotes(char *str)
{
    ssize   len;

    assert(str);
    len = slen(str);
    if (*str == '\"' && str[len - 1] == '\"' && len > 2 && str[1] != '\"') {
        return snclone(&str[1], len - 2);
    }
    return sclone(str);
}


PUBLIC cchar *httpGetDir(HttpRoute *route, cchar *name)
{
    cchar   *key;

    key = sjoin(supper(name), "_DIR", NULL);
    return httpGetRouteVar(route, key);
}


PUBLIC void httpSetDir(HttpRoute *route, cchar *name, cchar *value)
{
    cchar   *path, *rpath;

    if (value == 0) {
        value = slower(name);
    }
    path = httpMakePath(route, 0, value);
    path = mprJoinPath(route->home, path);
    name = supper(name);

    /*
        Define the variable as a relative path to the route home
     */
    rpath = mprGetRelPath(path, route->home);
    httpSetRouteVar(route, sjoin(name, "_DIR", NULL), rpath);

    /*
        Home and documents are stored as absolute paths
     */
    if (smatch(name, "HOME")) {
        httpSetRouteVar(route, name, rpath);
        route->home = path;
    } else if (smatch(name, "DOCUMENTS")) {
        httpSetRouteVar(route, name, rpath);
        route->documents = path;
    }
}


PUBLIC MprHash *httpGetOptions(cchar *options)
{
    if (options == 0) {
        return mprCreateHash(-1, MPR_HASH_STABLE);
    }
    if (*options == '@') {
        /* Allow embedded URIs as options */
        options = sfmt("{ data-click: '%s'}", options);
    }
    assert(*options == '{');
    if (*options != '{') {
        options = sfmt("{%s}", options);
    }
    return mprDeserialize(options);
}


PUBLIC void *httpGetOption(MprHash *options, cchar *field, cchar *defaultValue)
{
    MprKey      *kp;
    cchar       *value;

    if (options == 0) {
        value = defaultValue;
    } else if ((kp = mprLookupKeyEntry(options, field)) == 0) {
        value = defaultValue;
    } else {
        value = kp->data;
    }
    return (void*) value;
}


PUBLIC MprHash *httpGetOptionHash(MprHash *options, cchar *field)
{
    MprKey      *kp;

    if (options == 0) {
        return 0;
    }
    if ((kp = mprLookupKeyEntry(options, field)) == 0) {
        return 0;
    }
    return (MprHash*) kp->data;
}


/*
    Prepend an option
 */
PUBLIC void httpInsertOption(MprHash *options, cchar *field, cchar *value)
{
    MprKey      *kp;

    if (options == 0) {
        assert(options);
        return;
    }
    if ((kp = mprLookupKeyEntry(options, field)) != 0) {
        kp = mprAddKey(options, field, sjoin(value, " ", kp->data, NULL));
    } else {
        kp = mprAddKey(options, field, value);
    }
}


PUBLIC void httpAddOption(MprHash *options, cchar *field, cchar *value)
{
    MprKey      *kp;

    if (options == 0) {
        assert(options);
        return;
    }
    if ((kp = mprLookupKeyEntry(options, field)) != 0) {
        kp = mprAddKey(options, field, sjoin(kp->data, " ", value, NULL));
    } else {
        kp = mprAddKey(options, field, value);
    }
}


PUBLIC void httpRemoveOption(MprHash *options, cchar *field)
{
    if (options == 0) {
        assert(options);
        return;
    }
    mprRemoveKey(options, field);
}


PUBLIC bool httpOption(MprHash *hash, cchar *field, cchar *value, int useDefault)
{
    return smatch(value, httpGetOption(hash, field, useDefault ? value : 0));
}


PUBLIC void httpSetOption(MprHash *options, cchar *field, cchar *value)
{
    if (value == 0) {
        return;
    }
    if (options == 0) {
        assert(options);
        return;
    }
    mprAddKey(options, field, value);
}


PUBLIC void httpHideRoute(HttpRoute *route, bool on)
{
    route->flags &= ~HTTP_ROUTE_HIDDEN;
    if (on) {
        route->flags |= HTTP_ROUTE_HIDDEN;
    }
}


PUBLIC HttpLimits *httpGraduateLimits(HttpRoute *route, HttpLimits *limits)
{
    if (route->parent && route->limits == route->parent->limits) {
        if (limits == 0) {
            if (route->parent->limits) {
                limits = route->parent->limits;
            } else {
                limits = HTTP->serverLimits;
            }
        }
        route->limits = mprMemdup(limits, sizeof(HttpLimits));
    }
    return route->limits;
}


#undef  GRADUATE_HASH
#undef  GRADUATE_LIST

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
