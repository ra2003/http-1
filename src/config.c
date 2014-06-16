/*
    config.c -- Http JSON Configuration File Parsing

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/*********************************** Includes *********************************/

#include    "http.h"

/************************************ Defines *********************************/

#define ITERATE_CONFIG(route, obj, child, index) \
    index = 0, child = obj ? obj->children: 0; obj && index < obj->length && !route->error; child = child->next, index++

/************************************ Forwards ********************************/
static void parseAll(HttpRoute *route, cchar *key, MprJson *prop);
static void parseAuthStore(HttpRoute *route, cchar *key, MprJson *prop);
static void postParse(HttpRoute *route);
static void parseRoutes(HttpRoute *route, cchar *key, MprJson *prop);

/************************************** Code **********************************/

PUBLIC HttpParseCallback httpAddConfig(cchar *key, HttpParseCallback callback)
{
    Http                *http;
    HttpParseCallback   prior;

    http = MPR->httpService;
    prior = mprLookupKey(http->parsers, key);
    mprAddKey(http->parsers, key, callback);
    return prior;
}


static void httpParseError(HttpRoute *route, cchar *fmt, ...)
{
    va_list     args;
    char        *msg;

    va_start(args, fmt);
    msg = sfmtv(fmt, args);
    mprLog("error http config", 0, "%s", msg);
    va_end(args);
    route->error = 1;
}


static cchar *getList(MprJson *prop)
{
    char    *jstr, *cp;

    if (prop == 0) {
        return 0;
    }
    if ((jstr = mprJsonToString(prop, 0)) == 0) {
        return 0;
    }
    if (*jstr == '[') {
        jstr = strim(jstr, "[]", 0);
    }
    for (cp = jstr; *cp; cp++) {
        if (*cp == '"') {
            *cp = ' ';
        }
    }
    return jstr;
}


static int getint(cchar *value)
{
    int64   num;

    num = httpGetNumber(value);
    if (num >= MAXINT) {
        num = MAXINT;
    }
    return (int) num;
}


static int testConfig(HttpRoute *route, cchar *path)
{
    MprPath     cinfo;

    if (mprGetPathInfo(path, &cinfo) == 0) {
        if (route->config && cinfo.mtime > route->configLoaded) {
            route->config = 0;
        }
        route->configLoaded = cinfo.mtime;
    }
    if (route->config) {
        return 0;
    }
    if (!mprPathExists(path, R_OK)) {
        mprLog("error http config", 0, "Cannot find %s", path);
        return MPR_ERR_CANT_READ;
    }
    return 0;
}


/*
    Blend the app.modes[app.mode] into app
 */
static void blendMode(HttpRoute *route, MprJson *config)
{
    MprJson     *currentMode, *app;
    cchar       *mode;

    mode = mprGetJson(config, "app.mode");
    if (!mode) {
        mode = sclone("debug");
        mprLog("info http config", 3, "Route \"%s\" running in \"%s\" mode", route->name, mode);
    }
    if ((currentMode = mprGetJsonObj(config, sfmt("app.modes.%s", mode))) != 0) {
        app = mprLookupJsonObj(config, "app");
        mprBlendJson(app, currentMode, MPR_JSON_OVERWRITE);
        mprSetJson(app, "app.mode", mode);
    }
}


PUBLIC int parseFile(HttpRoute *route, cchar *path)
{
    MprJson     *config;
    cchar       *data, *errorMsg;

    if ((data = mprReadPathContents(path, NULL)) == 0) {
        mprLog("error http config", 0, "Cannot read configuration from \"%s\"", path);
        return MPR_ERR_CANT_READ;
    }
    if ((config = mprParseJsonEx(data, 0, 0, 0, &errorMsg)) == 0) {
        mprLog("error http config", 0, "Cannot parse %s: error %s", path, errorMsg);
        return MPR_ERR_CANT_READ;
    }
    if (route->config == 0) {
        blendMode(route, config);
        route->config = config;
    }
    parseAll(route, 0, config);
    return 0;
}


PUBLIC int httpLoadConfig(HttpRoute *route, cchar *name)
{
    cchar       *path;

    lock(route);
    route->error = 0;

    path = mprJoinPath(route->home, name);
    if (testConfig(route, path) < 0) {
        unlock(route);
        return MPR_ERR_CANT_READ;
    }
    if (route->config) {
        unlock(route);
        return 0;
    }
    if (parseFile(route, path) < 0) {
        unlock(route);
        return MPR_ERR_CANT_READ;
    }
    postParse(route);

    if (route->error) {
        route->config = 0;
        unlock(route);
        return MPR_ERR_BAD_STATE;
    }
    unlock(route);
    return 0;
}


static void clientCopy(HttpRoute *route, MprJson *dest, MprJson *obj)
{
    MprJson     *child, *job, *jvalue;
    cchar       *key, *value;
    int         ji;

    for (ITERATE_CONFIG(route, obj, child, ji)) {
        if (child->type & MPR_JSON_OBJ) {
            job = mprCreateJson(MPR_JSON_OBJ);
            clientCopy(route, job, child);
            mprSetJsonObj(dest, child->name, job);
        } else {
            key = child->value;
            if (sends(key, "|time")) {
                key = stok(sclone(key), " \t|", NULL);
                if ((value = mprGetJson(route->config, key)) != 0) {
                    mprSetJson(dest, child->name, itos(httpGetTicks(value)));
                }
            } else {
                if ((jvalue = mprGetJsonObj(route->config, key)) != 0) {
                    mprSetJsonObj(dest, child->name, mprCloneJson(jvalue));
                }
            }
        }
    }
}


static void postParse(HttpRoute *route)
{
    Http        *http;
    HttpHost    *host;
    HttpRoute   *rp;
    MprJson     *mappings, *client;
    int         nextHost, nextRoute;

    if (route->error) {
        return;
    }
    http = route->http;
    route->mode = mprGetJson(route->config, "app.mode");

    /*
        Create a subset, optimized configuration to send to the client
     */
    if ((mappings = mprGetJsonObj(route->config, "app.client.mappings")) != 0) {
        client = mprCreateJson(MPR_JSON_OBJ);
        clientCopy(route, client, mappings);
        mprSetJson(client, "prefix", route->prefix);
        route->client = mprJsonToString(client, MPR_JSON_QUOTES);
    }
    httpAddHostToEndpoints(route->host);

    /*
        Ensure the host home directory is set and the file handler is defined
        Propagate the HttpRoute.client to all child routes.
     */
    for (nextHost = 0; (host = mprGetNextItem(http->hosts, &nextHost)) != 0; ) {
        for (nextRoute = 0; (rp = mprGetNextItem(host->routes, &nextRoute)) != 0; ) {
            if (!mprLookupKey(rp->extensions, "")) {
                httpAddRouteHandler(rp, "fileHandler", "");
                httpAddRouteIndex(rp, "index.html");
            }
            if (rp->parent == route) {
                rp->client = route->client;
            }
        }
    }
}


#if MOVED
PUBLIC cchar *httpGetDir(HttpRoute *route, cchar *name)
{
    cchar   *key;

    key = sjoin(supper(name), "_DIR", NULL);
    return httpGetRouteVar(route, key);
}


PUBLIC void httpSetDir(HttpRoute *route, cchar *name, cchar *value)
{
    if (value == 0) {
        value = name;
    }
    value = mprJoinPath(route->documents, value);
    httpSetRouteVar(route, sjoin(supper(name), "_DIR", NULL), httpMakePath(route, 0, value));
}


PUBLIC void httpSetDefaultDirs(HttpRoute *route)
{
    httpSetDir(route, "cache", 0);
    httpSetDir(route, "client", 0);
    httpSetDir(route, "paks", "paks");
}
#endif

/**************************************** Parser Callbacks ****************************************/


static void parseKey(HttpRoute *route, cchar *key, MprJson *prop)
{
    Http                *http;
    HttpParseCallback   parser;

    http = MPR->httpService;
    key = key ? sjoin(key, ".", prop->name, NULL) : prop->name;
    if ((parser = mprLookupKey(http->parsers, key)) != 0) {
        parser(route, key, prop);
    }
}


static void parseAll(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        parseKey(route, key, child);
    }
}


static void parseDirectories(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (smatch(child->name, "documents")) {
            httpSetRouteDocuments(route, child->value);
        } else if (smatch(child->name, "home")) {
            httpSetRouteHome(route, child->value);
        }
        httpSetDir(route, child->name, child->value);
    }
}


static void parseAuth(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (prop->type & MPR_JSON_STRING) {
        parseAuthStore(route, key, prop);
    } else if (prop->type == MPR_JSON_OBJ) {
        parseAll(route, key, prop);
    }
}


static void parseAuthLogin(HttpRoute *route, cchar *key, MprJson *prop)
{
    /* Automatic login as this user. Password not required */
    httpSetAuthUsername(route->auth, mprGetJson(prop, "name"));
}


static void parseAuthRealm(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetAuthRealm(route->auth, prop->value);
}


static void parseAuthRequireRoles(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpSetAuthRequiredAbilities(route->auth, prop->value);
    }
}


static void parseAuthRequireUsers(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    if (prop->type & MPR_JSON_STRING) {
        if (smatch(prop->value, "*")) {
            httpSetAuthAnyValidUser(route->auth);
        } else {
            httpSetAuthPermittedUsers(route->auth, prop->value);
        }
    } else if (prop->type & MPR_JSON_OBJ) {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (smatch(prop->value, "*")) {
                httpSetAuthAnyValidUser(route->auth);
                break;
            } else {
                httpSetAuthPermittedUsers(route->auth, getList(child));
            }
        }
    }
}


static void parseAuthRoles(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (httpAddRole(route->auth, child->name, getList(child)) < 0) {
            httpParseError(route, "Cannot add role %s", child->name);
            break;
        }
    }
}


static void parseAuthSessionCookie(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteCookie(route, prop->value);
}


static void parseAuthSessionVisibility(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteSessionVisibility(route, scaselessmatch(prop->value, "visible"));
}


static void parseAuthStore(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (httpSetAuthStore(route->auth, prop->value) < 0) {
        httpParseError(route, "The %s AuthStore is not available on this platform", prop->value);
    }
}


static void parseAuthType(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (httpSetAuthType(route->auth, prop->value, 0) < 0) {
        httpParseError(route, "The %s AuthType is not available on this platform", prop->value);
    }
    if (smatch(prop->value, "basic") || smatch(prop->value, "digest")) {
        /*
            These are implemented by the browser, so we can use a global auth-condition
         */
        httpAddRouteCondition(route, "auth", 0, 0);
    }
}


static void parseAuthUsers(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *roles, *password;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        password = mprGetJson(child, "password");
        roles = getList(mprGetJsonObj(child, "roles"));
        if (httpAddUser(route->auth, child->name, password, roles) < 0) {
            httpParseError(route, "Cannot add user %s", child->name);
            break;
        }
    }
}


static void parseCache(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    MprTicks    clientLifespan, serverLifespan;
    cchar       *methods, *extensions, *uris, *mimeTypes, *client, *server;
    int         flags, ji;

    clientLifespan = serverLifespan = 0;
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        flags = 0;
        if ((client = mprGetJson(child, "client")) != 0) {
            flags |= HTTP_CACHE_CLIENT;
            clientLifespan = httpGetTicks(client);
        }
        if ((server = mprGetJson(child, "server")) != 0) {
            flags |= HTTP_CACHE_SERVER;
            serverLifespan = httpGetTicks(server);
        }
        methods = getList(mprGetJsonObj(child, "methods"));
        extensions = getList(mprGetJsonObj(child, "extensions"));
        uris = getList(mprGetJsonObj(child, "uris"));
        mimeTypes = getList(mprGetJsonObj(child, "mime"));

        if (smatch(mprGetJson(child, "unique"), "true")) {
            /* Uniquely cache requests with different params */
            flags |= HTTP_CACHE_UNIQUE;
        }
        if (smatch(mprGetJson(child, "manual"), "true")) {
            /* User must manually call httpWriteCache */
            flags |= HTTP_CACHE_MANUAL;
        }
        httpAddCache(route, methods, uris, extensions, mimeTypes, clientLifespan, serverLifespan, flags);
    }
}


static void parseContentCombine(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (smatch(child->value, "c")) {
            route->combine = 1;
            break;
        }
    }
}


static void parseContentCompress(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (mprGetJson(route->config, sfmt("app.http.content.minify[@ = '%s']", child->value))) {
            httpAddRouteMapping(route, child->value, "${1}.gz, min.${1}.gz, min.${1}");
        } else {
            httpAddRouteMapping(route, child->value, "${1}.gz");
        }
    }
}


#if DEPRECATED || 1
static void parseContentKeep(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (mprGetJson(prop, "[@=c]")) {
        route->keepSource = 1;
    }
}
#endif


static void parseContentMinify(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        /*
            Compressed and minified is handled in parseContentCompress
         */
        if (mprGetJson(route->config, sfmt("app.http.content.compress[@ = '%s']", child->value)) == 0) {
            httpAddRouteMapping(route, child->value, "min.${1}");
        }
    }
}


static void parseDatabase(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->database = prop->value;
}


static void parseDeleteUploads(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteAutoDelete(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseDomain(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetHostName(route->host, strim(prop->value, "http://", MPR_TRIM_START));
}


static void parseErrors(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddRouteErrorDocument(route, (int) stoi(prop->name), prop->value);
    }
}


static void parseFormatsResponse(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->responseFormat = prop->value;
}


static void parseHeadersAdd(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, child->name, child->value);
    }
}


static void parseHeadersRemove(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (prop->type & MPR_JSON_ARRAY) {
            httpAddRouteResponseHeader(route, HTTP_ROUTE_REMOVE_HEADER, child->value, 0);
        } else {
            httpAddRouteResponseHeader(route, HTTP_ROUTE_REMOVE_HEADER, child->name, 0);
        }
    }
}


static void parseHeadersSet(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddRouteResponseHeader(route, HTTP_ROUTE_SET_HEADER, child->name, child->value);
    }
}


static void parseIndexes(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    httpResetRouteIndexes(route);
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddRouteIndex(route, child->value);
    }
}


static void parseKeep(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->keepSource = (prop->type & MPR_JSON_TRUE) ? 1 : 0;
}


static void parseLanguages(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *path, *prefix, *suffix;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if ((prefix = mprGetJson(child, "prefix")) != 0) {
            httpAddRouteLanguageSuffix(route, child->name, child->value, HTTP_LANG_BEFORE);
        }
        if ((suffix = mprGetJson(child, "suffix")) != 0) {
            httpAddRouteLanguageSuffix(route, child->name, child->value, HTTP_LANG_AFTER);
        }
        if ((path = mprGetJson(child, "path")) != 0) {
            httpAddRouteLanguageDir(route, child->name, mprGetAbsPath(path));
        }
        if (smatch(mprGetJson(child, "default"), "default")) {
            httpSetRouteDefaultLanguage(route, child->name);
        }
    }
}


static void parseLimits(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpGraduateLimits(route, 0);
    parseAll(route, key, prop);
}


static void parseLimitsBuffer(HttpRoute *route, cchar *key, MprJson *prop)
{
    int     size;

    size = getint(prop->value);
    if (size > (1048576)) {
        size = 1048576;
    }
    route->limits->bufferSize = size;
}


static void parseLimitsCache(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetCacheLimits(route->host->responseCache, 0, 0, httpGetNumber(prop->value), 0);
}


static void parseLimitsCacheItem(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->cacheItemSize = getint(prop->value);
}


static void parseLimitsChunk(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->chunkSize = getint(prop->value);
}


static void parseLimitsClients(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->clientMax = getint(prop->value);
}


static void parseLimitsConnections(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->connectionsMax = getint(prop->value);
}


static void parseLimitsFiles(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetFilesLimit(getint(prop->value));
}


static void parseLimitsKeepAlive(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->keepAliveMax = getint(prop->value);
}


static void parseLimitsMemory(HttpRoute *route, cchar *key, MprJson *prop)
{
    ssize   maxMem;

    maxMem = (ssize) httpGetNumber(prop->value);
    mprSetMemLimits(maxMem / 100 * 85, maxMem, -1);
}


static void parseLimitsProcesses(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->processMax = getint(prop->value);
}


static void parseLimitsRequests(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->requestsPerClientMax = getint(prop->value);
}


static void parseLimitsRequestBody(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->receiveBodySize = httpGetNumber(prop->value);
}


static void parseLimitsRequestForm(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->receiveFormSize = httpGetNumber(prop->value);
}


static void parseLimitsRequestHeader(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->headerSize = getint(prop->value);
}


static void parseLimitsResponseBody(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->transmissionBodySize = httpGetNumber(prop->value);
}


static void parseLimitsSessions(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->sessionMax = getint(prop->value);
}


static void parseLimitsUri(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->uriSize = getint(prop->value);
}


static void parseLimitsUpload(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->uploadSize = httpGetNumber(prop->value);
}


static void parseLimitsWebSockets(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsMax = getint(prop->value);
}


static void parseLimitsWebSocketsMessage(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsMessageSize = getint(prop->value);
}


static void parseLimitsWebSocketsFrame(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsFrameSize = getint(prop->value);
}


static void parseLimitsWebSocketsPacket(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsPacketSize = getint(prop->value);
}


static void parseLimitsWorkers(HttpRoute *route, cchar *key, MprJson *prop)
{
    int     count;

    count = atoi(prop->value);
    if (count < 1) {
        count = MAXINT;
    }
    mprSetMaxWorkers(count);
}


static void parseMethods(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteMethods(route, getList(prop));
}


static void parseMode(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->mode = prop->value;
}


/*
    Match route only if param matches
 */
static void parseParams(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *name, *value;
    int         not, ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        name = mprGetJson(child, "name");
        value = mprGetJson(child, "value");
        not = smatch(mprGetJson(child, "equals"), "true") ? 0 : HTTP_ROUTE_NOT;
        httpAddRouteParam(route, name, value, not);
    }
}


static void parsePattern(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRoutePattern(route, prop->value, 0);
}


static void parsePipelineFilters(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *name, *extensions;
    int         flags, ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (prop->type & MPR_JSON_STRING) {
            flags = HTTP_STAGE_RX | HTTP_STAGE_TX;
            extensions = 0;
            name = child->value;
        } else {
            name = mprGetJson(child, "name");
            extensions = getList(mprGetJsonObj(child, "extensions"));
#if KEEP
            direction = mprGetJson(child, "direction");
            flags |= smatch(direction, "input") ? HTTP_STAGE_RX : 0;
            flags |= smatch(direction, "output") ? HTTP_STAGE_TX : 0;
            flags |= smatch(direction, "both") ? HTTP_STAGE_RX | HTTP_STAGE_TX : 0;
#else
            flags = HTTP_STAGE_RX | HTTP_STAGE_TX;
#endif
        }
        if (httpAddRouteFilter(route, name, extensions, flags) < 0) {
            httpParseError(route, "Cannot add filter %s", name);
            break;
        }
    }
}


static void parsePipelineHandlers(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    if (prop->type & MPR_JSON_STRING) {
        if (httpAddRouteHandler(route, prop->name, 0) < 0) {
            httpParseError(route, "Cannot add handler %s", prop->name);
        }

    } else {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (httpAddRouteHandler(route, child->name, getList(child)) < 0) {
                httpParseError(route, "Cannot add handler %s", child->name);
                break;
            }
        }
    }
}


static void parsePrefix(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRoutePrefix(route, sjoin(route->prefix, prop->value, 0));
}


static void createRedirectAlias(HttpRoute *route, int status, cchar *from, cchar *to)
{
    HttpRoute   *alias;
    cchar       *pattern;

    if (from == 0 || *from == '\0') {
        from = "/";
    }
    if (sends(from, "/")) {
        pattern = sfmt("^%s%s(.*)$", route->prefix, from);
    } else {
        /* Add a non-capturing optional trailing "/" */
        pattern = sfmt("^%s%s(?:/)*(.*)$", route->prefix, from);
    }
    alias = httpCreateAliasRoute(route, pattern, 0, 0);
    httpSetRouteName(alias, sfmt("redirect-%s", route->name));
    httpSetRouteMethods(alias, "*");
    httpSetRouteTarget(alias, "redirect", sfmt("%d %s/$1", status, to));
    if (sstarts(to, "https")) {
        httpAddRouteCondition(alias, "secure", 0, HTTP_ROUTE_NOT);
    }
    httpFinalizeRoute(alias);
}


static void parseRedirect(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *from, *status, *to;
    int         ji;

    if (prop->type & MPR_JSON_STRING) {
        createRedirectAlias(route, 0, "/", prop->value);

    } else {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (child->type & MPR_JSON_STRING) {
                from = "/";
                to = child->value;
                status = "302";
            } else {
                from = mprGetJson(child, "from");
                to = mprGetJson(child, "to");
                status = mprGetJson(child, "status");
            }
            createRedirectAlias(route, (int) stoi(status), from, to);
        }
    }
}


static void parseRouteName(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteName(route, prop->value);
}


PUBLIC HttpRouteSetProc httpDefineRouteSet(cchar *name, HttpRouteSetProc fn)
{
    Http                *http;
    HttpRouteSetProc    prior;

    http = MPR->httpService;
    prior = mprLookupKey(http->routeSets, name);
    mprAddKey(http->routeSets, name, fn);
    return prior;
}


PUBLIC void httpAddRouteSet(HttpRoute *route, cchar *set)
{
    HttpRouteSetProc    proc;

    if (set == 0 || *set == 0) {
        return;
    }
    if ((proc = mprLookupKey(route->http->routeSets, set)) != 0) {
        (proc)(route, set);
    } else {
        mprLog("error http config", 0, "Cannot find route set \"%s\"", set);
    }
}


static void setConfigDefaults(HttpRoute *route)
{
    route->mode = mprGetJson(route->config, "app.mode");
    if (smatch(route->mode, "debug")) {
        httpSetRouteShowErrors(route, 1);
        route->keepSource = 1;
    }
}


static void parseHttp(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *routes;

    setConfigDefaults(route);
    parseAll(route, key, prop);

    /*
        Property order is not guaranteed, so must ensure routes are processed after all outer properties.
     */
    if ((routes = mprGetJsonObj(prop, "routes")) != 0) {
        parseRoutes(route, key, routes);
    }
}


/*
    Must only be called directly via parseHttp as all other http.* keys must have already been processed.
 */
static void parseRoutes(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    HttpRoute   *newRoute;
    int         ji;

    if (route->loaded) {
        mprLog("warn http config", 1, "Skip reloading routes - must reboot if routes are modified");
        return;
    }
    if (prop->type & MPR_JSON_STRING) {
        httpAddRouteSet(route, prop->value);

    } else if (prop->type & MPR_JSON_ARRAY) {
        key = sreplace(key, ".routes", "");
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (child->type & MPR_JSON_STRING) {
                httpAddRouteSet(route, prop->value);

            } else if (child->type & MPR_JSON_OBJ) {
                /*
                    Create a new route
                 */
                newRoute = httpCreateInheritedRoute(route);
                httpSetRouteHost(newRoute, route->host);
                parseAll(newRoute, key, child);
                if (newRoute->error) {
                    break;
                }
                httpFinalizeRoute(newRoute);
            }
        }
    }
}


static void parseScheme(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (sstarts(prop->value, "https")) {
        httpAddRouteCondition(route, "secure", 0, 0);
    }
}


/*
    The server collection is only parsed for utilities and not if hosted
 */
static void parseServer(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (route->http->flags & HTTP_UTILITY) {
        parseAll(route, key, prop);
    }
}


static void parseServerAccount(HttpRoute *route, cchar *key, MprJson *prop)
{
    cchar       *value;

    if ((value = mprGetJson(prop, "user")) != 0) {
        if (!smatch(value, "_unchanged_") && !mprGetDebugMode()) {
            httpSetGroupAccount(value);
        }
    }
    if ((value = mprGetJson(prop, "user")) != 0) {
        if (!smatch(value, "_unchanged_") && !mprGetDebugMode()) {
            httpSetUserAccount(value);
        }
    }
}


static void parseServerChroot(HttpRoute *route, cchar *key, MprJson *prop)
{
#if ME_UNIX_LIKE
    char        *home;

    home = httpMakePath(route, 0, prop->value);
    if (chdir(home) < 0) {
        httpParseError(route, "Cannot change working directory to %s", home);
        return;
    }
    if (route->http->flags & HTTP_UTILITY) {
        /* Not running a web server but rather a utility like the "esp" generator program */
        mprLog("info http config", 2, "Change directory to: \"%s\"", home);
    } else {
        if (chroot(home) < 0) {
            if (errno == EPERM) {
                httpParseError(route, "Must be super user to use chroot\n");
            } else {
                httpParseError(route, "Cannot change change root directory to %s, errno %d\n", home, errno);
            }
            return;
        }
        mprLog("info http config", 2, "Chroot to: \"%s\"", home);
    }
#else
    mprLog("info http config", 2, "Chroot directive not supported on this operating system\n");
#endif
}


static void parseServerDefenses(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddDefenseFromJson(child->name, 0, child);
    }
}


static void parseServerListen(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpEndpoint    *endpoint;
    HttpHost        *host;
    MprJson         *child;
    char            *ip;
    int             ji, port, secure;

    host = route->host;
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        mprParseSocketAddress(child->value, &ip, &port, &secure, 80);
        if (port == 0) {
            httpParseError(route, "Bad or missing port %d in Listen directive", port);
            return;
        }
        endpoint = httpCreateEndpoint(ip, port, NULL);
        if (!host->defaultEndpoint) {
            httpSetHostDefaultEndpoint(host, endpoint);
        }
        if (secure) {
            if (route->ssl == 0) {
                if (route->parent && route->parent->ssl) {
                    route->ssl = mprCloneSsl(route->parent->ssl);
                } else {
                    route->ssl = mprCreateSsl(1);
                }
            }
            httpSecureEndpoint(endpoint, route->ssl);
            if (!host->secureEndpoint) {
                httpSetHostSecureEndpoint(host, endpoint);
            }
        }
        /*
            Single stack networks cannot support IPv4 and IPv6 with one socket. So create a specific IPv6 endpoint.
            This is currently used by VxWorks and Windows versions prior to Vista (i.e. XP)
         */
        if (!schr(prop->value, ':') && mprHasIPv6() && !mprHasDualNetworkStack()) {
            mprAddItem(route->http->endpoints, httpCreateEndpoint("::", port, NULL));
            httpSecureEndpoint(endpoint, route->ssl);
        }
    }
}


/*
    log: {
        location: 'stdout',
        level: '2',
        backup: 5,
        anew: true,
        size: '10MB',
        timestamp: '1hr',
    }
 */
static void parseServerLog(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprTicks    timestamp;
    cchar       *location;
    ssize       size;
    int         level, anew, backup;

    if (mprGetCmdlineLogging()) {
        mprLog("warn http config", 4, "Already logging. Ignoring log configuration");
        return;
    }
    location = mprGetJson(prop, "location");
    level = (int) stoi(mprGetJson(prop, "level"));
    backup = (int) stoi(mprGetJson(prop, "backup"));
    anew = smatch(mprGetJson(prop, "anew"), "true");
    size = (ssize) httpGetNumber(mprGetJson(prop, "size"));
    timestamp = httpGetNumber(mprGetJson(prop, "timestamp"));

    if (size < HTTP_TRACE_MIN_LOG_SIZE) {
        size = HTTP_TRACE_MIN_LOG_SIZE;
    }
    if (location == 0) {
        httpParseError(route, "Missing location");
        return;
    }
    if (!smatch(location, "stdout") && !smatch(location, "stderr")) {
        location = httpMakePath(route, 0, location);
    }
    mprSetLogBackup(size, backup, anew ? MPR_LOG_ANEW : 0);

    if (mprStartLogging(location, 0) < 0) {
        httpParseError(route, "Cannot write to error log: %s", location);
        return;
    }
    mprSetLogLevel(level);
    mprLogConfig();
    if (timestamp) {
        httpSetTimestamp(timestamp);
    }
}


static void parseServerMonitors(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    MprTicks    period;
    cchar       *counter, *expression, *limit, *relation, *defenses;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        defenses = mprGetJson(child, "defenses");
        expression = mprGetJson(child, "expression");
        period = httpGetTicks(mprGetJson(child, "period"));

        if (!httpTokenize(route, expression, "%S %S %S", &counter, &relation, &limit)) {
            httpParseError(route, "Cannot add monitor: %s", prop->name);
            break;
        }
        if (httpAddMonitor(counter, relation, getint(limit), period, defenses) < 0) {
            httpParseError(route, "Cannot add monitor: %s", prop->name);
            break;
        }
    }
}


static void parseServerPrefix(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteServerPrefix(route, prop->value);
}


static void parseShowErrors(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteShowErrors(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseSource(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteSource(route, prop->value);
}


static void parseSsl(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpRoute   *parent;

    parent = route->parent;
    if (route->ssl == 0) {
        if (parent && parent->ssl) {
            route->ssl = mprCloneSsl(parent->ssl);
        } else {
            route->ssl = mprCreateSsl(1);
        }
    } else {
        if (parent && route->ssl == parent->ssl) {
            route->ssl = mprCloneSsl(parent->ssl);
        }
    }
    parseAll(route, key, prop);
}


static void parseSslAuthorityFile(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetSslCaFile(route->ssl, prop->value);
}


static void parseSslAuthorityDirectory(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetSslCaPath(route->ssl, prop->value);
}


static void parseSslCertificate(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetSslCertFile(route->ssl, prop->value);
}


static void parseSslCiphers(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprAddSslCiphers(route->ssl, getList(prop));
}


static void parseSslKey(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetSslKeyFile(route->ssl, prop->value);
}


static void parseSslProvider(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetSslProvider(route->ssl, prop->value);
}


static void parseSslVerifyClient(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprVerifySslPeer(route->ssl, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseSslVerifyIssuer(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprVerifySslIssuer(route->ssl, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseStealth(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteStealth(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


/*
    Names: "close", "redirect", "run", "write"
    Rules:
        close:      [immediate]
        redirect:   status URI
        run:        ${DOCUMENT_ROOT}/${request:uri}
        run:        ${controller}-${name}
        write:      [-r] status "Hello World\r\n"
*/
static void parseTarget(HttpRoute *route, cchar *key, MprJson *prop)
{
    cchar   *name, *args;

    if (prop->type & MPR_JSON_OBJ) {
        name = mprGetJson(prop, "operation");
        args = mprGetJson(prop, "args");
    } else {
        name = "run";
        args = prop->value;
    }
    httpSetRouteTarget(route, name, args);
}


static void parseTimeouts(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpGraduateLimits(route, 0);
    parseAll(route, key, prop);
}


static void parseTimeoutsExit(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetExitTimeout(httpGetTicks(prop->value));
}


static void parseTimeoutsParse(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (! mprGetDebugMode()) {
        route->limits->requestParseTimeout = httpGetTicks(prop->value);
    }
}


static void parseTimeoutsInactivity(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (! mprGetDebugMode()) {
        route->limits->inactivityTimeout = httpGetTicks(prop->value);
    }
}


static void parseTimeoutsRequest(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (! mprGetDebugMode()) {
        route->limits->requestTimeout = httpGetTicks(prop->value);
    }
}


static void parseTimeoutsSession(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (! mprGetDebugMode()) {
        route->limits->sessionTimeout = httpGetTicks(prop->value);
    }
}


static void parseTrace(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *levels, *child;
    cchar       *location;
    ssize       size, maxContent;
    cchar       *format, *formatter;
    char        level;
    int         anew, backup, ji;

    if (route->trace && route->trace->flags & MPR_LOG_CMDLINE) {
        mprLog("info http config", 4, "Already tracing. Ignoring trace configuration");
        return;
    }
    size = (ssize) httpGetNumber(mprGetJson(prop, "size"));
    format = mprGetJson(prop, "format");
    formatter = mprGetJson(prop, "formatter");
    location = mprGetJson(prop, "location");
    level = (char) stoi(mprGetJson(prop, "level"));
    backup = (int) stoi(mprGetJson(prop, "backup"));
    anew = smatch(mprGetJson(prop, "anew"), "true");
    maxContent = (ssize) httpGetNumber(mprGetJson(prop, "content"));

    if (level < 0) {
        level = 0;
    } else if (level > 5) {
        level = 5;
    }
    if (size < (10 * 1000)) {
        httpParseError(route, "Trace log size is too small. Must be larger than 10K");
        return;
    }
    if (location == 0) {
        httpParseError(route, "Missing trace filename");
        return;
    }
    if (!smatch(location, "stdout") && !smatch(location, "stderr")) {
        location = httpMakePath(route, 0, location);
    }
    if ((levels = mprGetJsonObj(prop, "levels")) != 0) {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            httpSetTraceEventLevel(route->trace, child->name, (int) stoi(child->value));
        }
    }
    route->trace = httpCreateTrace(route->trace);
    httpSetTraceFormatterName(route->trace, formatter);
    httpSetTraceLogFile(route->trace, location, size, backup, format, anew ? MPR_LOG_ANEW : 0);
    httpSetTraceFormat(route->trace, format);
    httpSetTraceContentSize(route->trace, maxContent);
    httpSetTraceLevel(level);
}


static void parseUpdate(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->update = (prop->type & MPR_JSON_TRUE) ? 1 : 0;
}


static void parseXsrf(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteXsrf(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseInclude(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        parseFile(route, child->value);
    }
}


PUBLIC int httpInitParser()
{
    Http    *http;

    http = MPR->httpService;
    http->parsers = mprCreateHash(0, MPR_HASH_STATIC_VALUES);

    httpAddConfig("app", parseAll);
    httpAddConfig("app.http", parseHttp);
    httpAddConfig("app.http.auth", parseAuth);
    httpAddConfig("app.http.auth.login", parseAuthLogin);
    httpAddConfig("app.http.auth.realm", parseAuthRealm);
    httpAddConfig("app.http.auth.require", parseAll);
    httpAddConfig("app.http.auth.require.roles", parseAuthRequireRoles);
    httpAddConfig("app.http.auth.require.users", parseAuthRequireUsers);
    httpAddConfig("app.http.auth.roles", parseAuthRoles);
    httpAddConfig("app.http.auth.session.cookie", parseAuthSessionCookie);
    httpAddConfig("app.http.auth.session.vibility", parseAuthSessionVisibility);
    httpAddConfig("app.http.auth.store", parseAuthStore);
    httpAddConfig("app.http.auth.type", parseAuthType);
    httpAddConfig("app.http.auth.users", parseAuthUsers);
    httpAddConfig("app.http.cache", parseCache);
    httpAddConfig("app.http.content", parseAll);
    httpAddConfig("app.http.content.combine", parseContentCombine);
    httpAddConfig("app.http.content.minify", parseContentMinify);
    httpAddConfig("app.http.content.compress", parseContentCompress);
#if DEPRECATED || 1
    httpAddConfig("app.http.content.keep", parseContentKeep);
#endif
    httpAddConfig("app.http.database", parseDatabase);
    httpAddConfig("app.http.deleteUploads", parseDeleteUploads);
    httpAddConfig("app.http.domain", parseDomain);
    httpAddConfig("app.http.errors", parseErrors);
    httpAddConfig("app.http.formats", parseAll);
    httpAddConfig("app.http.formats.response", parseFormatsResponse);
    httpAddConfig("app.http.headers", parseAll);
    httpAddConfig("app.http.headers.add", parseHeadersAdd);
    httpAddConfig("app.http.headers.remove", parseHeadersRemove);
    httpAddConfig("app.http.headers.set", parseHeadersSet);
    httpAddConfig("app.http.indexes", parseIndexes);
    httpAddConfig("app.http.keep", parseKeep);
    httpAddConfig("app.http.languages", parseLanguages);
    httpAddConfig("app.http.limits", parseLimits);
    httpAddConfig("app.http.limits.buffer", parseLimitsBuffer);
    httpAddConfig("app.http.limits.cache", parseLimitsCache);
    httpAddConfig("app.http.limits.cacheItem", parseLimitsCacheItem);
    httpAddConfig("app.http.limits.chunk", parseLimitsChunk);
    httpAddConfig("app.http.limits.clients", parseLimitsClients);
    httpAddConfig("app.http.limits.connections", parseLimitsConnections);
    httpAddConfig("app.http.limits.keepAlive", parseLimitsKeepAlive);
    httpAddConfig("app.http.limits.files", parseLimitsFiles);
    httpAddConfig("app.http.limits.memory", parseLimitsMemory);
    httpAddConfig("app.http.limits.requestBody", parseLimitsRequestBody);
    httpAddConfig("app.http.limits.requestForm", parseLimitsRequestForm);
    httpAddConfig("app.http.limits.requestHeader", parseLimitsRequestHeader);
    httpAddConfig("app.http.limits.responseBody", parseLimitsResponseBody);
    httpAddConfig("app.http.limits.processes", parseLimitsProcesses);
    httpAddConfig("app.http.limits.requests", parseLimitsRequests);
    httpAddConfig("app.http.limits.sessions", parseLimitsSessions);
    httpAddConfig("app.http.limits.upload", parseLimitsUpload);
    httpAddConfig("app.http.limits.uri", parseLimitsUri);
    httpAddConfig("app.http.limits.webSockets", parseLimitsWebSockets);
    httpAddConfig("app.http.limits.webSocketsMessage", parseLimitsWebSocketsMessage);
    httpAddConfig("app.http.limits.webSocketsPacket", parseLimitsWebSocketsPacket);
    httpAddConfig("app.http.limits.webSocketsFrame", parseLimitsWebSocketsFrame);
    httpAddConfig("app.http.limits.workers", parseLimitsWorkers);
    httpAddConfig("app.http.methods", parseMethods);
    httpAddConfig("app.http.mode", parseMode);
    httpAddConfig("app.http.params", parseParams);
    httpAddConfig("app.http.pattern", parsePattern);
    httpAddConfig("app.http.pipeline", parseAll);
    httpAddConfig("app.http.pipeline.filter", parsePipelineFilters);
    httpAddConfig("app.http.pipeline.handlers", parsePipelineHandlers);
    httpAddConfig("app.http.prefix", parsePrefix);
    httpAddConfig("app.http.redirect", parseRedirect);
    httpAddConfig("app.http.routeName", parseRouteName);
    httpAddConfig("app.http.scheme", parseScheme);

    httpAddConfig("app.http.server", parseServer);
    httpAddConfig("app.http.server.account", parseServerAccount);
    httpAddConfig("app.http.server.chroot", parseServerChroot);
    httpAddConfig("app.http.server.defenses", parseServerDefenses);
    httpAddConfig("app.http.server.listen", parseServerListen);
    httpAddConfig("app.http.server.log", parseServerLog);
    httpAddConfig("app.http.server.monitors", parseServerMonitors);

    httpAddConfig("app.http.showErrors", parseShowErrors);
    httpAddConfig("app.http.source", parseSource);
    httpAddConfig("app.http.ssl", parseSsl);
    httpAddConfig("app.http.ssl.authority", parseAll);
    httpAddConfig("app.http.ssl.authority.file", parseSslAuthorityFile);
    httpAddConfig("app.http.ssl.authority.directory", parseSslAuthorityDirectory);
    httpAddConfig("app.http.ssl.certificate", parseSslCertificate);
    httpAddConfig("app.http.ssl.ciphers", parseSslCiphers);
    httpAddConfig("app.http.ssl.key", parseSslKey);
    httpAddConfig("app.http.ssl.provider", parseSslProvider);
    httpAddConfig("app.http.ssl.verify", parseAll);
    httpAddConfig("app.http.ssl.verify.client", parseSslVerifyClient);
    httpAddConfig("app.http.ssl.verify.issuer", parseSslVerifyIssuer);
    httpAddConfig("app.http.serverPrefix", parseServerPrefix);
    httpAddConfig("app.http.stealth", parseStealth);
    httpAddConfig("app.http.target", parseTarget);
    httpAddConfig("app.http.timeouts", parseTimeouts);
    httpAddConfig("app.http.timeouts.exit", parseTimeoutsExit);
    httpAddConfig("app.http.timeouts.parse", parseTimeoutsParse);
    httpAddConfig("app.http.timeouts.inactivity", parseTimeoutsInactivity);
    httpAddConfig("app.http.timeouts.request", parseTimeoutsRequest);
    httpAddConfig("app.http.timeouts.session", parseTimeoutsSession);
    httpAddConfig("app.http.trace", parseTrace);
    httpAddConfig("app.http.update", parseUpdate);
    httpAddConfig("app.http.xsrf", parseXsrf);
    httpAddConfig("directories", parseDirectories);
    httpAddConfig("include", parseInclude);

    return 0;
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
