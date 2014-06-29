/*
    user.c - User and Role management

    An internal cache of users is kept for authenticated users.
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

static void manageRole(HttpRole *role, int flags);
static void manageUser(HttpUser *user, int flags);

/*********************************** Code *************************************/

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


/*
    Compute a set of abilities for a role. Role strings can be either roles or abilities.
    The abilities hash is updated.
 */
PUBLIC void httpComputeRoleAbilities(HttpAuth *auth, MprHash *abilities, cchar *role)
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
        httpComputeRoleAbilities(auth, user->abilities, ability);
    }
}


/*
    Recompute all user abilities. Used if the role definitions change
 */
PUBLIC void httpComputeAllUserAbilities(HttpAuth *auth)
{
    MprKey      *kp;
    HttpUser    *user;

    for (ITERATE_KEY_DATA(auth->userCache, kp, user)) {
        httpComputeUserAbilities(auth, user);
    }
}


PUBLIC char *httpRolesToAbilities(HttpAuth *auth, cchar *roles, cchar *separator)
{
    MprKey      *ap;
    HttpRole    *rp;
    MprBuf      *buf;
    char        *role, *tok;

    buf = mprCreateBuf(0, 0);
    for (role = stok(sclone(roles), " \t,", &tok); role; role = stok(NULL, " \t,", &tok)) {
        if ((rp = mprLookupKey(auth->roles, role)) != 0) {
            /* Interpret as a role */
            for (ITERATE_KEYS(rp->abilities, ap)) {
                mprPutStringToBuf(buf, ap->key);
                mprPutStringToBuf(buf, separator);
            }
        } else {
            /* Not found as a role: Interpret role as an ability */
            mprPutStringToBuf(buf, role);
            mprPutStringToBuf(buf, separator);
        }
    }
    if (mprGetBufLength(buf) > 0) {
        mprAdjustBufEnd(buf, - slen(separator));
        mprAddNullToBuf(buf);
    }
    return mprBufToString(buf);
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
