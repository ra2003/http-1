/*
    uri.c - URI manipulation routines
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static cchar *expandRouteName(HttpConn *conn, cchar *routeName);
static int getPort(HttpUri *uri);
static int getDefaultPort(cchar *scheme);
static void manageUri(HttpUri *uri, int flags);
static void trimPathToDirname(HttpUri *uri);
static char *actionRoute(HttpRoute *route, cchar *controller, cchar *action);

/************************************ Code ************************************/
/*
    Create and initialize a URI. This accepts full URIs with schemes (http:) and partial URLs
    Support IPv4 and [IPv6]. Supported forms:

        scheme://[::]:PORT/URI
        scheme://HOST:PORT/URI
        [::]:PORT/URI
        :PORT/URI
        HOST:PORT/URI
        PORT/URI
        /URI
        URI

        NOTE: the following is not supported and requires a scheme prefix. This is because it is ambiguous with URI.
        HOST/URI

    Missing fields are null or zero.
 */
PUBLIC HttpUri *httpCreateUri(cchar *uri, int flags)
{
    HttpUri     *up;
    char        *tok, *next;

    assert(uri);

    if ((up = mprAllocObj(HttpUri, manageUri)) == 0) {
        return 0;
    }
    tok = up->uri = sclone(uri);

    if (sncmp(up->uri, "http://", 7) == 0) {
        up->scheme = sclone("http");
        if (flags & HTTP_COMPLETE_URI) {
            up->port = 80;
        }
        tok = &up->uri[7];

    } else if (sncmp(up->uri, "ws://", 5) == 0) {
        up->scheme = sclone("ws");
        if (flags & HTTP_COMPLETE_URI) {
            up->port = 80;
        }
        tok = &up->uri[5];
        up->webSockets = 1;

    } else if (sncmp(up->uri, "https://", 8) == 0) {
        up->scheme = sclone("https");
        up->secure = 1;
        if (flags & HTTP_COMPLETE_URI) {
            up->port = 443;
        }
        tok = &up->uri[8];

    } else if (sncmp(up->uri, "wss://", 6) == 0) {
        up->scheme = sclone("wss");
        up->secure = 1;
        if (flags & HTTP_COMPLETE_URI) {
            up->port = 443;
        }
        tok = &up->uri[6];
        up->webSockets = 1;

    } else {
        up->scheme = 0;
        tok = up->uri;
    }
    if (schr(tok, ':')) {
        /* Has port specifier */
        if (*tok == '[' && ((next = strchr(tok, ']')) != 0)) {
            /* IPv6  [::]:port/uri */
            up->host = snclone(&tok[1], (next - tok) - 1);
            if (*++next == ':') {
                up->port = atoi(++next);
            }
            tok = schr(next, '/');

        } else if ((next = spbrk(tok, ":/")) == NULL) {
            /* hostname */
            if (*tok) {
                up->host = sclone(tok);
            }
            tok = 0;

        } else if (*next == ':') {
            /* hostname:port */
            if (next > tok) {
                up->host = snclone(tok, next - tok);
            }
            up->port = atoi(++next);
            tok = schr(next, '/');

        } else if (*next == '/') {
            /* hostname/uri */
            if (next > tok) {
                up->host = snclone(tok, next - tok);
            }
            tok = next;
        }

    } else if (up->scheme && *tok != '/') {
        /* hostname/uri */
        if ((next = schr(tok, '/')) != 0) {
            if (next > tok) {
                up->host = snclone(tok, next - tok);
            }
            tok = next;
        } else {
            /* hostname */
            if (*tok) {
                up->host = sclone(tok);
            }
            tok = 0;
        }
    }
    if (tok) {
        if ((next = spbrk(tok, "#?")) == NULL) {
            if (*tok) {
                up->path = sclone(tok);
            }
        } else {
            if (next > tok) {
                up->path = snclone(tok, next - tok);
            }
            tok = next + 1;
            if (*next == '#') {
                if ((next = schr(tok, '?')) != NULL) {
                    up->reference = snclone(tok, next - tok);
                    up->query = sclone(++next);
                } else {
                    up->reference = sclone(tok);
                }
            } else {
                up->query = sclone(tok);
            }
        }
        if (up->path && (tok = srchr(up->path, '.')) != NULL) {
            if (tok[1]) {
                if ((next = srchr(up->path, '/')) != NULL) {
                    if (next <= tok) {
                        up->ext = sclone(++tok);
                    }
                } else {
                    up->ext = sclone(++tok);
                }
            }
        }
    }
    if (flags & (HTTP_COMPLETE_URI | HTTP_COMPLETE_URI_PATH)) {
        if (up->path == 0 || *up->path == '\0') {
            up->path = sclone("/");
        }
    }
    if (flags & HTTP_COMPLETE_URI) {
        if (!up->scheme) {
            up->scheme = sclone("http");
        }
        if (!up->host) {
            up->host = sclone("localhost");
        }
        if (!up->port) {
            up->port = 80;
        }
    }
    return up;
}


static void manageUri(HttpUri *uri, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(uri->scheme);
        mprMark(uri->host);
        mprMark(uri->path);
        mprMark(uri->ext);
        mprMark(uri->reference);
        mprMark(uri->query);
        mprMark(uri->uri);
    }
}


/*
    Create and initialize a URI. This accepts full URIs with schemes (http:) and partial URLs
 */
PUBLIC HttpUri *httpCreateUriFromParts(cchar *scheme, cchar *host, int port, cchar *path, cchar *reference, cchar *query, 
        int flags)
{
    HttpUri     *up;
    char        *cp, *tok;

    if ((up = mprAllocObj(HttpUri, manageUri)) == 0) {
        return 0;
    }
    if (scheme) {
        up->scheme = sclone(scheme);
        up->secure = (smatch(up->scheme, "https") || smatch(up->scheme, "wss"));
        up->webSockets = (smatch(up->scheme, "ws") || smatch(up->scheme, "wss"));
    } else if (flags & HTTP_COMPLETE_URI) {
        up->scheme = "http";
    }
    if (host) {
        if (*host == '[' && ((cp = strchr(host, ']')) != 0)) {
            up->host = snclone(&host[1], (cp - host) - 2);
            if ((cp = schr(++cp, ':')) && port == 0) {
                port = (int) stoi(++cp);
            }
        } else {
            up->host = sclone(host);
            if ((cp = schr(up->host, ':')) && port == 0) {
                port = (int) stoi(++cp);
            }
        }
    } else if (flags & HTTP_COMPLETE_URI) {
        up->host = sclone("localhost");
    }
    if (port) {
        up->port = port;
    }
    if (path) {
        while (path[0] == '/' && path[1] == '/') {
            path++;
        }
        up->path = sclone(path);
    }
    if (flags & (HTTP_COMPLETE_URI | HTTP_COMPLETE_URI_PATH)) {
        if (up->path == 0 || *up->path == '\0') {
            up->path = sclone("/");
        }
    }
    if (reference) {
        up->reference = sclone(reference);
    }
    if (query) {
        up->query = sclone(query);
    }
    if ((tok = srchr(up->path, '.')) != NULL) {
        if ((cp = srchr(up->path, '/')) != NULL) {
            if (cp <= tok) {
                up->ext = sclone(&tok[1]);
            }
        } else {
            up->ext = sclone(&tok[1]);
        }
    }
    return up;
}


PUBLIC HttpUri *httpCloneUri(HttpUri *base, int flags)
{
    HttpUri     *up;
    char        *path, *cp, *tok;

    if ((up = mprAllocObj(HttpUri, manageUri)) == 0) {
        return 0;
    }
    if (base->scheme) {
        up->scheme = sclone(base->scheme);
    } else if (flags & HTTP_COMPLETE_URI) {
        up->scheme = sclone("http");
    }
    up->secure = (smatch(up->scheme, "https") || smatch(up->scheme, "wss"));
    up->webSockets = (smatch(up->scheme, "ws") || smatch(up->scheme, "wss"));
    if (base->host) {
        up->host = sclone(base->host);
    } else if (flags & HTTP_COMPLETE_URI) {
        up->host = sclone("localhost");
    }
    if (base->port) {
        up->port = base->port;
    } else if (flags & HTTP_COMPLETE_URI) {
        up->port = (smatch(up->scheme, "https") || smatch(up->scheme, "wss"))? 443 : 80;
    }
    path = base->path;
    if (path) {
        while (path[0] == '/' && path[1] == '/') {
            path++;
        }
        up->path = sclone(path);
    }
    if (flags & (HTTP_COMPLETE_URI | HTTP_COMPLETE_URI_PATH)) {
        if (up->path == 0 || *up->path == '\0') {
            up->path = sclone("/");
        }
    }
    if (base->reference) {
        up->reference = sclone(base->reference);
    }
    if (base->query) {
        up->query = sclone(base->query);
    }
    if (up->path && (tok = srchr(up->path, '.')) != NULL) {
        if ((cp = srchr(up->path, '/')) != NULL) {
            if (cp <= tok) {
                up->ext = sclone(&tok[1]);
            }
        } else {
            up->ext = sclone(&tok[1]);
        }
    }
    return up;
}


/*
    Complete the "uri" using missing parts from base
 */
PUBLIC HttpUri *httpCompleteUri(HttpUri *uri, HttpUri *base)
{
    if (!base) {
        if (!uri->scheme) {
            uri->scheme = sclone("http");
        }
        if (!uri->host) {
            uri->host = sclone("localhost");
        }
        if (!uri->path) {
            uri->path = sclone("/");
        }
    } else {
        if (!uri->host) {
            uri->host = base->host;
            /* Must not add a port if there is already a host */
            if (!uri->port) {
                uri->port = base->port;
            }
        }
        if (!uri->scheme) {
            uri->scheme = base->scheme;
        }
        if (!uri->path) {
            uri->path = base->path;
            if (!uri->query) {
                uri->query = base->query;
            }
            if (!uri->reference) {
                uri->reference = base->reference;
            }
        }
    }
    uri->secure = (smatch(uri->scheme, "https") || smatch(uri->scheme, "wss"));
    uri->webSockets = (smatch(uri->scheme, "ws") || smatch(uri->scheme, "wss"));
    return uri;
}


/*
    Format a string URI from parts
 */
PUBLIC char *httpFormatUri(cchar *scheme, cchar *host, int port, cchar *path, cchar *reference, cchar *query, int flags)
{
    char    *uri;
    cchar   *portStr, *hostDelim, *portDelim, *pathDelim, *queryDelim, *referenceDelim, *cp;

    portDelim = "";
    portStr = "";

    if ((flags & HTTP_COMPLETE_URI) || host || scheme) {
        if (scheme == 0 || *scheme == '\0') {
            scheme = "http";
        }
        if (host == 0 || *host == '\0') {
            if (port || path || reference || query) {
                host = "localhost";
            }
        }
        hostDelim = "://";
    } else {
        host = hostDelim = "";
    }
    if (host) {
        if (mprIsIPv6(host)) {
            if (*host != '[') {
                host = sfmt("[%s]", host);
            } else if ((cp = scontains(host, "]:")) != 0) {
                port = 0;
            }
        } else if (schr(host, ':')) {
            port = 0;
        }
    }
    if (port != 0 && port != getDefaultPort(scheme)) {
        portStr = itos(port);
        portDelim = ":";
    }
    if (scheme == 0) {
        scheme = "";
    }
    if (path && *path) {
        if (*hostDelim) {
            pathDelim = (*path == '/') ? "" :  "/";
        } else {
            pathDelim = "";
        }
    } else {
        pathDelim = path = "";
    }
    if (reference && *reference) {
        referenceDelim = "#";
    } else {
        referenceDelim = reference = "";
    }
    if (query && *query) {
        queryDelim = "?";
    } else {
        queryDelim = query = "";
    }
    if (portDelim) {
        uri = sjoin(scheme, hostDelim, host, portDelim, portStr, pathDelim, path, referenceDelim, reference, 
            queryDelim, query, NULL);
    } else {
        uri = sjoin(scheme, hostDelim, host, pathDelim, path, referenceDelim, reference, queryDelim, query, NULL);
    }
    return uri;
}


/*
    This returns a URI relative to the base for the given target

    uri = target.relative(base)
 */
PUBLIC HttpUri *httpGetRelativeUri(HttpUri *base, HttpUri *target, int clone)
{
    HttpUri     *uri;
    char        *basePath, *bp, *cp, *tp, *startDiff;
    int         i, baseSegments, commonSegments;

    if (target == 0) {
        return (clone) ? httpCloneUri(base, 0) : base;
    }
    if (!(target->path && target->path[0] == '/') || !((base->path && base->path[0] == '/'))) {
        /* If target is relative, just use it. If base is relative, can't use it because we don't know where it is */
        return (clone) ? httpCloneUri(target, 0) : target;
    }
    if (base->scheme && target->scheme && scmp(base->scheme, target->scheme) != 0) {
        return (clone) ? httpCloneUri(target, 0) : target;
    }
    if (base->host && target->host && (base->host && scmp(base->host, target->host) != 0)) {
        return (clone) ? httpCloneUri(target, 0) : target;
    }
    if (getPort(base) != getPort(target)) {
        return (clone) ? httpCloneUri(target, 0) : target;
    }
    basePath = httpNormalizeUriPath(base->path);

    /* Count trailing "/" */
    for (baseSegments = 0, bp = basePath; *bp; bp++) {
        if (*bp == '/') {
            baseSegments++;
        }
    }

    /*
        Find portion of path that matches the base, if any.
     */
    commonSegments = 0;
    for (bp = base->path, tp = startDiff = target->path; *bp && *tp; bp++, tp++) {
        if (*bp == '/') {
            if (*tp == '/') {
                commonSegments++;
                startDiff = tp;
            }
        } else {
            if (*bp != *tp) {
                break;
            }
        }
    }
    if (*startDiff == '/') {
        startDiff++;
    }

    if ((uri = httpCloneUri(target, 0)) == 0) {
        return 0;
    }
    uri->host = 0;
    uri->scheme = 0;
    uri->port = 0;

    uri->path = cp = mprAlloc(baseSegments * 3 + (int) slen(target->path) + 2);
    for (i = commonSegments; i < baseSegments; i++) {
        *cp++ = '.';
        *cp++ = '.';
        *cp++ = '/';
    }
    if (*startDiff) {
        strcpy(cp, startDiff);
    } else if (cp > uri->path) {
        /*
            Cleanup trailing separators ("../" is the end of the new path)
         */
        cp[-1] = '\0';
    } else {
        strcpy(uri->path, ".");
    }
    return uri;
}


//  FUTURE - rethink API, makes chaining hard if result must be supplied
/*
    result = base.join(other)
 */
PUBLIC HttpUri *httpJoinUriPath(HttpUri *result, HttpUri *base, HttpUri *other)
{
    char    *sep;

    if (other->path[0] == '/') {
        result->path = sclone(other->path);
    } else {
        sep = ((base->path[0] == '\0' || base->path[slen(base->path) - 1] == '/') || 
               (other->path[0] == '\0' || other->path[0] == '/'))  ? "" : "/";
        result->path = sjoin(base->path, sep, other->path, NULL);
    }
    return result;
}


PUBLIC HttpUri *httpJoinUri(HttpUri *uri, int argc, HttpUri **others)
{
    HttpUri     *other;
    int         i;

    uri = httpCloneUri(uri, 0);

    for (i = 0; i < argc; i++) {
        other = others[i];
        if (other->scheme) {
            uri->scheme = sclone(other->scheme);
        }
        if (other->host) {
            uri->host = sclone(other->host);
        }
        if (other->port) {
            uri->port = other->port;
        }
        if (other->path) {
            httpJoinUriPath(uri, uri, other);
        }
        if (other->reference) {
            uri->reference = sclone(other->reference);
        }
        if (other->query) {
            uri->query = sclone(other->query);
        }
    }
    uri->ext = mprGetPathExt(uri->path);
    return uri;
}


#if DEPRECATE || 1
PUBLIC char *httpLink(HttpConn *conn, cchar *target, MprHash *options)
{
    return httpUriEx(conn, target, options);
}
#endif

/*
    Create and resolve a URI link given a set of options.
 */
PUBLIC HttpUri *httpMakeUriLocal(HttpUri *uri)
{
    if (uri) {
        uri->host = 0;
        uri->scheme = 0;
        uri->port = 0;
    }
    return uri;
}


PUBLIC void httpNormalizeUri(HttpUri *uri)
{
    uri->path = httpNormalizeUriPath(uri->path);
}


/*
    Normalize a URI path to remove redundant "./" and cleanup "../" and make separator uniform. Does not make an abs path.
    It does not map separators nor change case. 
 */
PUBLIC char *httpNormalizeUriPath(cchar *pathArg)
{
    char    *dupPath, *path, *sp, *dp, *mark, **segments;
    int     firstc, j, i, nseg, len;

    if (pathArg == 0 || *pathArg == '\0') {
        return mprEmptyString();
    }
    len = (int) slen(pathArg);
    if ((dupPath = mprAlloc(len + 2)) == 0) {
        return NULL;
    }
    strcpy(dupPath, pathArg);

    if ((segments = mprAlloc(sizeof(char*) * (len + 1))) == 0) {
        return NULL;
    }
    nseg = len = 0;
    firstc = *dupPath;
    for (mark = sp = dupPath; *sp; sp++) {
        if (*sp == '/') {
            *sp = '\0';
            while (sp[1] == '/') {
                sp++;
            }
            segments[nseg++] = mark;
            len += (int) (sp - mark);
            mark = sp + 1;
        }
    }
    segments[nseg++] = mark;
    len += (int) (sp - mark);
    for (j = i = 0; i < nseg; i++, j++) {
        sp = segments[i];
        if (sp[0] == '.') {
            if (sp[1] == '\0')  {
                if ((i+1) == nseg) {
                    /* Trim trailing "." */
                    segments[j] = "";
                } else {
                    /* Trim intermediate "." */
                    j--;
                }
            } else if (sp[1] == '.' && sp[2] == '\0')  {
                j = max(j - 2, -1);
                if ((i+1) == nseg) {
                    nseg--;
                }
            } else {
                /* ..more-chars */
                segments[j] = segments[i];
            }
        } else {
            segments[j] = segments[i];
        }
    }
    nseg = j;
    assert(nseg >= 0);
    if ((path = mprAlloc(len + nseg + 1)) != 0) {
        for (i = 0, dp = path; i < nseg; ) {
            strcpy(dp, segments[i]);
            len = (int) slen(segments[i]);
            dp += len;
            if (++i < nseg || (nseg == 1 && *segments[0] == '\0' && firstc == '/')) {
                *dp++ = '/';
            }
        }
        *dp = '\0';
    }
    return path;
}


PUBLIC HttpUri *httpResolveUri(HttpUri *base, int argc, HttpUri **others, bool local)
{
    HttpUri     *current, *other;
    int         i;

    if ((current = httpCloneUri(base, 0)) == 0) {
        return 0;
    }
    if (local) {
        current->host = 0;
        current->scheme = 0;
        current->port = 0;
    }
    /*
        Must not inherit the query or reference
     */
    current->query = NULL;
    current->reference = NULL;

    for (i = 0; i < argc; i++) {
        other = others[i];
        if (other->scheme) {
            current->scheme = sclone(other->scheme);
        }
        if (other->host) {
            current->host = sclone(other->host);
        }
        if (other->port) {
            current->port = other->port;
        }
        if (other->path) {
            trimPathToDirname(current);
            httpJoinUriPath(current, current, other);
        }
        if (other->reference) {
            current->reference = sclone(other->reference);
        }
        if (other->query) {
            current->query = sclone(other->query);
        }
    }
    current->ext = mprGetPathExt(current->path);
    return current;
}


PUBLIC char *httpUriEx(HttpConn *conn, cchar *target, MprHash *options)
{
    HttpRoute       *route, *lroute;
    HttpRx          *rx;
    HttpUri         *uri;
    cchar           *routeName, *action, *controller, *originalAction, *tplate;
    char            *rest;

    rx = conn->rx;
    route = rx->route;
    controller = 0;

    if (target == 0) {
        target = "";
    }
    if (*target == '@') {
        target = sjoin("{action: '", target, "'}", NULL);
    } 
    if (*target != '{') {
        tplate = target;
        if (!options) {
            options = route->vars;
        }
    } else  {
        if (options) {
            options = mprBlendHash(httpGetOptions(target), options);
        } else {
            options = httpGetOptions(target);
        }
        options = mprBlendHash(options, route->vars);

        /*
            Prep the action. Forms are:
                . @action               # Use the current controller
                . @controller/          # Use "index" as the action
                . @controller/action
         */
        if ((action = httpGetOption(options, "action", 0)) != 0) {
            originalAction = action;
            if (*action == '@') {
                action = &action[1];
            }
            if (strchr(action, '/')) {
                controller = stok((char*) action, "/", (char**) &action);
                action = stok((char*) action, "/", &rest);
            }
            if (controller) {
                httpSetOption(options, "controller", controller);
            } else {
                controller = httpGetParam(conn, "controller", 0);
            }
            if (action == 0 || *action == '\0') {
                action = "list";
            }
            if (action != originalAction) {
                httpSetOption(options, "action", action);
            }
        }
        /*
            Find the template to use. Strategy is this order:
                . options.template
                . options.route.template
                . options.action mapped to a route.template, via:
                . /app/STAR/action
                . /app/controller/action
                . /app/STAR/default
                . /app/controller/default
         */
        if ((tplate = httpGetOption(options, "template", 0)) == 0) {
            if ((routeName = httpGetOption(options, "route", 0)) != 0) {
                routeName = expandRouteName(conn, routeName);
                lroute = httpLookupRoute(conn->host, routeName);
            } else {
                lroute = 0;
            }
            if (!lroute) {
                if ((lroute = httpLookupRoute(conn->host, actionRoute(route, controller, action))) == 0) {
                    if ((lroute = httpLookupRoute(conn->host, actionRoute(route, "{controller}", action))) == 0) {
                        if ((lroute = httpLookupRoute(conn->host, actionRoute(route, controller, "default"))) == 0) {
                            lroute = httpLookupRoute(conn->host, actionRoute(route, "{controller}", "default"));
                        }
                    }
                }
            }
            if (lroute) {
                tplate = lroute->tplate;
            }
        }
        if (!tplate) {
            mprError("Cannot find template for URI %s", target);
            target = "/";
        }
    }
    //  OPT
    target = httpTemplate(conn, tplate, options);
    uri = httpCreateUri(target, 0);
    uri = httpResolveUri(httpCreateUri(rx->uri, 0), 1, &uri, 0);
    httpNormalizeUri(uri);
    return httpUriToString(uri, 0);
}


PUBLIC char *httpUri(HttpConn *conn, cchar *target)
{
    return httpUriEx(conn, target, 0);
}


PUBLIC char *httpUriToString(HttpUri *uri, int flags)
{
    return httpFormatUri(uri->scheme, uri->host, uri->port, uri->path, uri->reference, uri->query, flags);
}


/*
    Validate a URI path for use in a HTTP request line
    The URI must contain only valid characters and must being with "/" both before and after decoding.
    A decoded, normalized URI path is returned.
 */
PUBLIC char *httpValidateUriPath(cchar *uri)
{
    char    *up;

    if (*uri != '/') {
        return 0;
    }
    if (!httpValidUriChars(uri)) {
        return 0;
    }
    up = mprUriDecode(uri);
    up = httpNormalizeUriPath(up);
    if (*up != '/') {
        return 0;
    }
    return up;
}


/*
    This tests if the URI has only characters valid to use in a URI before decoding. i.e. It will permit %NN encodings.
 */
PUBLIC bool httpValidUriChars(cchar *uri)
{
    ssize   pos;

    if (uri == 0 || *uri == 0) {
        return 1;
    }
    pos = strspn(uri, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?#[]@!$&'()*+,;=%");
    if (pos < slen(uri)) {
        mprTrace(3, "Bad character in URI at %s", &uri[pos]);
        return 0;
    }
    return 1;
}


static int getPort(HttpUri *uri)
{
    if (uri->port) {
        return uri->port;
    }
    return (uri->scheme && (smatch(uri->scheme, "https") || smatch(uri->scheme, "wss"))) ? 443 : 80;
}


static int getDefaultPort(cchar *scheme)
{
    return (scheme && (smatch(scheme, "https") || smatch(scheme, "wss"))) ? 443 : 80;
}


static void trimPathToDirname(HttpUri *uri) 
{
    char        *path, *cp;
    int         len;

    path = uri->path;
    len = (int) slen(path);
    if (path[len - 1] == '/') {
        if (len > 1) {
            path[len - 1] = '\0';
        }
    } else {
        if ((cp = srchr(path, '/')) != 0) {
            if (cp > path) {
                *cp = '\0';
            } else {
                cp[1] = '\0';
            }
        } else if (*path) {
            path[0] = '\0';
        }
    }
}


/*
    Limited expansion of route names. Support ~/, |/ and ${app} at the start of the route name
 */
static cchar *expandRouteName(HttpConn *conn, cchar *routeName)
{
    HttpRoute   *route;

    route = conn->rx->route;
    if (routeName[0] == '~') {
        return sjoin(route->prefix, &routeName[1], NULL);
    }
    if (sstarts(routeName, "${app}")) {
        return sjoin(route->prefix, &routeName[6], NULL);
    }
    if (routeName[0] == BIT_SERVER_PREFIX_CHAR) {
        if (route->serverPrefix) {
            return sjoin(route->prefix, "/", route->serverPrefix, &routeName[1], NULL);
        }
        return sjoin(route->prefix, &routeName[1], NULL);
    }
    return routeName;
}


/*
    Calculate a qualified route name. The form is: /{app}/{controller}/action
 */
static char *actionRoute(HttpRoute *route, cchar *controller, cchar *action)
{
    cchar   *prefix, *controllerPrefix;

    prefix = route->prefix ? route->prefix : "";
    if (action == 0 || *action == '\0') {
        action = "default";
    }
    if (controller) {
        controllerPrefix = (controller && smatch(controller, "{controller}")) ? "*" : controller;
        return sjoin(prefix, "/", controllerPrefix, "/", action, NULL);
    } else {
        return sjoin(prefix, "/", action, NULL);
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2013. All Rights Reserved.

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
