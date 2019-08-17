/*
    rx.c -- Http receiver. Parses http requests and client responses.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void manageRx(HttpRx *rx, int flags);

/*********************************** Code *************************************/

PUBLIC HttpRx *httpCreateRx(HttpStream *stream)
{
    HttpRx      *rx;
    int         peer;

    if ((rx = mprAllocObj(HttpRx, manageRx)) == 0) {
        return 0;
    }
    rx->stream = stream;
    rx->length = -1;
    rx->ifMatch = 1;
    rx->ifModified = 1;
    rx->pathInfo = sclone("/");
    rx->scriptName = mprEmptyString();
    rx->needInputPipeline = httpClientStream(stream);
    rx->headers = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
    rx->chunkState = HTTP_CHUNK_UNCHUNKED;
    rx->remainingContent = 0;

    rx->seqno = ++stream->net->totalRequests;
    peer = stream->net->address ? stream->net->address->seqno : 0;
    rx->traceId = sfmt("%d-0-%lld-%d", peer, stream->net->seqno, rx->seqno);
    return rx;
}


static void manageRx(HttpRx *rx, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(rx->accept);
        mprMark(rx->acceptCharset);
        mprMark(rx->acceptEncoding);
        mprMark(rx->acceptLanguage);
        mprMark(rx->authDetails);
        mprMark(rx->authType);
        mprMark(rx->stream);
        mprMark(rx->connection);
        mprMark(rx->contentLength);
        mprMark(rx->cookie);
        mprMark(rx->etags);
        mprMark(rx->extraPath);
        mprMark(rx->files);
        mprMark(rx->headerPacket);
        mprMark(rx->headers);
        mprMark(rx->hostHeader);
        mprMark(rx->inputPipeline);
        mprMark(rx->inputRange);
        mprMark(rx->lang);
        mprMark(rx->method);
        mprMark(rx->mimeType);
        mprMark(rx->origin);
        mprMark(rx->originalMethod);
        mprMark(rx->originalUri);
        mprMark(rx->paramString);
        mprMark(rx->params);
        mprMark(rx->parsedUri);
        mprMark(rx->passwordDigest);
        mprMark(rx->pathInfo);
        mprMark(rx->pragma);
        mprMark(rx->protocol);
        mprMark(rx->redirect);
        mprMark(rx->referrer);
        mprMark(rx->requestData);
        mprMark(rx->route);
        mprMark(rx->scriptName);
        mprMark(rx->securityToken);
        mprMark(rx->session);
        mprMark(rx->statusMessage);
        mprMark(rx->svars);
        mprMark(rx->target);
        mprMark(rx->traceId);
        mprMark(rx->upgrade);
        mprMark(rx->uri);
        mprMark(rx->userAgent);
        mprMark(rx->webSocket);
    }
}


PUBLIC void httpDestroyRx(HttpRx *rx)
{
    if (rx->stream) {
        rx->stream->rx = 0;
        rx->stream = 0;
    }
}


/*
    Set the global request callback
 */
PUBLIC void httpSetRequestCallback(HttpRequestCallback callback)
{
    if (HTTP) {
        HTTP->requestCallback = callback;
    }
}


PUBLIC void httpCloseRx(HttpStream *stream)
{
    if (stream->rx && !stream->rx->remainingContent) {
        /* May not have consumed all read data, so cannot be assured the next request will be okay */
        stream->keepAliveCount = 0;
    }
    if (httpClientStream(stream)) {
        httpEnableNetEvents(stream->net);
    }
}


PUBLIC bool httpContentNotModified(HttpStream *stream)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprTime     modified;
    bool        same;

    rx = stream->rx;
    tx = stream->tx;

    if (rx->flags & HTTP_IF_MODIFIED) {
        /*
            If both checks, the last modification time and etag, claim that the request doesn't need to be
            performed, skip the transfer.
         */
        assert(tx->fileInfo.valid);
        modified = (MprTime) tx->fileInfo.mtime * TPS;
        same = httpMatchModified(stream, modified) && httpMatchEtag(stream, tx->etag);
        if (tx->outputRanges && !same) {
            tx->outputRanges = 0;
        }
        return same;
    }
    return 0;
}


PUBLIC MprOff httpGetContentLength(HttpStream *stream)
{
    if (stream->rx == 0) {
        assert(stream->rx);
        return 0;
    }
    return stream->rx->length;
}


PUBLIC cchar *httpGetCookies(HttpStream *stream)
{
    if (stream->rx == 0) {
        assert(stream->rx);
        return 0;
    }
    return stream->rx->cookie;
}


/*
    Extract a cookie.
    The rx->cookies contains a list of header cookies. A site may submit multiple cookies separated by ";"
 */
PUBLIC cchar *httpGetCookie(HttpStream *stream, cchar *name)
{
    HttpRx  *rx;
    cchar   *cookie;
    char    *cp, *value;
    ssize   nlen;
    int     quoted;

    assert(stream);
    rx = stream->rx;
    assert(rx);

    if ((cookie = rx->cookie) == 0 || name == 0 || *name == '\0') {
        return 0;
    }
    nlen = slen(name);
    while ((value = strstr(cookie, name)) != 0) {
        /* Ignore corrupt cookies of the form "name=;" */
        if ((value == rx->cookie || value[-1] == ' ' || value[-1] == ';') && value[nlen] == '=' && value[nlen+1] != ';') {
            break;
        }
        cookie += (value - cookie) + nlen;

    }
    if (value == 0) {
        return 0;
    }
    value += nlen;
    while (isspace((uchar) *value) || *value == '=') {
        value++;
    }
    quoted = 0;
    if (*value == '"') {
        value++;
        quoted++;
    }
    for (cp = value; *cp; cp++) {
        if (quoted) {
            if (*cp == '"' && cp[-1] != '\\') {
                break;
            }
        } else {
            if ((*cp == ',' || *cp == ';') && cp[-1] != '\\') {
                break;
            }
        }
    }
    return snclone(value, cp - value);
}


PUBLIC cchar *httpGetHeader(HttpStream *stream, cchar *key)
{
    if (stream->rx == 0) {
        assert(stream->rx);
        return 0;
    }
    return mprLookupKey(stream->rx->headers, slower(key));
}


PUBLIC char *httpGetHeadersFromHash(MprHash *hash)
{
    MprKey      *kp;
    char        *headers, *cp;
    ssize       len;

    for (len = 0, kp = 0; (kp = mprGetNextKey(hash, kp)) != 0; ) {
        len += strlen(kp->key) + 2 + strlen(kp->data) + 1;
    }
    if ((headers = mprAlloc(len + 1)) == 0) {
        return 0;
    }
    for (kp = 0, cp = headers; (kp = mprGetNextKey(hash, kp)) != 0; ) {
        strcpy(cp, kp->key);
        cp += strlen(cp);
        *cp++ = ':';
        *cp++ = ' ';
        strcpy(cp, kp->data);
        cp += strlen(cp);
        *cp++ = '\n';
    }
    *cp = '\0';
    return headers;
}


PUBLIC char *httpGetHeaders(HttpStream *stream)
{
    return httpGetHeadersFromHash(stream->rx->headers);
}


PUBLIC MprHash *httpGetHeaderHash(HttpStream *stream)
{
    if (stream->rx == 0) {
        assert(stream->rx);
        return 0;
    }
    return stream->rx->headers;
}


PUBLIC cchar *httpGetQueryString(HttpStream *stream)
{
    return (stream->rx && stream->rx->parsedUri) ? stream->rx->parsedUri->query : 0;
}


PUBLIC int httpGetStatus(HttpStream *stream)
{
    return (stream->rx) ? stream->rx->status : 0;
}


PUBLIC cchar *httpGetStatusMessage(HttpStream *stream)
{
    return (stream->rx) ? stream->rx->statusMessage : 0;
}


PUBLIC int httpSetUri(HttpStream *stream, cchar *uri)
{
    HttpRx      *rx;
    HttpUri     *parsedUri;
    char        *pathInfo;

    rx = stream->rx;
    if ((parsedUri = httpCreateUri(uri, 0)) == 0 || !parsedUri->valid) {
        return MPR_ERR_BAD_ARGS;
    }
    if (parsedUri->host && !rx->hostHeader) {
        rx->hostHeader = parsedUri->host;
    }
    if ((pathInfo = httpValidateUriPath(parsedUri->path)) == 0) {
        return MPR_ERR_BAD_ARGS;
    }
    rx->pathInfo = pathInfo;
    rx->uri = parsedUri->path;
    stream->tx->ext = httpGetExt(stream);

    /*
        Start out with no scriptName and the entire URI in the pathInfo. Stages may rewrite.
     */
    rx->scriptName = mprEmptyString();
    rx->parsedUri = parsedUri;
    return 0;
}


PUBLIC bool httpIsEof(HttpStream *stream)
{
    return stream->rx == 0 || stream->rx->eof;
}


/*
    Match the entity's etag with the client's provided etag.
 */
PUBLIC bool httpMatchEtag(HttpStream *stream, char *requestedEtag)
{
    HttpRx  *rx;
    char    *tag;
    int     next;

    rx = stream->rx;
    if (rx->etags == 0) {
        return 1;
    }
    if (requestedEtag == 0) {
        return 0;
    }
    for (next = 0; (tag = mprGetNextItem(rx->etags, &next)) != 0; ) {
        if (strcmp(tag, requestedEtag) == 0) {
            return (rx->ifMatch) ? 0 : 1;
        }
    }
    return (rx->ifMatch) ? 1 : 0;
}


/*
    If an IF-MODIFIED-SINCE was specified, then return true if the resource has not been modified. If using
    IF-UNMODIFIED, then return true if the resource was modified.
 */
PUBLIC bool httpMatchModified(HttpStream *stream, MprTime time)
{
    HttpRx   *rx;

    rx = stream->rx;

    if (rx->since == 0) {
        /*  If-Modified or UnModified not supplied. */
        return 1;
    }
    if (rx->ifModified) {
        /*  Return true if the file has not been modified.  */
        return !(time > rx->since);
    } else {
        /*  Return true if the file has been modified.  */
        return (time > rx->since);
    }
}


PUBLIC void httpSetEof(HttpStream *stream)
{
    if (stream) {
        stream->rx->eof = 1;
    }
}


PUBLIC void httpSetStageData(HttpStream *stream, cchar *key, cvoid *data)
{
    HttpRx      *rx;

    rx = stream->rx;
    if (rx->requestData == 0) {
        rx->requestData = mprCreateHash(-1, 0);
    }
    mprAddKey(rx->requestData, key, data);
}


PUBLIC cvoid *httpGetStageData(HttpStream *stream, cchar *key)
{
    HttpRx      *rx;

    rx = stream->rx;
    if (rx->requestData == 0) {
        return NULL;
    }
    return mprLookupKey(rx->requestData, key);
}


PUBLIC char *httpGetPathExt(cchar *path)
{
    char    *ep, *ext;

    if ((ext = strrchr(path, '.')) != 0) {
        ext = sclone(++ext);
        for (ep = ext; *ep && isalnum((uchar) *ep); ep++) {
            ;
        }
        *ep = '\0';
    }
    return ext;
}


/*
    Get the request extension. Look first at the URI pathInfo. If no extension, look at the filename if defined.
    Return NULL if no extension.
 */
PUBLIC char *httpGetExt(HttpStream *stream)
{
    HttpRx  *rx;
    char    *ext;

    rx = stream->rx;
    if ((ext = httpGetPathExt(rx->pathInfo)) == 0) {
        if (stream->tx->filename) {
            ext = httpGetPathExt(stream->tx->filename);
        }
    }
    return ext;
}


static int compareLang(char **s1, char **s2)
{
    return scmp(*s2, *s1);
}


PUBLIC HttpLang *httpGetLanguage(HttpStream *stream, MprHash *spoken, cchar *defaultLang)
{
    HttpRx      *rx;
    HttpLang    *lang;
    MprList     *list;
    cchar       *accept;
    char        *nextTok, *tok, *quality, *language;
    int         next;

    rx = stream->rx;
    if (rx->lang) {
        return rx->lang;
    }
    if (spoken == 0) {
        return 0;
    }
    list = mprCreateList(-1, MPR_LIST_STABLE);
    if ((accept = httpGetHeader(stream, "Accept-Language")) != 0) {
        for (tok = stok(sclone(accept), ",", &nextTok); tok; tok = stok(nextTok, ",", &nextTok)) {
            language = stok(tok, ";q=", &quality);
            if (quality == 0) {
                quality = "1";
            }
            mprAddItem(list, sfmt("%03d %s", (int) (atof(quality) * 100), language));
        }
        mprSortList(list, (MprSortProc) compareLang, 0);
        for (next = 0; (language = mprGetNextItem(list, &next)) != 0; ) {
            if ((lang = mprLookupKey(rx->route->languages, &language[4])) != 0) {
                rx->lang = lang;
                return lang;
            }
        }
    }
    if (defaultLang && (lang = mprLookupKey(rx->route->languages, defaultLang)) != 0) {
        rx->lang = lang;
        return lang;
    }
    return 0;
}


/*
    Trim extra path information after the uri extension. This is used by CGI and PHP only. The strategy is to
    heuristically find the script name in the uri. This is assumed to be the original uri up to and including
    first path component containing a "." Any path information after that is regarded as extra path.
    WARNING: Extra path is an old, unreliable, CGI specific technique. Do not use directories with embedded periods.
 */
PUBLIC void httpTrimExtraPath(HttpStream *stream)
{
    HttpRx      *rx;
    char        *cp, *extra;
    ssize       len;

    rx = stream->rx;
    if (!(rx->flags & (HTTP_OPTIONS | HTTP_TRACE))) {
        if ((cp = schr(rx->pathInfo, '.')) != 0 && (extra = schr(cp, '/')) != 0) {
            len = extra - rx->pathInfo;
            if (0 < len && len < slen(rx->pathInfo)) {
                rx->extraPath = sclone(&rx->pathInfo[len]);
                rx->pathInfo = snclone(rx->pathInfo, len);
            }
        }
        if ((cp = schr(rx->target, '.')) != 0 && (extra = schr(cp, '/')) != 0) {
            len = extra - rx->target;
            if (0 < len && len < slen(rx->target)) {
                rx->target[len] = '\0';
            }
        }
    }
}


PUBLIC void httpParseMethod(HttpStream *stream)
{
    HttpRx      *rx;
    cchar       *method;
    int         methodFlags;

    rx = stream->rx;
    method = rx->method;
    methodFlags = 0;

    switch (method[0]) {
    case 'D':
        if (strcmp(method, "DELETE") == 0) {
            methodFlags = HTTP_DELETE;
        }
        break;

    case 'G':
        if (strcmp(method, "GET") == 0) {
            methodFlags = HTTP_GET;
        }
        break;

    case 'H':
        if (strcmp(method, "HEAD") == 0) {
            methodFlags = HTTP_HEAD;
        }
        break;

    case 'O':
        if (strcmp(method, "OPTIONS") == 0) {
            methodFlags = HTTP_OPTIONS;
        }
        break;

    case 'P':
        if (strcmp(method, "POST") == 0) {
            methodFlags = HTTP_POST;
            rx->needInputPipeline = 1;

        } else if (strcmp(method, "PUT") == 0) {
            methodFlags = HTTP_PUT;
            rx->needInputPipeline = 1;
        }
        break;

    case 'T':
        if (strcmp(method, "TRACE") == 0) {
            methodFlags = HTTP_TRACE;
        }
        break;
    }
    rx->flags |= methodFlags;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
