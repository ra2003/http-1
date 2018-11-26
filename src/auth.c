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

static void manageAuth(HttpAuth *auth, int flags);
static void formLogin(HttpConn *conn);
PUBLIC int formParse(HttpConn *conn, cchar **username, cchar **password);
static bool configVerifyUser(HttpConn *conn, cchar *username, cchar *password);

/*********************************** Code *************************************/

PUBLIC void httpInitAuth()
{
    /*
        Auth protocol types: basic, digest, form
        These are typically not used for web frameworks like ESP or PHP
     */
    httpCreateAuthType("basic", httpBasicLogin, httpBasicParse, httpBasicSetHeaders);
    httpCreateAuthType("digest", httpDigestLogin, httpDigestParse, httpDigestSetHeaders);
    httpCreateAuthType("form", formLogin, formParse, NULL);

    /*
        Stores: app, config, system
     */
    httpCreateAuthStore("app", NULL);
    httpCreateAuthStore("config", configVerifyUser);
#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
    httpCreateAuthStore("system", httpPamVerifyUser);
#endif
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
        auth->flags = parent->flags;
        auth->allow = parent->allow;
        auth->cipher = parent->cipher;
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
        auth->loggedOutPage = parent->loggedOutPage;
        auth->loggedInPage = parent->loggedInPage;
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
        mprMark(auth->cipher);
        mprMark(auth->realm);
        mprMark(auth->allow);
        mprMark(auth->deny);
        mprMark(auth->userCache);
        mprMark(auth->roles);
        mprMark(auth->abilities);
        mprMark(auth->permittedUsers);
        mprMark(auth->loginPage);
        mprMark(auth->loggedInPage);
        mprMark(auth->loggedOutPage);
        mprMark(auth->username);
        mprMark(auth->qop);
        mprMark(auth->type);
        mprMark(auth->store);
    }
}


static void manageAuthType(HttpAuthType *type, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(type->name);
    }
}



static void manageAuthStore(HttpAuthStore *store, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(store->name);
    }
}

/*
    Authenticate a user using the session stored username. This will set HttpRx.authenticated if authentication succeeds.
    Note: this does not call httpLogin except for auto-login cases where a password is not used.
 */
PUBLIC bool httpAuthenticate(HttpConn *conn)
{
    HttpRx      *rx;
    HttpAuth    *auth;
    cchar       *ip, *username;

    rx = conn->rx;
    auth = rx->route->auth;

    if (!rx->authenticateProbed) {
        rx->authenticateProbed = 1;
        ip = httpGetSessionVar(conn, HTTP_SESSION_IP, 0);
        username = httpGetSessionVar(conn, HTTP_SESSION_USERNAME, 0);
        if (!smatch(ip, conn->ip) || !username) {
            if (auth->username && *auth->username) {
                /* Auto-login */
                httpLogin(conn, auth->username, NULL);
                username = httpGetSessionVar(conn, HTTP_SESSION_USERNAME, 0);
            }
            if (!username) {
                return 0;
            }
        }
        httpTrace(conn->trace, "auth.login.authenticated", "context",
            "msg: 'Using cached authentication data', username:'%s'", username);
        conn->username = username;
        rx->authenticated = 1;
    }
    return rx->authenticated;
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


PUBLIC HttpAuthStore *httpCreateAuthStore(cchar *name, HttpVerifyUser verifyUser)
{
    HttpAuthStore   *store;

    if ((store = mprAllocObj(HttpAuthStore, manageAuthStore)) == 0) {
        return 0;
    }
    store->name = sclone(name);
    store->verifyUser = verifyUser;
    if (mprAddKey(HTTP->authStores, name, store) == 0) {
        return 0;
    }
    return store;
}



PUBLIC int httpCreateAuthType(cchar *name, HttpAskLogin askLogin, HttpParseAuth parseAuth, HttpSetAuth setAuth)
{
    HttpAuthType    *type;

    if ((type = mprAllocObj(HttpAuthType, manageAuthType)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    type->name = sclone(name);
    type->askLogin = askLogin;
    type->parseAuth = parseAuth;
    type->setAuth = setAuth;

    if (mprAddKey(HTTP->authTypes, name, type) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    return 0;
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
    if (!auth || !auth->type) {
        return 0;
    }
    if (auth->type) {
        if (conn->authType && !smatch(conn->authType, auth->type->name)) {
            if (!(smatch(auth->type->name, "form") && conn->rx->flags & HTTP_POST)) {
                /* If a posted form authentication, ignore any basic|digest details in request */
                return 0;
            }
        }
        if (auth->type->parseAuth && (auth->type->parseAuth)(conn, username, password) < 0) {
            return 0;
        }
    } else {
        *username = httpGetParam(conn, "username", 0);
        *password = httpGetParam(conn, "password", 0);
    }
    return 1;
}


PUBLIC bool httpIsAuthenticated(HttpConn *conn)
{
    return httpAuthenticate(conn);
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
        httpTrace(conn->trace, "auth.login.error", "error", "msg:'missing username'");
        return 0;
    }
    if (!auth->store) {
        mprLog("error http auth", 0, "No AuthStore defined");
        return 0;
    }
    if ((verifyUser = auth->verifyUser) == 0) {
        if (!auth->parent || (verifyUser = auth->parent->verifyUser) == 0) {
            verifyUser = auth->store->verifyUser;
        }
    }
    if (!verifyUser) {
        mprLog("error http auth", 0, "No user verification routine defined on route %s", rx->route->pattern);
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
    if (!(auth->flags & HTTP_AUTH_NO_SESSION) && !auth->store->noSession) {
        if ((session = httpCreateSession(conn)) == 0) {
            /* Too many sessions */
            return 0;
        }
        httpSetSessionVar(conn, HTTP_SESSION_USERNAME, username);
        httpSetSessionVar(conn, HTTP_SESSION_IP, conn->ip);
    }
    rx->authenticated = 1;
    rx->authenticateProbed = 1;
    conn->username = sclone(username);
    conn->encoded = 0;
    return 1;
}


PUBLIC bool httpIsLoggedIn(HttpConn *conn)
{
    return httpAuthenticate(conn);
}


/*
    Log the user out and remove the authentication username from the session state
 */
PUBLIC void httpLogout(HttpConn *conn)
{
    conn->rx->authenticated = 0;
    httpDestroySession(conn);
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


PUBLIC void httpSetAuthLogin(HttpAuth *auth, cchar *value)
{
    auth->loginPage = sclone(value);
}


/*
    Web form login service routine. Called in response to a form-based login request when defined via httpSetAuthLogin.
    It is expected that "authCondition" has already authenticated the request.
 */
static void loginServiceProc(HttpConn *conn)
{
    HttpAuth    *auth;

    auth = conn->rx->route->auth;
    if (httpIsAuthenticated(conn)) {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, auth->loggedInPage ? auth->loggedInPage : "~");
    } else {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, auth->loginPage);
    }
}


/*
    Logout service for use with httpSetAuthFormDetails.
 */
static void logoutServiceProc(HttpConn *conn)
{
    HttpRoute       *route;
    HttpAuth        *auth;
    cchar           *loggedOut;

    route = conn->rx->route;
    auth = route->auth;

    httpLogout(conn);

    loggedOut = (auth->loggedOutPage) ? auth->loggedOutPage : auth->loginPage;
    if (!loggedOut) {
        loggedOut = "/";
    }
    httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, loggedOut);
}


static HttpRoute *createLoginRoute(HttpRoute *route, cchar *pattern, HttpAction action)
{
    bool    secure;

    secure = 0;
    if (sstarts(pattern, "https:///")) {
        pattern = &pattern[8];
        secure = 1;
    } else if (sstarts(pattern, "http:///")) {
        pattern = &pattern[7];
    }
    if ((route = httpCreateInheritedRoute(route)) != 0) {
        httpSetRoutePattern(route, sjoin("^", pattern, "$", NULL), 0);
        if (secure) {
            httpAddRouteCondition(route, "secure", "https://", HTTP_ROUTE_REDIRECT);
        }
        if (action) {
            route->handler = route->http->actionHandler;
            httpDefineAction(pattern, action);
        }
        httpFinalizeRoute(route);
    }
    return route;
}


/*
    Define login URLs by creating routes. Used by Appweb AuthType directive.
    Web frameworks like ESP should NOT use this.
 */
PUBLIC void httpSetAuthFormDetails(HttpRoute *route, cchar *loginPage, cchar *loginService, cchar *logoutService,
    cchar *loggedInPage, cchar *loggedOutPage)
{
    HttpRoute   *loginRoute;
    HttpAuth    *auth;

    auth = route->auth;

    if (!route->cookie) {
        httpSetRouteCookie(route, HTTP_SESSION_COOKIE);
    }
    if (loggedInPage) {
        auth->loggedInPage = sclone(loggedInPage);
    }
    if (loginPage) {
        auth->loginPage = sclone(loginPage);
        createLoginRoute(route, auth->loginPage, 0);
    }
    if (loggedOutPage) {
        if (smatch(loginPage, loggedOutPage)) {
            auth->loggedOutPage = auth->loginPage;
        } else {
            auth->loggedOutPage = sclone(loggedOutPage);
            createLoginRoute(route, auth->loggedOutPage, 0);
        }
    }
    /*
        Put services last so they inherit the auth settings above
     */
    if (loginService) {
        loginRoute = createLoginRoute(route, loginService, loginServiceProc);
        httpAddRouteCondition(loginRoute, "auth", 0, 0);
    }
    if (logoutService) {
        createLoginRoute(route, logoutService, logoutServiceProc);
    }
}


/*
    Can supply a roles or abilities in the "abilities" parameter
 */
PUBLIC void httpSetAuthRequiredAbilities(HttpAuth *auth, cchar *abilities)
{
    char    *ability, *tok;

    GRADUATE_HASH(auth, abilities);
    for (ability = stok(sclone(abilities), " \t,", &tok); abilities; abilities = stok(NULL, " \t,", &tok)) {
        httpComputeRoleAbilities(auth, auth->abilities, ability);
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


PUBLIC void httpSetAuthQop(HttpAuth *auth, cchar *qop)
{
    auth->qop = sclone(qop);
}


PUBLIC void httpSetAuthRealm(HttpAuth *auth, cchar *realm)
{
    auth->realm = sclone(realm);
}


PUBLIC void httpSetAuthStoreSessions(HttpAuthStore *store, bool noSession)
{
    assert(store);
    store->noSession = noSession;
}


PUBLIC void httpSetAuthSession(HttpAuth *auth, bool enable)
{
    auth->flags &= ~HTTP_AUTH_NO_SESSION;
    if (!enable) {
        auth->flags |= HTTP_AUTH_NO_SESSION;
    }
}


PUBLIC int httpSetAuthStore(HttpAuth *auth, cchar *store)
{
    if ((auth->store = mprLookupKey(HTTP->authStores, store)) == 0) {
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


PUBLIC int httpSetAuthType(HttpAuth *auth, cchar *type, cchar *details)
{
    if (type == 0 || *type == '\0' || smatch(type, "none")) {
        auth->type = 0;
        return 0;
    }
    if ((auth->type = mprLookupKey(HTTP->authTypes, type)) == 0) {
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
    return mprLookupKey(HTTP->authTypes, type);
}


/*
    Verify the user password for the "config" store based on the users defined via configuration directives.
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
        httpTrace(conn->trace, "auth.login.error", "error", "msg: 'Unknown user', username:'%s'", username);
        return 0;
    }
    if (password) {
        if (auth->realm == 0 || *auth->realm == '\0') {
            mprLog("error http auth", 0, "No AuthRealm defined");
        }
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
            httpTrace(conn->trace, "auth.login.authenticated", "context", "msg:'User authenticated', username:'%s'", username);
        } else {
            httpTrace(conn->trace, "auth.login.error", "error", "msg:'Password failed to authenticate', username:'%s'", username);
        }
        return success;
    }
    return 1;
}


/*
    Web form-based authentication callback for the "form" auth protocol.
    Asks the user to login via a web page.
 */
static void formLogin(HttpConn *conn)
{
    if (conn->rx->route->auth && conn->rx->route->auth->loginPage) {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, conn->rx->route->auth->loginPage);
    } else {
        httpError(conn, HTTP_CODE_UNAUTHORIZED, "Access Denied. Login required");
    }
}


PUBLIC int formParse(HttpConn *conn, cchar **username, cchar **password)
{
    *username = httpGetParam(conn, "username", 0);
    *password = httpGetParam(conn, "password", 0);
    return 0;
}


#undef  GRADUATE_HASH

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
