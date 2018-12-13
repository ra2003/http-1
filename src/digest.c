/*
    digest.c - Digest Authorization

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Locals ************************************/
/*
    Per-request digest authorization data
    @see HttpAuth
    @ingroup HttpAuth
    @stability Evolving
 */
typedef struct HttpDigest {
    char    *algorithm;
    char    *cnonce;
    char    *domain;
    char    *nc;
    char    *nonce;
    char    *opaque;
    char    *qop;
    char    *realm;
    char    *stale;
    char    *uri;
} HttpDigest;


/********************************** Forwards **********************************/

static char *calcDigest(HttpStream *stream, HttpDigest *dp, cchar *username);
static char *createDigestNonce(HttpStream *stream, cchar *secret, cchar *realm);
static void manageDigestData(HttpDigest *dp, int flags);
static int parseDigestNonce(char *nonce, cchar **secret, cchar **realm, MprTime *when);

/*********************************** Code *************************************/
/*
    Parse the 'Authorization' header and the server 'Www-Authenticate' header
 */
PUBLIC int httpDigestParse(HttpStream *stream, cchar **username, cchar **password)
{
    HttpRx      *rx;
    HttpDigest  *dp;
    MprTime     when;
    char        *value, *tok, *key, *cp, *sp;
    cchar       *secret, *realm;
    int         seenComma;

    rx = stream->rx;
    if (password) {
        *password = NULL;
    }
    if (username) {
        *username = NULL;
    }
    if (!rx->authDetails) {
        return 0;
    }
    dp = stream->authData = mprAllocObj(HttpDigest, manageDigestData);
    key = sclone(rx->authDetails);

    while (*key) {
        while (*key && isspace((uchar) *key)) {
            key++;
        }
        tok = key;
        while (*tok && !isspace((uchar) *tok) && *tok != ',' && *tok != '=') {
            tok++;
        }
        if (*tok) {
            *tok++ = '\0';
        }
        while (isspace((uchar) *tok)) {
            tok++;
        }
        seenComma = 0;
        if (*tok == '\"') {
            value = ++tok;
            while (*tok && *tok != '\"') {
                tok++;
            }
        } else {
            value = tok;
            while (*tok && *tok != ',') {
                tok++;
            }
            seenComma++;
        }
        if (*tok) {
            *tok++ = '\0';
        }

        /*
            Handle back-quoting
         */
        if (strchr(value, '\\')) {
            for (cp = sp = value; *sp; sp++) {
                if (*sp == '\\') {
                    sp++;
                }
                *cp++ = *sp++;
            }
            *cp = '\0';
        }

        /*
            user, response, oqaque, uri, realm, nonce, nc, cnonce, qop
         */
        switch (tolower((uchar) *key)) {
        case 'a':
            if (scaselesscmp(key, "algorithm") == 0) {
                dp->algorithm = sclone(value);
                break;
            } else if (scaselesscmp(key, "auth-param") == 0) {
                break;
            }
            break;

        case 'c':
            if (scaselesscmp(key, "cnonce") == 0) {
                dp->cnonce = sclone(value);
            }
            break;

        case 'd':
            if (scaselesscmp(key, "domain") == 0) {
                dp->domain = sclone(value);
                break;
            }
            break;

        case 'n':
            if (scaselesscmp(key, "nc") == 0) {
                dp->nc = sclone(value);
            } else if (scaselesscmp(key, "nonce") == 0) {
                dp->nonce = sclone(value);
            }
            break;

        case 'o':
            if (scaselesscmp(key, "opaque") == 0) {
                dp->opaque = sclone(value);
            }
            break;

        case 'q':
            if (scaselesscmp(key, "qop") == 0) {
                dp->qop = sclone(value);
            }
            break;

        case 'r':
            if (scaselesscmp(key, "realm") == 0) {
                dp->realm = sclone(value);
            } else if (scaselesscmp(key, "response") == 0) {
                /* Store the response digest in the password field. This is MD5(user:realm:password) */
                if (password) {
                    *password = sclone(value);
                }
                stream->encoded = 1;
            }
            break;

        case 's':
            if (scaselesscmp(key, "stale") == 0) {
                break;
            }

        case 'u':
            if (scaselesscmp(key, "uri") == 0) {
                dp->uri = sclone(value);
            } else if (scaselesscmp(key, "username") == 0 || scaselesscmp(key, "user") == 0) {
                if (username) {
                    *username = sclone(value);
                }
            }
            break;

        default:
            /*  Just ignore keywords we don't understand */
            ;
        }
        key = tok;
        if (!seenComma) {
            while (*key && *key != ',') {
                key++;
            }
            if (*key) {
                key++;
            }
        }
    }
    if (username && *username == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    if (password && *password == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    if (dp->realm == 0 || dp->nonce == 0 || dp->uri == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    if (dp->qop && (dp->cnonce == 0 || dp->nc == 0)) {
        return MPR_ERR_BAD_FORMAT;
    }
    if (httpServerStream(stream)) {
        realm = secret = 0;
        when = 0;
        parseDigestNonce(dp->nonce, &secret, &realm, &when);
        if (!smatch(stream->http->secret, secret)) {
            httpTrace(stream->trace, "auth.digest.error", "error", "msg:'Access denied, Nonce mismatch'");
            return MPR_ERR_BAD_STATE;

        } else if (!smatch(realm, rx->route->auth->realm)) {
            httpTrace(stream->trace, "auth.digest.error", "error", "msg:'Access denied, Realm mismatch'");
            return MPR_ERR_BAD_STATE;

        } else if (dp->qop && !smatch(dp->qop, "auth")) {
            httpTrace(stream->trace, "auth.digest.error", "error", "msg:'Access denied, Bad qop'");
            return MPR_ERR_BAD_STATE;

        } else if ((when + (5 * 60)) < time(0)) {
            httpTrace(stream->trace, "auth.digest.error", "error", "msg:'Access denied, Nonce is stale'");
            return MPR_ERR_BAD_STATE;
        }
        rx->passwordDigest = calcDigest(stream, dp, *username);
    } else {
        if (dp->domain == 0 || dp->opaque == 0 || dp->algorithm == 0 || dp->stale == 0) {
            return MPR_ERR_BAD_FORMAT;
        }
    }
    return 0;
}


static void manageDigestData(HttpDigest *dp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(dp->algorithm);
        mprMark(dp->cnonce);
        mprMark(dp->domain);
        mprMark(dp->nc);
        mprMark(dp->nonce);
        mprMark(dp->opaque);
        mprMark(dp->qop);
        mprMark(dp->realm);
        mprMark(dp->stale);
        mprMark(dp->uri);
    }
}


/*
    Respond to the request by asking for a login
    Only called if not logged in.
 */
PUBLIC void httpDigestLogin(HttpStream *stream)
{
    HttpAuth    *auth;
    char        *nonce, *opaque;

    auth = stream->rx->route->auth;

    if (auth->loginPage && !sends(stream->rx->referrer, auth->loginPage)) {
        httpRedirect(stream, HTTP_CODE_MOVED_TEMPORARILY, auth->loginPage);
    } else {
        nonce = createDigestNonce(stream, stream->http->secret, auth->realm);
        /* Opaque is unused, set to anything */
        opaque = "799d5";

        if (smatch(auth->qop, "none")) {
            httpSetHeader(stream, "WWW-Authenticate", "Digest realm=\"%s\", nonce=\"%s\"", auth->realm, nonce);
        } else {
            /* qop value of null defaults to "auth" */
            httpSetHeader(stream, "WWW-Authenticate", "Digest realm=\"%s\", domain=\"%s\", "
                "qop=\"auth\", nonce=\"%s\", opaque=\"%s\", algorithm=\"MD5\", stale=\"FALSE\"",
                auth->realm, "/", nonce, opaque);
        }
        httpSetContentType(stream, "text/plain");
        httpError(stream, HTTP_CODE_UNAUTHORIZED, "Access Denied. Login required");
    }
}


/*
    Add the 'Authorization' header for authenticated requests
    Must first get a 401 response to get the authData.
 */
PUBLIC bool httpDigestSetHeaders(HttpStream *stream, cchar *username, cchar *password)
{
    Http        *http;
    HttpTx      *tx;
    HttpDigest  *dp;
    char        *ha1, *ha2, *digest, *cnonce;

    http = stream->http;
    tx = stream->tx;
    if ((dp = stream->authData) == 0) {
        /* Need to await a failing auth response */
        return 0;
    }
    cnonce = sfmt("%s:%s:%x", http->secret, dp->realm, (int) http->now);
    ha1 = mprGetMD5(sfmt("%s:%s:%s", username, dp->realm, password));
    ha2 = mprGetMD5(sfmt("%s:%s", tx->method, tx->parsedUri->path));
    if (smatch(dp->qop, "auth")) {
        digest = mprGetMD5(sfmt("%s:%s:%s:%s:%s:%s", ha1, dp->nonce, dp->nc, cnonce, dp->qop, ha2));
        httpAddHeader(stream, "Authorization", "Digest username=\"%s\", realm=\"%s\", domain=\"%s\", "
            "algorithm=\"MD5\", qop=\"%s\", cnonce=\"%s\", nc=\"%s\", nonce=\"%s\", opaque=\"%s\", "
            "stale=\"FALSE\", uri=\"%s\", response=\"%s\"", username, dp->realm, dp->domain, dp->qop,
            cnonce, dp->nc, dp->nonce, dp->opaque, tx->parsedUri->path, digest);
    } else {
        digest = mprGetMD5(sfmt("%s:%s:%s", ha1, dp->nonce, ha2));
        httpAddHeader(stream, "Authorization", "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
            "uri=\"%s\", response=\"%s\"", username, dp->realm, dp->nonce, tx->parsedUri->path, digest);
    }
    return 1;
}


/*
    Create a nonce value for digest authentication (RFC 2617)
 */
static char *createDigestNonce(HttpStream *stream, cchar *secret, cchar *realm)
{
    static int64 next = 0;

    assert(realm && *realm);
    return mprEncode64(sfmt("%s:%s:%llx:%llx", secret, realm, mprGetTime(), next++));
}


static int parseDigestNonce(char *nonce, cchar **secret, cchar **realm, MprTime *when)
{
    char    *tok, *decoded, *whenStr;

    if ((decoded = mprDecode64(nonce)) == 0) {
        return MPR_ERR_CANT_READ;
    }
    *secret = stok(decoded, ":", &tok);
    *realm = stok(NULL, ":", &tok);
    whenStr = stok(NULL, ":", &tok);
    *when = (MprTime) stoiradix(whenStr, 16, NULL);
    return 0;
}


/*
    Get a password digest using the MD5 algorithm -- See RFC 2617 to understand this code.
 */
static char *calcDigest(HttpStream *stream, HttpDigest *dp, cchar *username)
{
    HttpAuth    *auth;
    char        *digestBuf, *ha1, *ha2;

    auth = stream->rx->route->auth;
    if (!stream->user) {
        stream->user = mprLookupKey(auth->userCache, username);
    }
    assert(stream->user && stream->user->password);
    if (stream->user == 0 || stream->user->password == 0) {
        return 0;
    }

    /*
        Compute HA1. Password is already expected to be in the HA1 format MD5(username:realm:password).
     */
    ha1 = sclone(stream->user->password);

    /*
        HA2
     */
#if PROTOTYPE || 1
    if (stream->rx->route->flags & HTTP_ROUTE_DOTNET_DIGEST_FIX) {
        char *uri = stok(sclone(dp->uri), "?", 0);
        ha2 = mprGetMD5(sfmt("%s:%s", stream->rx->method, uri));
    } else {
        ha2 = mprGetMD5(sfmt("%s:%s", stream->rx->method, dp->uri));
    }
#else
    ha2 = mprGetMD5(sfmt("%s:%s", stream->rx->method, dp->uri));
#endif

    /*
        H(HA1:nonce:HA2)
     */
    if (scmp(dp->qop, "auth") == 0) {
        digestBuf = sfmt("%s:%s:%s:%s:%s:%s", ha1, dp->nonce, dp->nc, dp->cnonce, dp->qop, ha2);
    } else {
        digestBuf = sfmt("%s:%s:%s", ha1, dp->nonce, ha2);
    }
    return mprGetMD5(digestBuf);
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
