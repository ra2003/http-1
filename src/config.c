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

static void parseAuthRoles(HttpRoute *route, cchar *key, MprJson *prop);
static void parseAuthStore(HttpRoute *route, cchar *key, MprJson *prop);
static void parseRoutes(HttpRoute *route, cchar *key, MprJson *prop);

/************************************** Code **********************************/
/*
    Define a configuration callbacks. The key is specified as it is used in json files.
 */

PUBLIC HttpParseCallback httpAddConfig(cchar *key, HttpParseCallback callback)
{
    HttpParseCallback   prior;

    prior = mprLookupKey(HTTP->parsers, key);
    mprAddKey(HTTP->parsers, key, callback);
    return prior;
}


PUBLIC void httpParseError(HttpRoute *route, cchar *fmt, ...)
{
    HttpRoute   *rp;
    va_list     args;
    char        *msg;

    va_start(args, fmt);
    msg = sfmtv(fmt, args);
    mprLog("error http config", 0, "%s", msg);
    va_end(args);
    route->error = 1;
    for (rp = route; rp; rp = rp->parent) {
        rp->error = 1;
    }
}


PUBLIC void httpParseWarn(HttpRoute *route, cchar *fmt, ...)
{
    va_list     args;
    char        *msg;

    va_start(args, fmt);
    msg = sfmtv(fmt, args);
    mprLog("warn http config", 1, "%s", msg);
    va_end(args);
}


/*
    Convert a JSON string to a space-separated C string
 */
static cchar *getList(MprJson *prop)
{
    char    *cp, *p;

    if (prop == 0) {
        return 0;
    }
    if ((cp = mprJsonToString(prop, 0)) == 0) {
        return 0;
    }
    if (*cp == '[') {
        cp = strim(cp, "[]", 0);
    }
    for (p = cp; *p; p++) {
        if (*p == '"' || *p == ',') {
            *p = ' ';
        }
    }
    if (*cp == ' ') {
        cp = strim(cp, " \t", 0);
    }
    return cp;
}


/*
    Blend the pak.modes[pak.mode] up to the top level
 */
static void blendMode(HttpRoute *route, MprJson *config)
{
    MprJson     *modeObj;
    cchar       *mode;

    /*
        Use existing mode from route->config. Blending of config should already have taken place, 
        so pak.mode should be defined.
     */
    mode = mprGetJson(route->config, "pak.mode");
    if (!mode) {
        mode = mprGetJson(config, "pak.mode");
        if (!mode) {
            mode = sclone("debug");
        }
    }
    route->mode = mode;
    if ((route->debug = smatch(mode, "debug")) != 0) {
        httpSetRouteShowErrors(route, 1);
        route->keepSource = 1;
    }
    /*
        Http uses top level modes
        Pak uses top level pak.modes
     */
    if ((modeObj = mprGetJsonObj(config, sfmt("modes.%s", mode))) == 0) {
        modeObj = mprGetJsonObj(config, sfmt("pak.modes.%s", mode));
    }
    if (modeObj) {
        mprBlendJson(route->config, modeObj, MPR_JSON_OVERWRITE);
        httpParseAll(route, 0, modeObj);
    }
}


PUBLIC int parseInclude(HttpRoute *route, MprJson *config, MprJson *inc)
{
    MprJson     *child, *obj;
    MprList     *files;
    cchar       *data, *errorMsg, *path;
    int         ji, next;

    for (ITERATE_CONFIG(route, inc, child, ji)) {
        files = mprGlobPathFiles(".", child->value, MPR_PATH_NO_DIRS | MPR_PATH_RELATIVE);
        for (ITERATE_ITEMS(files, path, next)) {
            if ((data = mprReadPathContents(path, NULL)) == 0) {
                httpParseError(route, "Cannot read configuration from \"%s\"", path);
                return MPR_ERR_CANT_READ;
            }
            if ((obj = mprParseJsonEx(data, 0, 0, 0, &errorMsg)) == 0) {
                httpParseError(route, "Cannot parse %s: error %s", path, errorMsg);
                return MPR_ERR_CANT_READ;
            }
            mprBlendJson(config, obj, MPR_JSON_COMBINE);
        }
    }
    return 0;
}


PUBLIC void httpInitConfig(HttpRoute *route)
{
    route->error = 0;
    route->config = 0;
    route->clientConfig = 0;
}


PUBLIC int httpLoadConfig(HttpRoute *route, cchar *path)
{
    MprJson     *config, *obj;
    cchar       *data, *errorMsg;

    /*
        Order of processing matters. First load the file and then blend included files into the same json obj.
        Then blend the mode directives and then assign/blend into the route config.
        Lastly, parse the json config dom.
     */
    if ((data = mprReadPathContents(path, NULL)) == 0) {
        mprLog("error http config", 0, "Cannot read configuration from \"%s\"", path);
        return MPR_ERR_CANT_READ;
    }
    if ((config = mprParseJsonEx(data, 0, 0, 0, &errorMsg)) == 0) {
        mprLog("error http config", 0, "Cannot parse %s: error %s", path, errorMsg);
        return MPR_ERR_CANT_READ;
    }
    if ((obj = mprGetJsonObj(config, "include")) != 0) {
        parseInclude(route, config, obj);
    }
#if DEPRECATE || 1
{
    MprJson *obj;
    if ((obj = mprGetJsonObj(config, "app.http")) != 0) {
        mprRemoveJson(config, "app.http");
        mprSetJsonObj(config, "http", obj);
    }
    if ((obj = mprGetJsonObj(config, "app.esp")) != 0) {
        mprRemoveJson(config, "app.esp");
        mprSetJsonObj(config, "esp", obj);
    }
}
#endif
    blendMode(route, config);
    if (route->config) {
        mprBlendJson(route->config, config, MPR_JSON_COMBINE);
    } else {
        route->config = config;
    }
    route->error = 0;

    httpParseAll(route, 0, config);
    if (route->error) {
        route->config = 0;
        return MPR_ERR_BAD_STATE;
    }
    return 0;
}


#if UNUSED
PUBLIC int httpFinalizeConfig(HttpRoute *route)
{
    HttpHost    *host;
    HttpRoute   *rp;
    MprJson     *routes;
    cchar       *home;
    int         nextHost, nextRoute;

    if (route->error) {
        return MPR_ERR_BAD_STATE;
    }
    /*
        Property order is not guaranteed, so must ensure routes are processed after all outer properties.
     */
    if ((routes = mprGetJsonObj(route->config, "http.routes")) != 0) {
        parseRoutes(route, "http.routes", routes);
    }
    httpAddHostToEndpoints(route->host);

#if UNUSED
    for (nextHost = 0; (host = mprGetNextItem(route->http->hosts, &nextHost)) != 0; ) {
        for (nextRoute = 0; (rp = mprGetNextItem(host->routes, &nextRoute)) != 0; ) {
            if (!mprLookupKey(rp->extensions, "")) {
                if (!rp->handler) {
                    httpAddRouteHandler(rp, "fileHandler", "");
                    httpAddRouteIndex(rp, "index.html");
                }
            }
            if (rp->parent == route) {
                rp->clientConfig = route->clientConfig;
            }
        }
    }
#endif
#if ME_UNIX_LIKE
    if ((home = mprGetJson(route->config, "http.server.chroot")) != 0) {
        home = httpMakePath(route, 0, home);
        if (chdir(home) < 0) {
            httpParseError(route, "Cannot change working directory to %s", home);
            return MPR_ERR_BAD_STATE;
        }
        if (route->flags & HTTP_ROUTE_NO_LISTEN) {
            /* Not running a web server */
            mprLog("info http config", 2, "Change directory to: \"%s\"", home);
        } else {
            if (chroot(home) < 0) {
                if (errno == EPERM) {
                    httpParseError(route, "Must be super user to use chroot\n");
                } else {
                    httpParseError(route, "Cannot change change root directory to %s, errno %d\n", home, errno);
                }
            } else {
                mprLog("info http config", 2, "Chroot to: \"%s\"", home);
            }
        }
    }
#endif
    if (route->error) {
        route->config = 0;
        return MPR_ERR_BAD_STATE;
    }
    return 0;
}
#endif


#if UNUSED
static void copyMappings(HttpRoute *route, MprJson *dest, MprJson *obj)
{
    MprJson     *child, *job, *jvalue;
    cchar       *key, *value;
    int         ji;

    for (ITERATE_CONFIG(route, obj, child, ji)) {
        if (child->type & MPR_JSON_OBJ) {
            job = mprCreateJson(MPR_JSON_OBJ);
            copyMappings(route, job, child);
            mprSetJsonObj(dest, child->name, job);
        } else {
            key = child->value;
            if (sends(key, "|time")) {
                key = ssplit(sclone(key), " \t|", NULL);
                if ((value = mprGetJson(route->config, key)) != 0) {
                    mprSetJson(dest, child->name, itos(httpGetTicks(value)), MPR_JSON_NUMBER);
                }
            } else {
                if ((jvalue = mprGetJsonObj(route->config, key)) != 0) {
                    mprSetJsonObj(dest, child->name, mprCloneJson(jvalue));
                }
            }
        }
    }
}
#endif


/**************************************** Parser Callbacks ****************************************/

static void parseKey(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpParseCallback   parser;

    key = key ? sjoin(key, ".", prop->name, NULL) : prop->name;
    if ((parser = mprLookupKey(HTTP->parsers, key)) != 0) {
        parser(route, key, prop);
    }
}


PUBLIC void httpParseAll(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        parseKey(route, key, child);
    }
}


#if DEPRECATED || 1
static void parseApp(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpParseAll(route, 0, prop);
}
#endif


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


static void parseAliases(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    HttpRoute   *alias;
    MprPath     info;
    cchar       *path, *prefix;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        prefix = child->name;
        path = child->value;
        if (!path || !prefix) {
            httpParseError(route, "Alias is missing path or prefix properties");
            break;
        }
        mprGetPathInfo(path, &info);
        if (info.isDir) {
            alias = httpCreateAliasRoute(route, prefix, path, 0);
            if (sends(prefix, "/")) {
                httpSetRoutePattern(alias, sfmt("^%s(.*)$", prefix), 0);
            } else {
                /* Add a non-capturing optional trailing "/" */
                httpSetRoutePattern(alias, sfmt("^%s(?:/)*(.*)$", prefix), 0);
            }
            httpSetRouteTarget(alias, "run", "$1");
        } else {
            alias = httpCreateAliasRoute(route, sjoin("^", prefix, NULL), 0, 0);
            httpSetRouteTarget(alias, "run", path);
        }
        httpFinalizeRoute(alias);
    }
}


static void parseAuth(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (prop->type & MPR_JSON_STRING) {
        /* Permits auth: "app" to set the store */
        parseAuthStore(route, key, prop);
    } else if (prop->type == MPR_JSON_OBJ) {
        httpParseAll(route, key, prop);
    }
}


static void parseAuthAutoName(HttpRoute *route, cchar *key, MprJson *prop)
{
    /* Automatic login as this user. Password not required */
    httpSetAuthUsername(route->auth, prop->value);
}


/*
    Parse roles and compute abilities
 */
static void parseAuthAutoRoles(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprHash     *abilities;
    MprKey      *kp;
    MprJson     *child, *job;
    int         ji;

    if ((job = mprGetJsonObj(route->config, "http.auth.roles")) != 0) {
        parseAuthRoles(route, "http.auth.roles", job);
    }
    abilities = mprCreateHash(0, 0);
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpComputeRoleAbilities(route->auth, abilities, child->value);
    }
    if (mprGetHashLength(abilities) > 0) {
        job = mprCreateJson(MPR_JSON_ARRAY);
        for (ITERATE_KEYS(abilities, kp)) {
            mprSetJson(job, "$", kp->key, 0);
        }
        mprSetJsonObj(route->config, "http.auth.auto.abilities", job);
    }
}


static void parseAuthLogin(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetAuthLogin(route->auth, prop->value);
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
        httpSetAuthRequiredAbilities(route->auth, child->value);
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


static void parseAuthSessionCookiePersist(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteCookiePersist(route, smatch(prop->value, "true"));
}


static void parseAuthSessionEnable(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetAuthSession(route->auth, 0);
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
    HttpAuth    *auth;
    cchar       *type;

    auth = route->auth;
    type = prop->value;

    if (httpSetAuthType(auth, type, 0) < 0) {
        httpParseError(route, "The %s AuthType is not available on this platform", type);
    }
    if (type && !smatch(type, "none")) {
        httpAddRouteCondition(route, "auth", 0, 0);
    }
    if (smatch(type, "basic") || smatch(type, "digest")) {
        /*
            Must not use cookies by default, otherwise, the client cannot logoff.
         */
        httpSetAuthSession(auth, 0);
    }
}


static void parseAuthUsers(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *roles, *password;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        password = mprReadJson(child, "password");
        roles = getList(mprReadJsonObj(child, "roles"));
        if (httpAddUser(route->auth, child->name, password, roles) < 0) {
            httpParseError(route, "Cannot add user %s", child->name);
            break;
        }
        if (!route->auth->store) {
            httpSetAuthStore(route->auth, "config");
        }
    }
}


static void parseCache(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    MprTicks    clientLifespan, serverLifespan;
    cchar       *methods, *extensions, *urls, *mimeTypes, *client, *server;
    int         flags, ji;

    clientLifespan = serverLifespan = 0;
    if (prop->type & MPR_JSON_STRING && smatch(prop->value, "true")) {
        httpAddCache(route, 0, 0, 0, 0, 3600 * 1000, 0, HTTP_CACHE_CLIENT | HTTP_CACHE_STATIC);
    } else {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            flags = 0;
            if ((client = mprReadJson(child, "client")) != 0) {
                flags |= HTTP_CACHE_CLIENT;
                clientLifespan = httpGetTicks(client);
            }
            if ((server = mprReadJson(child, "server")) != 0) {
                flags |= HTTP_CACHE_SERVER;
                serverLifespan = httpGetTicks(server);
            }
            methods = getList(mprReadJsonObj(child, "methods"));
            urls = getList(mprReadJsonObj(child, "urls"));
#if DEPRECATE || 1
            if (urls == 0) {
                if ((urls = getList(mprReadJsonObj(child, "urls"))) != 0) {
                    mprLog("error http config", 0, "Using deprecated property \"uris\", use \"urls\" instead");
                }
            }
#endif
            mimeTypes = getList(mprReadJsonObj(child, "mime"));
            extensions = getList(mprReadJsonObj(child, "extensions"));
            if (smatch(mprReadJson(child, "unique"), "true")) {
                /* Uniquely cache requests with different params */
                flags |= HTTP_CACHE_UNIQUE;
            }
            if (smatch(mprReadJson(child, "manual"), "true")) {
                /* User must manually call httpWriteCache */
                flags |= HTTP_CACHE_MANUAL;
            }
            httpAddCache(route, methods, urls, extensions, mimeTypes, clientLifespan, serverLifespan, flags);
        }
    }
}


static void parseCgiEscape(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteEnvEscape(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseCgiPrefix(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteEnvPrefix(route, prop->value);
}


static void parseCompress(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (smatch(prop->value, "true")) {
        httpAddRouteMapping(route, "", "${1}.gz, min.${1}.gz, min.${1}");
    } else if (prop->type & MPR_JSON_ARRAY) {
        httpAddRouteMapping(route, mprJsonToString(prop, 0), "${1}.gz, min.${1}.gz, min.${1}");
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


static void parseDocuments(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, X_OK)) {
        httpParseError(route, "Cannot locate documents directory %s", prop->value);
    } else {
        httpSetRouteDocuments(route, prop->value);
    }
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


static void parseHandler(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (httpSetRouteHandler(route, prop->value) < 0) {
        httpParseError(route, "Cannot add handler \"%s\"", prop->value);
    }
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


static void parseHome(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, X_OK)) {
        httpParseError(route, "Cannot locate home directory %s", prop->value);
    } else {
        httpSetRouteHome(route, prop->value);
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
        if ((prefix = mprReadJson(child, "prefix")) != 0) {
            httpAddRouteLanguageSuffix(route, child->name, child->value, HTTP_LANG_BEFORE);
        }
        if ((suffix = mprReadJson(child, "suffix")) != 0) {
            httpAddRouteLanguageSuffix(route, child->name, child->value, HTTP_LANG_AFTER);
        }
        if ((path = mprReadJson(child, "path")) != 0) {
            httpAddRouteLanguageDir(route, child->name, mprGetAbsPath(path));
        }
        if (smatch(mprReadJson(child, "default"), "default")) {
            httpSetRouteDefaultLanguage(route, child->name);
        }
    }
}


static void parseLimits(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpGraduateLimits(route, 0);
    httpParseAll(route, key, prop);
}


static void parseLimitsBuffer(HttpRoute *route, cchar *key, MprJson *prop)
{
    int     size;

    size = httpGetInt(prop->value);
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
    route->limits->cacheItemSize = httpGetInt(prop->value);
}


static void parseLimitsChunk(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->chunkSize = httpGetInt(prop->value);
}


static void parseLimitsClients(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->clientMax = httpGetInt(prop->value);
}


static void parseLimitsConnections(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->connectionsMax = httpGetInt(prop->value);
}


static void parseLimitsFiles(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetFilesLimit(httpGetInt(prop->value));
}


static void parseLimitsDepletion(HttpRoute *route, cchar *key, MprJson *prop)
{
    cchar   *policy;
    int     flags;

    flags = MPR_ALLOC_POLICY_EXIT;
    policy = prop->value;

    if (scmp(policy, "restart") == 0) {
#if VXWORKS
        flags = MPR_ALLOC_POLICY_RESTART;
#else
        /* Appman will restart */
        flags = MPR_ALLOC_POLICY_EXIT;
#endif
    } else if (scmp(policy, "continue") == 0) {
        flags = MPR_ALLOC_POLICY_PRUNE;
    } else {
        httpParseError(route, "Unknown limit depletion policy '%s'", policy);
        return;
    }
    mprSetMemPolicy(flags);
}

static void parseLimitsKeepAlive(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->keepAliveMax = httpGetInt(prop->value);
}


static void parseLimitsMemory(HttpRoute *route, cchar *key, MprJson *prop)
{
    ssize   maxMem;

    maxMem = (ssize) httpGetNumber(prop->value);
    mprSetMemLimits(maxMem / 100 * 85, maxMem, -1);
}


static void parseLimitsProcesses(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->processMax = httpGetInt(prop->value);
}


static void parseLimitsRequests(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->requestsPerClientMax = httpGetInt(prop->value);
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
    route->limits->headerSize = httpGetInt(prop->value);
}


static void parseLimitsResponseBody(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->transmissionBodySize = httpGetNumber(prop->value);
}


static void parseLimitsSessions(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->sessionMax = httpGetInt(prop->value);
}


static void parseLimitsUri(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->uriSize = httpGetInt(prop->value);
}


static void parseLimitsUpload(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->uploadSize = httpGetNumber(prop->value);
}


static void parseLimitsWebSockets(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsMax = httpGetInt(prop->value);
}


static void parseLimitsWebSocketsMessage(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsMessageSize = httpGetInt(prop->value);
}


static void parseLimitsWebSocketsFrame(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsFrameSize = httpGetInt(prop->value);
}


static void parseLimitsWebSocketsPacket(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsPacketSize = httpGetInt(prop->value);
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
    httpSetRouteMethods(route, supper(getList(prop)));
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
        name = mprReadJson(child, "name");
        value = mprReadJson(child, "value");
        not = smatch(mprReadJson(child, "equals"), "true") ? 0 : HTTP_ROUTE_NOT;
        httpAddRouteParam(route, name, value, not);
    }
}


static void parsePattern(HttpRoute *route, cchar *key, MprJson *prop)
{
    cchar   *pattern;

    pattern = prop->value;
    if (pattern && *pattern != '^') {
        pattern = sfmt("^%s%s", route->parent->prefix, pattern);
    }
    httpSetRoutePattern(route, pattern, 0);
}


static void parsePipelineFilters(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *name, *extensions;
    int         flags, ji;

    flags = HTTP_STAGE_RX | HTTP_STAGE_TX;

    if (prop->type & MPR_JSON_STRING) {
        name = prop->value;
        if (httpAddRouteFilter(route, prop->value, NULL, flags) < 0) {
            httpParseWarn(route, "Cannot add filter %s", name);
        }
    } else if (prop->type & MPR_JSON_OBJ) {
        name = mprReadJson(prop, "name");
        extensions = getList(mprReadJsonObj(prop, "extensions"));
        if (httpAddRouteFilter(route, name, extensions, flags) < 0) {
            httpParseWarn(route, "Cannot add filter %s", name);
        }
    } else if (prop->type & MPR_JSON_ARRAY) {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            parsePipelineFilters(route, key, child);
        }
    }
}


/*
    pipeline: {
        handlers: 'espHandler',                     //  For all extensions
        handlers: {
            espHandler: [ '*.esp, '*.xesp' ],
        },
    },
 */
static void parsePipelineHandlers(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    if (prop->type & MPR_JSON_STRING) {
        if (httpAddRouteHandler(route, prop->value, "") < 0) {
            httpParseWarn(route, "Handler \"%s\" is not available", prop->name);
        }

    } else {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (httpAddRouteHandler(route, child->name, getList(child)) < 0) {
                httpParseWarn(route, "Handler \"%s\" is not available", child->name);
            }
        }
    }
}


static void parsePrefix(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRoutePrefix(route, prop->value);
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
    httpSetRouteMethods(alias, "*");
    httpSetRouteTarget(alias, "redirect", sfmt("%d %s/$1", status, to));
    if (sstarts(to, "https")) {
        httpAddRouteCondition(alias, "secure", to, HTTP_ROUTE_REDIRECT);
    }
    httpFinalizeRoute(alias);
}


static void parseRedirect(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *from, *status, *to;
    int         ji;

    if (prop->type & MPR_JSON_STRING) {
        if (smatch(prop->value, "secure") ||smatch(prop->value, "https://")) {
            httpAddRouteCondition(route, "secure", "https://", HTTP_ROUTE_REDIRECT);
        } else {
            createRedirectAlias(route, 0, "/", prop->value);
        }

    } else {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (child->type & MPR_JSON_STRING) {
                from = "/";
                to = child->value;
                status = "302";
            } else {
                from = mprReadJson(child, "from");
                to = mprReadJson(child, "to");
                status = mprReadJson(child, "status");
            }
            if (smatch(child->value, "secure")) {
                httpAddRouteCondition(route, "secure", "https://", HTTP_ROUTE_REDIRECT);
            } else {
                createRedirectAlias(route, (int) stoi(status), from, to);
            }
        }
    }
}


/*
    Create RESTful routes
 */
static void parseResources(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child, *groups, *singletons, *sets;
    int         ji;

    if ((sets = mprReadJsonObj(prop, "sets")) != 0) {
        for (ITERATE_CONFIG(route, sets, child, ji)) {
            httpAddRouteSet(route, child->value);
        }
    }
    if ((groups = mprReadJsonObj(prop, "groups")) != 0) {
        for (ITERATE_CONFIG(route, groups, child, ji)) {
            httpAddResourceGroup(route, child->value);
        }
    }
    if ((singletons = mprReadJsonObj(prop, "singletons")) != 0) {
        for (ITERATE_CONFIG(route, singletons, child, ji)) {
            httpAddResource(route, child->value);
        }
    }
}


PUBLIC HttpRouteSetProc httpDefineRouteSet(cchar *name, HttpRouteSetProc fn)
{
    HttpRouteSetProc    prior;

    prior = mprLookupKey(HTTP->routeSets, name);
    mprAddKey(HTTP->routeSets, name, fn);
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


static void parseHttp(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpParseAll(route, key, prop);
}


static void parseRoute(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpRoute   *newRoute;
    cchar       *pattern;

    if (prop->type & MPR_JSON_STRING) {
        httpAddRouteSet(route, prop->value);

    } else if (prop->type & MPR_JSON_OBJ) {
        newRoute = 0;
        pattern = mprReadJson(prop, "pattern");
        if (pattern) {
            newRoute = httpLookupRoute(route->host, pattern);
            if (!newRoute) {
                newRoute = httpCreateInheritedRoute(route);
                httpSetRouteHost(newRoute, route->host);
            }
        } else {
            newRoute = route;
        }
        httpParseAll(newRoute, key, prop);
        if (pattern) {
            httpFinalizeRoute(newRoute);
        }
    }
}


static void parseRoutes(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

#if UNUSED
    if (route->loaded) {
        mprLog("warn http config", 1, "Skip reloading routes - must reboot if routes are modified");
        return;
    }
#endif
    if (prop->type & MPR_JSON_STRING) {
        httpAddRouteSet(route, prop->value);

    } else if (prop->type & MPR_JSON_OBJ) {
        key = sreplace(key, ".routes", "");
        parseRoute(route, key, prop);

    } else if (prop->type & MPR_JSON_ARRAY) {
        key = sreplace(key, ".routes", "");
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            parseRoute(route, key, child);
        }
    }
}


static void parseScheme(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (sstarts(prop->value, "https")) {
        httpAddRouteCondition(route, "secure", 0, 0);
    }
}


static void parseServerAccount(HttpRoute *route, cchar *key, MprJson *prop)
{
    cchar       *value;

    if (route->flags & HTTP_ROUTE_HOSTED) {
        return;
    }
    if ((value = mprReadJson(prop, "user")) != 0) {
        if (!smatch(value, "_unchanged_") && !mprGetDebugMode()) {
            httpSetGroupAccount(value);
        }
    }
    if ((value = mprReadJson(prop, "user")) != 0) {
        if (!smatch(value, "_unchanged_") && !mprGetDebugMode()) {
            httpSetUserAccount(value);
        }
    }
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

    if (route->flags & HTTP_ROUTE_HOSTED) {
        return;
    }
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

    if (route->flags & HTTP_ROUTE_HOSTED) {
        return;
    }
    if (mprGetCmdlineLogging()) {
        mprLog("warn http config", 4, "Already logging. Ignoring log configuration");
        return;
    }
    location = mprReadJson(prop, "location");
    level = (int) stoi(mprReadJson(prop, "level"));
    backup = (int) stoi(mprReadJson(prop, "backup"));
    anew = smatch(mprReadJson(prop, "anew"), "true");
    size = (ssize) httpGetNumber(mprReadJson(prop, "size"));
    timestamp = httpGetNumber(mprReadJson(prop, "timestamp"));

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


static void parseServerModules(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprModule   *module;
    MprJson     *child;
    cchar       *entry, *name, *path;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        name = mprGetJson(child, "name");
        path = mprGetJson(child, "path");
        if (!name) {
            name = path;
        }
        if (!path) {
            path = sjoin("libmod_", name, ME_SHOBJ, NULL);
        }
        if ((module = mprLookupModule(name)) != 0) {
#if ME_STATIC
            mprLog("info http config", 2, "Activating module (Builtin) %s", name);
#endif
            continue;
        }
        entry = sfmt("http%sInit", stitle(name));
        module = mprCreateModule(name, path, entry, HTTP);

        if (mprLoadModule(module) < 0) {
#if DEPRECATED || 1
            module->entry = sfmt("ma%sInit", stitle(name));
            if (mprLoadModule(module) < 0) {
                httpParseError(route, "Cannot load module: %s", path);
            }
#endif
            break;
        }
    }
}

static void parseServerMonitors(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    MprTicks    period;
    cchar       *counter, *expression, *limit, *relation, *defenses;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        defenses = mprReadJson(child, "defenses");
        expression = mprReadJson(child, "expression");
        period = httpGetTicks(mprReadJson(child, "period"));

        if (!httpTokenize(route, expression, "%S %S %S", &counter, &relation, &limit)) {
            httpParseError(route, "Cannot add monitor: %s", prop->name);
            break;
        }
        if (httpAddMonitor(counter, relation, httpGetInt(limit), period, defenses) < 0) {
            httpParseError(route, "Cannot add monitor: %s", prop->name);
            break;
        }
    }
}


#if DEPRECATE || 1
static void parseServerPrefix(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteServerPrefix(route, prop->value);
}
#endif


static void parseShowErrors(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteShowErrors(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseSource(HttpRoute *route, cchar *key, MprJson *prop)
{
/*  UNUSED - messes up esp controllers/source
    httpSetRouteSource(route, mprJoinPath(route->home, prop->value));
*/
    httpSetRouteSource(route, prop->value);
}


static void parseSsl(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpRoute   *parent;

    if (route->flags & HTTP_ROUTE_HOSTED) {
        return;
    }
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
    httpParseAll(route, key, prop);
}


static void parseSslAuthorityFile(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, R_OK)) {
        httpParseError(route, "Cannot find file %s", prop->value);
    } else {
        mprSetSslCaFile(route->ssl, prop->value);
    }
}


static void parseSslAuthorityDirectory(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, R_OK)) {
        httpParseError(route, "Cannot find file %s", prop->value);
    } else {
        mprSetSslCaPath(route->ssl, prop->value);
    }
}


static void parseSslCertificate(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, R_OK)) {
        httpParseError(route, "Cannot find file %s", prop->value);
    } else {
        mprSetSslCertFile(route->ssl, prop->value);
    }
}


static void parseSslCiphers(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprAddSslCiphers(route->ssl, getList(prop));
}


static void parseSslKey(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, R_OK)) {
        httpParseError(route, "Cannot find file %s", prop->value);
    } else {
        mprSetSslKeyFile(route->ssl, prop->value);
    }
}


static void parseSslProtocols(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *value;
    int         bit, clear, ji, mask;

    mask = 0;
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        value = child->value;
        clear = 0;
        if (sstarts(value, "+")) {
            value++;
        } else if (sstarts(value, "-")) {
            clear = 1;
            value++;
        }
        bit = 0;
        if (scaselessmatch(value, "all")) {
            /* Do not include insecure SSLv2 and SSLv3 */
            bit = MPR_PROTO_TLSV1 | MPR_PROTO_TLSV1_2;
        } else if (scaselessmatch(value, "sslv2")) {
            /* SSLv2 is insecure */
            bit = MPR_PROTO_SSLV2;
        } else if (scaselessmatch(value, "sslv3")) {
            /* SSLv3 is insecure */
            bit = MPR_PROTO_SSLV3;
        } else if (scaselessmatch(value, "tlsv1") || scaselessmatch(value, "tls")) {
            bit = MPR_PROTO_TLSV1;
        } else if (scaselessmatch(value, "tlsv1.1")) {
            bit = MPR_PROTO_TLSV1_1;
        } else if (scaselessmatch(value, "tlsv1.2")) {
            bit = MPR_PROTO_TLSV1_2;
        }
        if (clear) {
            mask &= ~bit;
        } else {
            mask |= bit;
        }
    }
    mprSetSslProtocols(route->ssl, mask);
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
        name = mprReadJson(prop, "operation");
        args = mprReadJson(prop, "args");
    } else {
        name = "run";
        args = prop->value;
    }
    httpSetRouteTarget(route, name, args);
}


static void parseTimeouts(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpGraduateLimits(route, 0);
    httpParseAll(route, key, prop);
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
    size = (ssize) httpGetNumber(mprReadJson(prop, "size"));
    format = mprReadJson(prop, "format");
    formatter = mprReadJson(prop, "formatter");
    location = mprReadJson(prop, "location");
    level = (char) stoi(mprReadJson(prop, "level"));
    backup = (int) stoi(mprReadJson(prop, "backup"));
    anew = smatch(mprReadJson(prop, "anew"), "true");
    maxContent = (ssize) httpGetNumber(mprReadJson(prop, "content"));

    if (level < 0) {
        level = 0;
    } else if (level > 5) {
        level = 5;
    }
    if (size < (10 * 1000)) {
        size = 10 * 1000 * 1000;
        mprLog("warn http config", 0, "Trace log size is too small, setting to 10MB. Must be larger than 10K.");
    }
    if (location == 0) {
        httpParseError(route, "Missing trace filename");
        return;
    }
    if (!smatch(location, "stdout") && !smatch(location, "stderr")) {
        location = httpMakePath(route, 0, location);
    }
    if ((levels = mprReadJsonObj(prop, "levels")) != 0) {
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


PUBLIC int httpInitParser()
{
    HTTP->parsers = mprCreateHash(0, MPR_HASH_STATIC_VALUES);

    /*
        Parse callbacks keys are specified as they are defined in the json files
     */
#if DEPRECATED || 1
    httpAddConfig("app", parseApp);
#endif
    httpAddConfig("http", parseHttp);
    httpAddConfig("http.aliases", parseAliases);
    httpAddConfig("http.auth", parseAuth);
    httpAddConfig("http.auth.auto", httpParseAll);
    httpAddConfig("http.auth.auto.name", parseAuthAutoName);
    httpAddConfig("http.auth.auto.roles", parseAuthAutoRoles);
    httpAddConfig("http.auth.login", parseAuthLogin);
    httpAddConfig("http.auth.realm", parseAuthRealm);
    httpAddConfig("http.auth.require", httpParseAll);
    httpAddConfig("http.auth.require.roles", parseAuthRequireRoles);
    httpAddConfig("http.auth.require.users", parseAuthRequireUsers);
    httpAddConfig("http.auth.roles", parseAuthRoles);
    httpAddConfig("http.auth.session.cookie", parseAuthSessionCookie);
    httpAddConfig("http.auth.session.persist", parseAuthSessionCookiePersist);
    httpAddConfig("http.auth.session.enable", parseAuthSessionEnable);
    httpAddConfig("http.auth.session.vibility", parseAuthSessionVisibility);
    httpAddConfig("http.auth.store", parseAuthStore);
    httpAddConfig("http.auth.type", parseAuthType);
    httpAddConfig("http.auth.users", parseAuthUsers);
    httpAddConfig("http.cache", parseCache);
    httpAddConfig("http.cgi", httpParseAll);
    httpAddConfig("http.cgi.escape", parseCgiEscape);
    httpAddConfig("http.cgi.prefix", parseCgiPrefix);
#if KEEP
    httpAddConfig("http.content", httpParseAll);
    httpAddConfig("http.content.combine", parseContentCombine);
    httpAddConfig("http.content.minify", parseContentMinify);
    httpAddConfig("http.content.compress", parseContentCompress);
#if DEPRECATED
    httpAddConfig("http.content.keep", parseContentKeep);
#endif
#endif
    httpAddConfig("http.compress", parseCompress);
    httpAddConfig("http.database", parseDatabase);
    httpAddConfig("http.deleteUploads", parseDeleteUploads);
    httpAddConfig("http.directories", parseDirectories);
    httpAddConfig("http.documents", parseDocuments);
    httpAddConfig("http.domain", parseDomain);
    httpAddConfig("http.errors", parseErrors);
    httpAddConfig("http.formats", httpParseAll);
    httpAddConfig("http.formats.response", parseFormatsResponse);
    httpAddConfig("http.handler", parseHandler);
    httpAddConfig("http.headers", httpParseAll);
    httpAddConfig("http.headers.add", parseHeadersAdd);
    httpAddConfig("http.headers.remove", parseHeadersRemove);
    httpAddConfig("http.headers.set", parseHeadersSet);
    httpAddConfig("http.home", parseHome);
    httpAddConfig("http.indexes", parseIndexes);
    httpAddConfig("http.keep", parseKeep);
    httpAddConfig("http.languages", parseLanguages);
    httpAddConfig("http.limits", parseLimits);
    httpAddConfig("http.limits.buffer", parseLimitsBuffer);
    httpAddConfig("http.limits.cache", parseLimitsCache);
    httpAddConfig("http.limits.cacheItem", parseLimitsCacheItem);
    httpAddConfig("http.limits.chunk", parseLimitsChunk);
    httpAddConfig("http.limits.clients", parseLimitsClients);
    httpAddConfig("http.limits.connections", parseLimitsConnections);
    httpAddConfig("http.limits.depletion", parseLimitsDepletion);
    httpAddConfig("http.limits.keepAlive", parseLimitsKeepAlive);
    httpAddConfig("http.limits.files", parseLimitsFiles);
    httpAddConfig("http.limits.memory", parseLimitsMemory);
    httpAddConfig("http.limits.requestBody", parseLimitsRequestBody);
    httpAddConfig("http.limits.requestForm", parseLimitsRequestForm);
    httpAddConfig("http.limits.requestHeader", parseLimitsRequestHeader);
    httpAddConfig("http.limits.responseBody", parseLimitsResponseBody);
    httpAddConfig("http.limits.processes", parseLimitsProcesses);
    httpAddConfig("http.limits.requests", parseLimitsRequests);
    httpAddConfig("http.limits.sessions", parseLimitsSessions);
    httpAddConfig("http.limits.upload", parseLimitsUpload);
    httpAddConfig("http.limits.uri", parseLimitsUri);
    httpAddConfig("http.limits.webSockets", parseLimitsWebSockets);
    httpAddConfig("http.limits.webSocketsMessage", parseLimitsWebSocketsMessage);
    httpAddConfig("http.limits.webSocketsPacket", parseLimitsWebSocketsPacket);
    httpAddConfig("http.limits.webSocketsFrame", parseLimitsWebSocketsFrame);
    httpAddConfig("http.limits.workers", parseLimitsWorkers);
    httpAddConfig("http.methods", parseMethods);
    httpAddConfig("http.mode", parseMode);
    httpAddConfig("http.params", parseParams);
    httpAddConfig("http.pattern", parsePattern);
    httpAddConfig("http.pipeline", httpParseAll);
    httpAddConfig("http.pipeline.filters", parsePipelineFilters);
    httpAddConfig("http.pipeline.handlers", parsePipelineHandlers);
    httpAddConfig("http.prefix", parsePrefix);
    httpAddConfig("http.redirect", parseRedirect);
    httpAddConfig("http.routes", parseRoutes);
    httpAddConfig("http.resources", parseResources);
    httpAddConfig("http.scheme", parseScheme);

    httpAddConfig("http.server", httpParseAll);
    httpAddConfig("http.server.account", parseServerAccount);
    httpAddConfig("http.server.defenses", parseServerDefenses);
    httpAddConfig("http.server.listen", parseServerListen);
    httpAddConfig("http.server.log", parseServerLog);
    httpAddConfig("http.server.modules", parseServerModules);
    httpAddConfig("http.server.monitors", parseServerMonitors);

#if DEPRECATED || 1
    httpAddConfig("http.server.ssl", parseSsl);
    httpAddConfig("http.server.ssl.authority", httpParseAll);
    httpAddConfig("http.server.ssl.authority.file", parseSslAuthorityFile);
    httpAddConfig("http.server.ssl.authority.directory", parseSslAuthorityDirectory);
    httpAddConfig("http.server.ssl.certificate", parseSslCertificate);
    httpAddConfig("http.server.ssl.ciphers", parseSslCiphers);
    httpAddConfig("http.server.ssl.key", parseSslKey);
    httpAddConfig("http.server.ssl.provider", parseSslProvider);
    httpAddConfig("http.server.ssl.protocols", parseSslProtocols);
    httpAddConfig("http.server.ssl.verify", httpParseAll);
    httpAddConfig("http.server.ssl.verify.client", parseSslVerifyClient);
    httpAddConfig("http.server.ssl.verify.issuer", parseSslVerifyIssuer);
#endif

    httpAddConfig("http.showErrors", parseShowErrors);
    httpAddConfig("http.source", parseSource);
#if DEPRECATE || 1
    httpAddConfig("http.serverPrefix", parseServerPrefix);
#endif
    httpAddConfig("http.ssl", parseSsl);
    httpAddConfig("http.ssl.authority", httpParseAll);
    httpAddConfig("http.ssl.authority.file", parseSslAuthorityFile);
    httpAddConfig("http.ssl.authority.directory", parseSslAuthorityDirectory);
    httpAddConfig("http.ssl.certificate", parseSslCertificate);
    httpAddConfig("http.ssl.ciphers", parseSslCiphers);
    httpAddConfig("http.ssl.key", parseSslKey);
    httpAddConfig("http.ssl.provider", parseSslProvider);
    httpAddConfig("http.ssl.protocols", parseSslProtocols);
    httpAddConfig("http.ssl.verify", httpParseAll);
    httpAddConfig("http.ssl.verify.client", parseSslVerifyClient);
    httpAddConfig("http.ssl.verify.issuer", parseSslVerifyIssuer);

    httpAddConfig("http.stealth", parseStealth);
    httpAddConfig("http.target", parseTarget);
    httpAddConfig("http.timeouts", parseTimeouts);
    httpAddConfig("http.timeouts.exit", parseTimeoutsExit);
    httpAddConfig("http.timeouts.parse", parseTimeoutsParse);
    httpAddConfig("http.timeouts.inactivity", parseTimeoutsInactivity);
    httpAddConfig("http.timeouts.request", parseTimeoutsRequest);
    httpAddConfig("http.timeouts.session", parseTimeoutsSession);
    httpAddConfig("http.trace", parseTrace);
    httpAddConfig("http.update", parseUpdate);
    httpAddConfig("http.xsrf", parseXsrf);
    httpAddConfig("directories", parseDirectories);

    return 0;
}

/*
    @copy   default

    Copyright (c) Embedthis Software. All Rights Reserved.

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
