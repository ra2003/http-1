/*
    auth.c - Authorization and access management

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************* Forwards ***********************************/

#undef  GRADUATE_HASH
#define GRADUATE_HASH(auth, field) \
    if (!auth->field) { \
        if (auth->parent && auth->field && auth->field == auth->parent->field) { \
            auth->field = mprCloneHash(auth->parent->field); \
        } else { \
            auth->field = mprCreateHash(0, MPR_HASH_STABLE); \
        } \
    }

static void computeAbilities(HttpAuth *auth, MprHash *abilities, cchar *role);
static void manageAuth(HttpAuth *auth, int flags);
static void manageRole(HttpRole *role, int flags);
static void manageUser(HttpUser *user, int flags);
static void formLogin(HttpConn *conn);
static bool configVerifyUser(HttpConn *conn, cchar *username, cchar *password);

/*********************************** Code *************************************/

PUBLIC void httpInitAuth()
{
    /*
        Types: basic, digest, form
     */
    httpCreateAuthType("basic", httpBasicLogin, httpBasicParse, httpBasicSetHeaders);
    httpCreateAuthType("digest", httpDigestLogin, httpDigestParse, httpDigestSetHeaders);
    httpCreateAuthType("form", formLogin, NULL, NULL);

    /*
        Stores: app, config, system
     */
    httpCreateAuthStore("app", NULL);
    httpCreateAuthStore("config", configVerifyUser);
#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
    httpCreateAuthStore("system", httpPamVerifyUser);
#endif

#if DEPRECATED || 1
    httpCreateAuthStore("file", configVerifyUser);
    httpCreateAuthStore("internal", configVerifyUser);
#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
    httpCreateAuthStore("pam", httpPamVerifyUser);
#endif
#endif
}


PUBLIC bool httpAuthenticate(HttpConn *conn)
{
    HttpRx      *rx;
    HttpAuth    *auth;
    cchar       *ip, *username;

    rx = conn->rx;
    auth = rx->route->auth;

    if (!rx->authenticated) {
        ip = httpGetSessionVar(conn, HTTP_SESSION_IP, 0);
        username = httpGetSessionVar(conn, HTTP_SESSION_USERNAME, 0);
        if (!smatch(ip, conn->ip) || !username) {
            if (auth->username && *auth->username) {
                httpLogin(conn, auth->username, NULL);
                username = httpGetSessionVar(conn, HTTP_SESSION_USERNAME, 0);
            }
            if (!username) {
                return 0;
            }
        }
        httpTrace(conn, "context", "authenticate", "msg=\"Using cached authentication data\", username=%s", username);
        conn->username = username;
        rx->authenticated = 1;
    }
    return rx->authenticated;
}


PUBLIC bool httpLoggedIn(HttpConn *conn)
{
    if (!conn->rx->authenticated) {
        httpAuthenticate(conn);
    }
    return conn->rx->authenticated;
}


/*
    Get the username and password credentials. If using an in-protocol auth scheme like basic|digest, the
    rx->authDetails will contain the credentials and the parseAuth callback will be invoked to parse.
    Otherwise, it is expected that "username" and "password" fields are present in the request parameters.

    This is called by authCondition which thereafter calls httpLogin
 */
PUBLIC bool httpGetCredentials(HttpConn *conn, cchar **username, cchar **password)
{
    HttpAuth    *auth;

    assert(username);
    assert(password);
    *username = *password = NULL;

    auth = conn->rx->route->auth;
    if (auth->type) {
        if (conn->authType && !smatch(conn->authType, auth->type->name)) {
            /* Do not call httpError so that a 401 response will be sent with WWW-Authenticate header */
            return 0;
        }
        if (auth->type->parseAuth && (auth->type->parseAuth)(conn, username, password) < 0) {
            httpError(conn, HTTP_CODE_BAD_REQUEST, "Access denied. Bad authentication data.");
            return 0;
        }
    } else {
        *username = httpGetParam(conn, "username", 0);
        *password = httpGetParam(conn, "password", 0);
    }
    return 1;
}


/*
    Login the user and create an authenticated session state store
 */
PUBLIC bool httpLogin(HttpConn *conn, cchar *username, cchar *password)
{
    HttpRx          *rx;
    HttpAuth        *auth;
    HttpSession     *session;
    HttpVerifyUser  verifyUser;

    rx = conn->rx;
    auth = rx->route->auth;
    if (!username || !*username) {
        httpTrace(conn, "context", "login", "msg=\"missing username\"");
        return 0;
    }
    if (!auth->store) {
        mprLog("error http auth", 0, "No AuthStore defined");
        return 0;
    }
    if ((verifyUser = auth->verifyUser) == 0) {
        if (auth->parent && (verifyUser = auth->parent->verifyUser) == 0) {
            verifyUser = auth->store->verifyUser;
        }
    }
    if (!verifyUser) {
        mprLog("error http auth", 0, "No user verification routine defined on route %s", rx->route->name);
        return 0;
    }
    if (auth->username && *auth->username) {
        /* If using auto-login, replace the username */
        username = auth->username;
        password = 0;
    }
    if (!(verifyUser)(conn, username, password)) {
        return 0;
    }
    if (!auth->store->noSession) {
        if ((session = httpCreateSession(conn)) == 0) {
            /* Too many sessions */
            return 0;
        }
        httpSetSessionVar(conn, HTTP_SESSION_USERNAME, username);
        httpSetSessionVar(conn, HTTP_SESSION_IP, conn->ip);
    }
    rx->authenticated = 1;
    conn->username = sclone(username);
    conn->encoded = 0;
    return 1;
}


/*
    Log the user out and remove the authentication username from the session state
 */
PUBLIC void httpLogout(HttpConn *conn) 
{
    conn->rx->authenticated = 0;
    httpDestroySession(conn);
}


/*
    Test if the user has the requisite abilities to perform an action. Abilities may be explicitly defined or if NULL,
    the abilities specified by the route are used.
 */
PUBLIC bool httpCanUser(HttpConn *conn, cchar *abilities)
{
    HttpAuth    *auth;
    char        *ability, *tok;
    MprKey      *kp;

    auth = conn->rx->route->auth;
    if (auth->permittedUsers && !mprLookupKey(auth->permittedUsers, conn->username)) {
        return 0;
    }
    if (!auth->abilities && !abilities) {
        /* No abilities are required */
        return 1;
    }
    if (!conn->username) {
        /* User not authenticated */
        return 0;
    }
    if (!conn->user && (conn->user = mprLookupKey(auth->userCache, conn->username)) == 0) {
        return 0;
    }
    if (abilities) {
        for (ability = stok(sclone(abilities), " \t,", &tok); abilities; abilities = stok(NULL, " \t,", &tok)) {
            if (!mprLookupKey(conn->user->abilities, ability)) {
                return 0;
            }
        }
    } else {
        for (ITERATE_KEYS(auth->abilities, kp)) {
            if (!mprLookupKey(conn->user->abilities, kp->key)) {
                return 0;
            }
        }
    }
    return 1;
}


//  TODO - need api to return username

PUBLIC bool httpIsAuthenticated(HttpConn *conn)
{
    return conn->rx->authenticated;
}


PUBLIC HttpAuth *httpCreateAuth()
{
    HttpAuth    *auth;

    if ((auth = mprAllocObj(HttpAuth, manageAuth)) == 0) {
        return 0;
    }
    auth->realm = MPR->emptyString;
    return auth;
}


PUBLIC HttpAuth *httpCreateInheritedAuth(HttpAuth *parent)
{
    HttpAuth      *auth;

    if ((auth = mprAllocObj(HttpAuth, manageAuth)) == 0) {
        return 0;
    }
    if (parent) {
        //  OPT. Structure assignment
        auth->allow = parent->allow;
        auth->deny = parent->deny;
        auth->type = parent->type;
        auth->store = parent->store;
        auth->flags = parent->flags;
        auth->qop = parent->qop;
        auth->realm = parent->realm;
        auth->permittedUsers = parent->permittedUsers;
        auth->abilities = parent->abilities;
        auth->userCache = parent->userCache;
        auth->roles = parent->roles;
        auth->loggedIn = parent->loggedIn;
        auth->loginPage = parent->loginPage;
        auth->username = parent->username;
        auth->verifyUser = parent->verifyUser;
        auth->parent = parent;
    }
    return auth;
}


static void manageAuth(HttpAuth *auth, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(auth->allow);
        mprMark(auth->deny);
        mprMark(auth->loggedIn);
        mprMark(auth->loginPage);
        mprMark(auth->permittedUsers);
        mprMark(auth->qop);
        mprMark(auth->realm);
        mprMark(auth->abilities);
        mprMark(auth->store);
        mprMark(auth->type);
        mprMark(auth->userCache);
        mprMark(auth->roles);
        mprMark(auth->username);
    }
}


static void manageAuthType(HttpAuthType *type, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(type->name);
    }
}


PUBLIC int httpCreateAuthType(cchar *name, HttpAskLogin askLogin, HttpParseAuth parseAuth, HttpSetAuth setAuth)
{
    Http            *http;
    HttpAuthType    *type;

    http = MPR->httpService;
    if ((type = mprAllocObj(HttpAuthType, manageAuthType)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    type->name = sclone(name);
    type->askLogin = askLogin;
    type->parseAuth = parseAuth;
    type->setAuth = setAuth;

    if (mprAddKey(http->authTypes, name, type) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    return 0;
}


static void manageAuthStore(HttpAuthStore *store, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(store->name);
    }
}


PUBLIC HttpAuthStore *httpCreateAuthStore(cchar *name, HttpVerifyUser verifyUser)
{
    Http            *http;
    HttpAuthStore   *store;

    if ((store = mprAllocObj(HttpAuthStore, manageAuthStore)) == 0) {
        return 0;
    }
    store->name = sclone(name);
    store->verifyUser = verifyUser;
    http = MPR->httpService;
    if (mprAddKey(http->authStores, name, store) == 0) {
        return 0;
    }
    return store;
}


PUBLIC void httpSetAuthVerify(HttpAuth *auth, HttpVerifyUser verifyUser)
{
    auth->verifyUser = verifyUser;
}


PUBLIC void httpSetAuthAllow(HttpAuth *auth, cchar *allow)
{
    GRADUATE_HASH(auth, allow);
    mprAddKey(auth->allow, sclone(allow), auth);
}


PUBLIC void httpSetAuthAnyValidUser(HttpAuth *auth)
{
    auth->permittedUsers = 0;
}


/*
    Can supply a roles or abilities in the "abilities" parameter 
 */
PUBLIC void httpSetAuthRequiredAbilities(HttpAuth *auth, cchar *abilities)
{
    char    *ability, *tok;

    GRADUATE_HASH(auth, abilities);
    for (ability = stok(sclone(abilities), " \t,", &tok); abilities; abilities = stok(NULL, " \t,", &tok)) {
        computeAbilities(auth, auth->abilities, ability);
    }
}


PUBLIC void httpSetAuthDeny(HttpAuth *auth, cchar *client)
{
    GRADUATE_HASH(auth, deny);
    mprAddKey(auth->deny, sclone(client), auth);
}


PUBLIC void httpSetAuthOrder(HttpAuth *auth, int order)
{
    auth->flags &= (HTTP_ALLOW_DENY | HTTP_DENY_ALLOW);
    auth->flags |= (order & (HTTP_ALLOW_DENY | HTTP_DENY_ALLOW));
}


/*
    Form login service routine. Called in response to a form-based login request. Only used when httpSetAuthForm is utilized.
    The password is clear-text so this must be used over SSL to be secure.
 */
static void loginServiceProc(HttpConn *conn)
{
    HttpAuth    *auth;
    cchar       *username, *password, *referrer;

    auth = conn->rx->route->auth;
    username = httpGetParam(conn, "username", 0);
    password = httpGetParam(conn, "password", 0);

    if (httpLogin(conn, username, password)) {
        if ((referrer = httpGetSessionVar(conn, "referrer", 0)) != 0) {
            /*
                Preserve protocol scheme from existing connection
             */
            HttpUri *where = httpCreateUri(referrer, 0);
            httpCompleteUri(where, conn->rx->parsedUri);
            referrer = httpUriToString(where, 0);
            httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, referrer);
        } else {
            if (auth->loggedIn) {
                httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, auth->loggedIn);
            } else {
                httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, "~");
            }
        }
    } else {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, auth->loginPage);
    }
}


static void logoutServiceProc(HttpConn *conn)
{
    httpLogout(conn);
    httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, conn->rx->route->auth->loginPage);
}


PUBLIC void httpSetAuthForm(HttpRoute *parent, cchar *loginPage, cchar *loginService, cchar *logoutService, cchar *loggedIn)
{
    HttpAuth    *auth;
    HttpRoute   *route;
    bool        secure;

    secure = 0;
    auth = parent->auth;
    auth->loginPage = sclone(loginPage);
    if (loggedIn) {
        auth->loggedIn = sclone(loggedIn);
    }
    /*
        Create routes without auth for the loginPage, loginService and logoutService
     */
    if ((route = httpCreateInheritedRoute(parent)) != 0) {
        if (sstarts(loginPage, "https:///")) {
            loginPage = &loginPage[8];
            secure = 1;
        }
        httpSetRoutePattern(route, loginPage, 0);
        route->auth->type = 0;
        if (secure) {
            httpAddRouteCondition(route, "secure", 0, 0);
        }
        httpFinalizeRoute(route);
    }
    if (loginService && *loginService) {
        if (sstarts(loginService, "https:///")) {
            loginService = &loginService[8];
            secure = 1;
        }
        route = httpCreateActionRoute(parent, loginService, loginServiceProc);
        httpSetRouteMethods(route, "POST");
        route->auth->type = 0;
        if (secure) {
            httpAddRouteCondition(route, "secure", 0, 0);
        }
    }
    if (logoutService && *logoutService) {
        if (sstarts(logoutService, "https://")) {
            logoutService = &logoutService[8];
            secure = 1;
        }
        httpSetRouteMethods(route, "POST");
        route = httpCreateActionRoute(parent, logoutService, logoutServiceProc);
        route->auth->type = 0;
        if (secure) {
            httpAddRouteCondition(route, "secure", 0, 0);
        }
    }
}


PUBLIC void httpSetAuthQop(HttpAuth *auth, cchar *qop)
{
    auth->qop = sclone(qop);
}


PUBLIC void httpSetAuthRealm(HttpAuth *auth, cchar *realm)
{
    auth->realm = sclone(realm);
}


/*
    Can also achieve this via abilities
 */
PUBLIC void httpSetAuthPermittedUsers(HttpAuth *auth, cchar *users)
{
    char    *user, *tok;

    GRADUATE_HASH(auth, permittedUsers);
    for (user = stok(sclone(users), " \t,", &tok); users; users = stok(NULL, " \t,", &tok)) {
        if (smatch(user, "*")) {
            auth->permittedUsers = 0;
            break;
        } else {
            mprAddKey(auth->permittedUsers, user, user);
        }
    }
}


PUBLIC int httpSetAuthStore(HttpAuth *auth, cchar *store)
{
    Http    *http;

    http = MPR->httpService;
    if ((auth->store = mprLookupKey(http->authStores, store)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    if (smatch(store, "system")) {
#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
        if (auth->type && smatch(auth->type->name, "digest")) {
            mprLog("critical http auth", 0, "Cannot use the PAM password store with digest authentication");
            return MPR_ERR_BAD_ARGS;
        }
#else
        mprLog("critical http auth", 0, "PAM is not supported in the current configuration");
        return MPR_ERR_BAD_ARGS;
#endif
    }
    GRADUATE_HASH(auth, userCache);
    return 0;
}


PUBLIC void httpSetAuthStoreSessions(HttpAuthStore *store, bool noSession)
{
    assert(store);
    store->noSession = noSession;
}


PUBLIC int httpSetAuthType(HttpAuth *auth, cchar *type, cchar *details)
{
    Http    *http;

    http = MPR->httpService;
    if ((auth->type = mprLookupKey(http->authTypes, type)) == 0) {
        mprLog("critical http auth", 0, "Cannot find auth type %s", type);
        return MPR_ERR_CANT_FIND;
    }
    if (!auth->store) {
        httpSetAuthStore(auth, "config");
    }
    return 0;
}


/*
    This implements auto-loging without requiring a password
 */
PUBLIC void httpSetAuthUsername(HttpAuth *auth, cchar *username)
{
    auth->username = sclone(username);
}


PUBLIC HttpAuthType *httpLookupAuthType(cchar *type)
{
    Http    *http;

    http = MPR->httpService;
    return mprLookupKey(http->authTypes, type);
}


static void manageRole(HttpRole *role, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(role->name);
        mprMark(role->abilities);
    }
}


PUBLIC HttpRole *httpAddRole(HttpAuth *auth, cchar *name, cchar *abilities)
{
    HttpRole    *role;
    char        *ability, *tok;

    GRADUATE_HASH(auth, roles);
    if ((role = mprLookupKey(auth->roles, name)) == 0) {
        if ((role = mprAllocObj(HttpRole, manageRole)) == 0) {
            return 0;
        }
        role->name = sclone(name);
    }
    role->abilities = mprCreateHash(0, 0);
    for (ability = stok(sclone(abilities), " \t", &tok); ability; ability = stok(NULL, " \t", &tok)) {
        mprAddKey(role->abilities, ability, role);
    }
    if (mprAddKey(auth->roles, name, role) == 0) {
        return 0;
    }
    mprDebug("http auth", 5, "Role \"%s\" defined, abilities=\"%s\"", role->name, abilities);
    return role;
}


PUBLIC HttpRole *httpLookupRole(HttpAuth *auth, cchar *role)
{
    return mprLookupKey(auth->roles, role);
}


PUBLIC int httpRemoveRole(HttpAuth *auth, cchar *role)
{
    if (auth->roles == 0 || !mprLookupKey(auth->roles, role)) {
        return MPR_ERR_CANT_ACCESS;
    }
    mprRemoveKey(auth->roles, role);
    return 0;
}


static void manageUser(HttpUser *user, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(user->password);
        mprMark(user->name);
        mprMark(user->abilities);
        mprMark(user->roles);
    }
}


PUBLIC HttpUser *httpAddUser(HttpAuth *auth, cchar *name, cchar *password, cchar *roles)
{
    HttpUser    *user;

    if (!auth->userCache) {
        auth->userCache = mprCreateHash(0, 0);
    }
    if ((user = mprLookupKey(auth->userCache, name)) == 0) {
        if ((user = mprAllocObj(HttpUser, manageUser)) == 0) {
            return 0;
        }
        user->name = sclone(name);
    }
    user->password = sclone(password);
    if (roles) {
        user->roles = sclone(roles);
        httpComputeUserAbilities(auth, user);
    }
    if (mprAddKey(auth->userCache, name, user) == 0) {
        return 0;
    }
    return user;
}



PUBLIC HttpUser *httpLookupUser(HttpAuth *auth, cchar *name)
{
    return mprLookupKey(auth->userCache, name);
}


PUBLIC int httpRemoveUser(HttpAuth *auth, cchar *name)
{
    if (!mprLookupKey(auth->userCache, name)) {
        return MPR_ERR_CANT_ACCESS;
    }
    mprRemoveKey(auth->userCache, name);
    return 0;
}


/*
    Compute the set of user abilities from the user roles. Role strings can be either roles or abilities. 
    Expand roles into the equivalent set of abilities.
 */
static void computeAbilities(HttpAuth *auth, MprHash *abilities, cchar *role)
{
    MprKey      *ap;
    HttpRole    *rp;

    if ((rp = mprLookupKey(auth->roles, role)) != 0) {
        /* Interpret as a role */
        for (ITERATE_KEYS(rp->abilities, ap)) {
            if (!mprLookupKey(abilities, ap->key)) {
                mprAddKey(abilities, ap->key, MPR->oneString);
            }
        }
    } else {
        /* Not found as a role: Interpret role as an ability */
        mprAddKey(abilities, role, MPR->oneString);
    }
}


/*
    Compute the set of user abilities from the user roles. User ability strings can be either roles or abilities. Expand
    roles into the equivalent set of abilities.
 */
PUBLIC void httpComputeUserAbilities(HttpAuth *auth, HttpUser *user)
{
    char        *ability, *tok;

    user->abilities = mprCreateHash(0, 0);
    for (ability = stok(sclone(user->roles), " \t,", &tok); ability; ability = stok(NULL, " \t,", &tok)) {
        computeAbilities(auth, user->abilities, ability);
    }
#if ME_DEBUG
    {
        MprBuf *buf = mprCreateBuf(0, 0);
        MprKey *ap;
        for (ITERATE_KEYS(user->abilities, ap)) {
            mprPutToBuf(buf, "%s ", ap->key);
        }
        mprAddNullToBuf(buf);
        mprDebug("http auth", 5, "User \"%s\" has abilities: %s", user->name, mprGetBufStart(buf));
    }
#endif
}


PUBLIC void httpComputeAllUserAbilities(HttpAuth *auth)
{
    MprKey      *kp;
    HttpUser    *user;

    for (ITERATE_KEY_DATA(auth->userCache, kp, user)) {
        httpComputeUserAbilities(auth, user);
    }
}


/*
    Verify the user password based on the users defined via the configuration files. 
    Password may be NULL only if using auto-login.
 */
static bool configVerifyUser(HttpConn *conn, cchar *username, cchar *password)
{
    HttpRx      *rx;
    HttpAuth    *auth;
    bool        success;
    char        *requiredPassword;

    rx = conn->rx;
    auth = rx->route->auth;
    if (!conn->user && (conn->user = mprLookupKey(auth->userCache, username)) == 0) {
        httpTrace(conn, "context", "login", "msg=\"Unknown user\", username=%s", username);
        return 0;
    }
    if (password) {
        requiredPassword = (rx->passwordDigest) ? rx->passwordDigest : conn->user->password;
        if (sncmp(requiredPassword, "BF", 2) == 0 && slen(requiredPassword) > 4 && isdigit(requiredPassword[2]) && 
                requiredPassword[3] == ':') {
            /* Blowifsh */
            success = mprCheckPassword(sfmt("%s:%s:%s", username, auth->realm, password), conn->user->password);

        } else {
            if (!conn->encoded) {
                password = mprGetMD5(sfmt("%s:%s:%s", username, auth->realm, password));
                conn->encoded = 1;
            }
            success = smatch(password, requiredPassword);
        }
        if (success) {
            httpTrace(conn, "context", "login", "msg=\"User authenticated\", username=%s", username);
        } else {
            httpTrace(conn, "context", "login", "msg=\"Password failed to authenticate\", username=%s", username);
        }
        return success;
    }
    return 1;
}


/*
    Web form-based authentication callback to ask the user to login via a web page
 */
static void formLogin(HttpConn *conn)
{
    if (conn->rx->route->auth && conn->rx->route->auth->loginPage) {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, conn->rx->route->auth->loginPage);
    } else {
        httpError(conn, HTTP_CODE_UNAUTHORIZED, "Access Denied. Login required");
    }
}


PUBLIC void httpSetConnUser(HttpConn *conn, HttpUser *user)
{
    conn->user = user;
}

#undef  GRADUATE_HASH

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
