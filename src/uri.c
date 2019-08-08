/*
    uri.c - URI manipulation routines
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static cchar *expandRouteName(HttpStream *stream, cchar *routeName);
static int getPort(HttpUri *uri);
static int getDefaultPort(cchar *scheme);
static void manageUri(HttpUri *uri, int flags);
static void trimPathToDirname(HttpUri *uri);
static char *actionRoute(HttpRoute *route, cchar *controller, cchar *action);

/************************************ Code ************************************/
/*
    Create and initialize a URI. This accepts full URIs with schemes (http:) and partial URLs
    Support IPv4 and [IPv6]. Supported forms:

        SCHEME://[::]:PORT/URI
        SCHEME://HOST:PORT/URI
        [::]:PORT/URI
        :PORT/URI
        HOST:PORT/URI
        PORT/URI
        /URI
        URI

        NOTE: HOST/URI is not supported and requires a scheme prefix. This is because it is ambiguous with a
        relative uri path.

    Missing fields are null or zero.
 */
PUBLIC HttpUri *httpCreateUri(cchar *uri, int flags)
{
    HttpUri     *up;
    char        *tok, *next;

    if ((up = mprAllocObj(HttpUri, manageUri)) == 0) {
        return 0;
    }
    tok = sclone(uri);

    /*
        [scheme://][hostname[:port]][/path[.ext]][#ref][?query]
        First trim query and then reference from the end
     */
    if ((next = schr(tok, '?')) != 0) {
        *next++ = '\0';
        up->query = sclone(next);
    }
    if ((next = schr(tok, '#')) != 0) {
        *next++ = '\0';
        up->reference = sclone(next);
    }

    /*
        [scheme://][hostname[:port]][/path]
     */
    if ((next = scontains(tok, "://")) != 0) {
        up->scheme = snclone(tok, (next - tok));
        if (smatch(up->scheme, "http")) {
            if (flags & HTTP_COMPLETE_URI) {
                up->port = 80;
            }
        } else if (smatch(up->scheme, "ws")) {
            if (flags & HTTP_COMPLETE_URI) {
                up->port = 80;
            }
            up->webSockets = 1;
        } else if (smatch(up->scheme, "https")) {
            if (flags & HTTP_COMPLETE_URI) {
                up->port = 443;
            }
            up->secure = 1;
        } else if (smatch(up->scheme, "wss")) {
            if (flags & HTTP_COMPLETE_URI) {
                up->port = 443;
            }
            up->secure = 1;
            up->webSockets = 1;
        }
        tok = &next[3];
    }

    /*
        [hostname[:port]][/path]
     */
    if (*tok == '[' && ((next = strchr(tok, ']')) != 0)) {
        /* IPv6  [::]:port/uri */
        up->host = snclone(&tok[1], (next - tok) - 1);
        tok = ++next;

    } else if (*tok && *tok != '/' && *tok != ':' && (up->scheme || strchr(tok, ':'))) {
        /*
            Supported forms:
                scheme://hostname
                hostname:port/
         */
        if ((next = spbrk(tok, ":/")) == 0) {
            next = &tok[slen(tok)];
        }
        up->host = snclone(tok, next - tok);
        tok = next;
    }
    assert(tok);

    /* [:port][/path] */
    if (*tok == ':') {
        up->port = atoi(++tok);
        if ((tok = schr(tok, '/')) == 0) {
            tok = "";
        }
        if (up->port == 4443 || up->port == 443) {
            up->secure = 1;
        }
    }
    assert(tok);

    /* [/path] */
    if (*tok) {
        up->path = sclone(tok);
        /* path[.ext[/extra]] */
        if ((tok = srchr(up->path, '.')) != 0) {
            if (tok[1]) {
                if ((next = srchr(up->path, '/')) != 0) {
                    if (next < tok) {
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
    up->secure = smatch(up->scheme, "https") || smatch(up->scheme, "wss");
    up->webSockets = smatch(up->scheme, "ws") || smatch(up->scheme, "wss");

    if (flags & HTTP_COMPLETE_URI) {
        if (!up->scheme) {
            up->scheme = sclone("http");
        }
        if (!up->host) {
            up->host = sclone("localhost");
        }
        if (!up->port) {
            up->port = up->secure ? 443 : 80;
        }
    }
    up->valid = httpValidUriChars(uri);
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
        up->valid = 0;
        return 0;
    }
    if (!httpValidUriChars(scheme) || !httpValidUriChars(host) || !httpValidUriChars(path) ||
        !httpValidUriChars(reference) || !httpValidUriChars(query)) {
        up->valid = 0;
        return up;
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
    if ((tok = srchr(up->path, '.')) != 0) {
        if ((cp = srchr(up->path, '/')) != 0) {
            if (cp <= tok) {
                up->ext = sclone(&tok[1]);
            }
        } else {
            up->ext = sclone(&tok[1]);
        }
    }
    up->valid = 1;
    return up;
}


PUBLIC HttpUri *httpCloneUri(HttpUri *base, int flags)
{
    HttpUri     *up;
    cchar       *path, *cp, *tok;

    if ((up = mprAllocObj(HttpUri, manageUri)) == 0) {
        up->valid = 0;
        return 0;
    }
    if (!base || !base->valid) {
        up->valid = 0;
        return up;
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
        up->port = up->secure ? 443 : 80;
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
    if (up->path && (tok = srchr(up->path, '.')) != 0) {
        if ((cp = srchr(up->path, '/')) != 0) {
            if (cp <= tok) {
                up->ext = sclone(&tok[1]);
            }
        } else {
            up->ext = sclone(&tok[1]);
        }
    }
    up->valid = 1;
    return up;
}


/*
    Complete the "uri" using missing parts from base
 */
PUBLIC HttpUri *httpCompleteUri(HttpUri *uri, HttpUri *base)
{
    if (!uri) {
        return 0;
    }
    if (base) {
        if (!uri->host) {
            uri->host = base->host;
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
    if (!uri->scheme) {
        uri->scheme = sclone("http");
    }
    if (!uri->host) {
        uri->host = sclone("localhost");
    }
    if (!uri->path) {
        uri->path = sclone("/");
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
    hostDelim = "";

    if (flags & HTTP_COMPLETE_URI) {
        if (scheme == 0 || *scheme == '\0') {
            scheme = "http";
        }
        if (host == 0 || *host == '\0') {
            if (port || path || reference || query) {
                host = "localhost";
            }
        }
    }
    if (scheme) {
        hostDelim = "://";
    }
    if (!host) {
        host = "";
    }
    if (mprIsIPv6(host)) {
        if (*host != '[') {
            host = sfmt("[%s]", host);
        } else if ((cp = scontains(host, "]:")) != 0) {
            port = 0;
        }
    } else if (schr(host, ':')) {
        port = 0;
    }
    if (port != 0 && port != getDefaultPort(scheme)) {
        portStr = itos(port);
        portDelim = ":";
    }
    if (scheme == 0) {
        scheme = "";
    }
    if (path && *path) {
        if (*host) {
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
    if (*portDelim) {
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
    cchar       *bp, *startDiff, *tp;
    char        *basePath, *cp, *path;
    int         i, baseSegments, commonSegments;

    if (base == 0) {
        return clone ? httpCloneUri(target, 0) : target;
    }
    if (target == 0) {
        return clone ? httpCloneUri(base, 0) : base;
    }
    if (!(target->path && target->path[0] == '/') || !((base->path && base->path[0] == '/'))) {
        /* If target is relative, just use it. If base is relative, cannot use it because we don't know where it is */
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
    if ((basePath = httpNormalizeUriPath(base->path)) == 0) {
        return 0;
    }
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

    uri->path = path = cp = mprAlloc(baseSegments * 3 + (int) slen(target->path) + 2);
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
        strcpy(path, ".");
    }
    return uri;
}


PUBLIC HttpUri *httpJoinUriPath(HttpUri *result, HttpUri *base, HttpUri *other)
{
    char    *sep;

    if (other->path) {
        if (other->path[0] == '/' || !base->path) {
            result->path = sclone(other->path);
        } else {
            sep = ((base->path[0] == '\0' || base->path[slen(base->path) - 1] == '/') ||
                   (other->path[0] == '\0' || other->path[0] == '/'))  ? "" : "/";
            result->path = sjoin(base->path, sep, other->path, NULL);
        }
    }
    return result;
}


PUBLIC HttpUri *httpJoinUri(HttpUri *uri, int argc, HttpUri **others)
{
    HttpUri     *other;
    int         i;

    if ((uri = httpCloneUri(uri, 0)) == 0) {
        return 0;
    }
    if (!uri->valid) {
        return 0;
    }
    for (i = 0; i < argc; i++) {
        other = others[i];
        if (other->scheme) {
            uri->scheme = sclone(other->scheme);
            uri->port = other->port;
        }
        if (other->host) {
            uri->host = sclone(other->host);
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


PUBLIC HttpUri *httpNormalizeUri(HttpUri *uri)
{
    if (!uri) {
        return 0;
    }
    uri->path = httpNormalizeUriPath(uri->path);
    return uri;
}


/*
    Normalize a URI path to remove redundant "./", "../" and make separators uniform.
    This will not permit leading '../' segments.
    Does not make an abs path, map separators or change case.
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
        return 0;
    }
    strcpy(dupPath, pathArg);

    if ((segments = mprAlloc(sizeof(char*) * (len + 1))) == 0) {
        return 0;
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
                /* .more-chars */
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


PUBLIC HttpUri *httpResolveUri(HttpStream *stream, HttpUri *base, HttpUri *other)
{
    HttpHost        *host;
    HttpEndpoint    *endpoint;
    HttpUri         *current;

    if (!base || !base->valid) {
        return other;
    }
    if (!other || !other->valid) {
        return base;
    }
    current = httpCloneUri(base, 0);

    /*
        Must not inherit the query or reference
     */
    current->query = 0;
    current->reference = 0;

    if (other->scheme && !smatch(current->scheme, other->scheme)) {
        current->scheme = sclone(other->scheme);
        /*
            If the scheme is changed (test above), then accept an explict port.
            If no port, then must not use the current port as the scheme has changed.
         */
        if (other->port) {
            current->port = other->port;
        } else {
            host = stream ? stream->host : httpGetDefaultHost();
            endpoint = smatch(current->scheme, "https") ? host->secureEndpoint : host->defaultEndpoint;
            if (endpoint) {
                current->port = endpoint->port;
            } else {
                current->port = 0;
            }
        }
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
        current->path = httpNormalizeUriPath(current->path);
    }
    if (other->reference) {
        current->reference = sclone(other->reference);
    }
    if (other->query) {
        current->query = sclone(other->query);
    }
    current->ext = mprGetPathExt(current->path);
    return current;
}


PUBLIC HttpUri *httpLinkUri(HttpStream *stream, cchar *target, MprHash *options)
{
    HttpRoute       *route, *lroute;
    HttpRx          *rx;
    HttpUri         *uri;
    cchar           *routeName, *action, *controller, *originalAction, *tplate;
    char            *rest;

    assert(stream);

    rx = stream->rx;
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
                controller = httpGetParam(stream, "controller", 0);
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
                routeName = expandRouteName(stream, routeName);
                lroute = httpLookupRoute(stream->host, routeName);
            } else {
                lroute = 0;
            }
            if (!lroute) {
                if ((lroute = httpLookupRoute(stream->host, actionRoute(route, controller, action))) == 0) {
                    if ((lroute = httpLookupRoute(stream->host, actionRoute(route, "{controller}", action))) == 0) {
                        if ((lroute = httpLookupRoute(stream->host, actionRoute(route, controller, "default"))) == 0) {
                            lroute = httpLookupRoute(stream->host, actionRoute(route, "{controller}", "default"));
                        }
                    }
                }
            }
            if (lroute) {
                tplate = lroute->tplate;
            }
        }
        if (!tplate) {
            mprLog("error http", 0, "Cannot find template for URI %s", target);
            target = "/";
        }
    }
    target = httpTemplate(stream, tplate, options);

    if ((uri = httpCreateUri(target, 0)) == 0) {
        return 0;
    }
    return uri;
}


PUBLIC char *httpLink(HttpStream *stream, cchar *target)
{
    return httpLinkEx(stream, target, 0);
}


PUBLIC char *httpLinkEx(HttpStream *stream, cchar *target, MprHash *options)
{
    return httpUriToString(httpLinkUri(stream, target, options), 0);
}


PUBLIC char *httpLinkAbs(HttpStream *stream, cchar *target)
{
    return httpUriToString(httpResolveUri(stream, stream->rx->parsedUri, httpLinkUri(stream, target, 0)), 0);
}


PUBLIC char *httpUriToString(HttpUri *uri, int flags)
{
    if (!uri) {
        return "";
    }
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

    if (uri == 0 || *uri != '/') {
        return 0;
    }
    if (!httpValidUriChars(uri)) {
        return 0;
    }
    up = mprUriDecode(uri);
    if ((up = httpNormalizeUriPath(up)) == 0) {
        return 0;
    }
    if (*up != '/' || strchr(up, '\\')) {
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

    if (uri == 0) {
        return 1;
    }
    pos = strspn(uri, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?#[]@!$&'()*+,;=%");
    if (pos < slen(uri)) {
        return 0;
    }
    return 1;
}


static int getPort(HttpUri *uri)
{
    if (!uri) {
        return 0;
    }
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
    char    *path, *cp;
    int     len;

    path = (char*) uri->path;
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
    Limited expansion of route names. Support ~ and ${app} at the start of the route name
 */
static cchar *expandRouteName(HttpStream *stream, cchar *routeName)
{
    if (routeName[0] == '~') {
        return sjoin(httpGetRouteTop(stream), &routeName[1], NULL);
    }
    if (sstarts(routeName, "${app}")) {
        return sjoin(httpGetRouteTop(stream), &routeName[6], NULL);
    }
    return routeName;
}


/*
    Calculate a qualified route name. The form is: /{app}/{controller}/action
 */
static char *actionRoute(HttpRoute *route, cchar *controller, cchar *action)
{
    cchar   *controllerPrefix;

    if (action == 0 || *action == '\0') {
        action = "default";
    }
    if (controller) {
        controllerPrefix = (controller && smatch(controller, "{controller}")) ? "*" : controller;
        return sjoin("^", route->prefix, "/", controllerPrefix, "/", action, NULL);
    } else {
        return sjoin("^", route->prefix, "/", action, NULL);
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
