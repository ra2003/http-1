/*
    authPam.c - Authorization using PAM (Pluggable Authorization Module)

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
 #include    <security/pam_appl.h>

/********************************* Defines ************************************/

typedef struct {
    char    *name;
    char    *password;
} UserInfo;

#if MACOSX
    typedef int Gid;
#else
    typedef gid_t Gid;
#endif

/********************************* Forwards ***********************************/

static int pamChat(int msgCount, const struct pam_message **msg, struct pam_response **resp, void *data);

/*********************************** Code *************************************/
/*
    Use PAM to verify a user. The password may be NULL if using auto-login.
 */
PUBLIC bool httpPamVerifyUser(HttpStream *stream, cchar *username, cchar *password)
{
    MprBuf              *abilities;
    pam_handle_t        *pamh;
    UserInfo            info;
    struct pam_conv     conv = { pamChat, &info };
    struct group        *gp;
    int                 res, i;

    assert(username);
    assert(!stream->encoded);

    info.name = (char*) username;

    if (password) {
        info.password = (char*) password;
        pamh = NULL;
        if ((res = pam_start("login", info.name, &conv, &pamh)) != PAM_SUCCESS) {
            return 0;
        }
        if ((res = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK)) != PAM_SUCCESS) {
            pam_end(pamh, PAM_SUCCESS);
            mprDebug("http pam", 5, "httpPamVerifyUser failed to verify %s", username);
            return 0;
        }
        pam_end(pamh, PAM_SUCCESS);
    }
    mprDebug("http pam", 5, "httpPamVerifyUser verified %s", username);

    if (!stream->user) {
        stream->user = mprLookupKey(stream->rx->route->auth->userCache, username);
    }
    if (!stream->user) {
        /*
            Create a temporary user with a abilities set to the groups
         */
        Gid     groups[32];
        int     ngroups;
        ngroups = sizeof(groups) / sizeof(Gid);
        if ((i = getgrouplist(username, 99999, groups, &ngroups)) >= 0) {
            abilities = mprCreateBuf(0, 0);
            for (i = 0; i < ngroups; i++) {
                if ((gp = getgrgid(groups[i])) != 0) {
                    mprPutToBuf(abilities, "%s ", gp->gr_name);
                }
            }
#if ME_DEBUG
            mprAddNullToBuf(abilities);
            mprDebug("http pam", 5, "Create temp user \"%s\" with abilities: %s", username, mprGetBufStart(abilities));
#endif
            /*
                Create a user and map groups to roles and expand to abilities
             */
            stream->user = httpAddUser(stream->rx->route->auth, username, 0, mprGetBufStart(abilities));
        }
    }
    return 1;
}

/*
    Callback invoked by the pam_authenticate function
 */
static int pamChat(int msgCount, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    UserInfo                *info;
    struct pam_response     *reply;
    int                     i;

    i = 0;
    reply = 0;
    info = (UserInfo*) data;

    if (resp == 0 || msg == 0 || info == 0) {
        return PAM_CONV_ERR;
    }
    if ((reply = calloc(msgCount, sizeof(struct pam_response))) == 0) {
        return PAM_CONV_ERR;
    }
    for (i = 0; i < msgCount; i++) {
        reply[i].resp_retcode = 0;
        reply[i].resp = 0;

        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_ON:
            reply[i].resp = strdup(info->name);
            break;

        case PAM_PROMPT_ECHO_OFF:
            /* Retrieve the user password and pass onto pam */
            reply[i].resp = strdup(info->password);
            break;

        default:
            free(reply);
            return PAM_CONV_ERR;
        }
    }
    *resp = reply;
    return PAM_SUCCESS;
}
#endif /* ME_COMPILER_HAS_PAM */

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
