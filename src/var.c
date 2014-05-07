/*
    var.c -- Manage the request variables
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Defines ***********************************/

#define HTTP_VAR_HASH_SIZE  61           /* Hash size for vars and params */

/*********************************** Code *************************************/
/*
    Define standard CGI variables
 */
PUBLIC void httpCreateCGIParams(HttpConn *conn)
{
    HttpRx          *rx;
    HttpTx          *tx;
    HttpHost        *host;
    HttpUploadFile  *up;
    MprSocket       *sock;
    MprHash         *svars;
    MprJson         *params;
    MprKey          *kp;
    int             index;

    rx = conn->rx;
    if ((svars = rx->svars) != 0) {
        /* Do only once */
        return;
    }
    svars = rx->svars = mprCreateHash(HTTP_VAR_HASH_SIZE, MPR_HASH_STABLE);
    tx = conn->tx;
    host = conn->host;
    sock = conn->sock;

    mprAddKey(svars, "ROUTE_HOME", rx->route->home);

    mprAddKey(svars, "AUTH_TYPE", conn->authType);
    mprAddKey(svars, "AUTH_USER", conn->username);
    mprAddKey(svars, "AUTH_ACL", MPR->emptyString);
    mprAddKey(svars, "CONTENT_LENGTH", rx->contentLength);
    mprAddKey(svars, "CONTENT_TYPE", rx->mimeType);
    mprAddKey(svars, "DOCUMENTS", rx->route->documents);
    mprAddKey(svars, "GATEWAY_INTERFACE", sclone("CGI/1.1"));
    mprAddKey(svars, "QUERY_STRING", rx->parsedUri->query);
    mprAddKey(svars, "REMOTE_ADDR", conn->ip);
    mprAddKeyFmt(svars, "REMOTE_PORT", "%d", conn->port);

    /* 
        Set to the same as AUTH_USER 
     */
    mprAddKey(svars, "REMOTE_USER", conn->username);
    mprAddKey(svars, "REQUEST_METHOD", rx->method);
    mprAddKey(svars, "REQUEST_TRANSPORT", sclone((char*) ((conn->secure) ? "https" : "http")));
    mprAddKey(svars, "SERVER_ADDR", sock->acceptIp);
    mprAddKey(svars, "SERVER_NAME", host->name);
    mprAddKeyFmt(svars, "SERVER_PORT", "%d", sock->acceptPort);
    mprAddKey(svars, "SERVER_PROTOCOL", conn->protocol);
    mprAddKey(svars, "SERVER_SOFTWARE", conn->http->software);

    /*
        For PHP, REQUEST_URI must be the original URI. The SCRIPT_NAME will refer to the new pathInfo
     */
    mprAddKey(svars, "REQUEST_URI", rx->originalUri);

    /*
        URIs are broken into the following: http://{SERVER_NAME}:{SERVER_PORT}{SCRIPT_NAME}{PATH_INFO} 
        NOTE: Appweb refers to pathInfo as the app relative URI and scriptName as the app address before the pathInfo.
        In CGI|PHP terms, the scriptName is the appweb rx->pathInfo and the PATH_INFO is the extraPath. 
     */
    mprAddKey(svars, "PATH_INFO", rx->extraPath);
    mprAddKeyFmt(svars, "SCRIPT_NAME", "%s%s", rx->scriptName, rx->pathInfo);
    mprAddKey(svars, "SCRIPT_FILENAME", tx->filename);
    if (rx->extraPath) {
        /*
            Only set PATH_TRANSLATED if extraPath is set (CGI spec) 
         */
        assert(rx->extraPath[0] == '/');
        mprAddKey(svars, "PATH_TRANSLATED", mprNormalizePath(sfmt("%s%s", rx->route->documents, rx->extraPath)));
    }
    if (rx->files) {
        params = httpGetParams(conn);
        assert(params);
        for (index = 0, kp = 0; (kp = mprGetNextKey(rx->files, kp)) != 0; index++) {
            up = (HttpUploadFile*) kp->data;
            mprSetJson(params, sfmt("FILE_%d_FILENAME", index), up->filename);
            mprSetJson(params, sfmt("FILE_%d_CLIENT_FILENAME", index), up->clientFilename);
            mprSetJson(params, sfmt("FILE_%d_CONTENT_TYPE", index), up->contentType);
            mprSetJson(params, sfmt("FILE_%d_NAME", index), kp->key);
            mprSetJson(params, sfmt("FILE_%d_SIZE", index), sfmt("%d", up->size));
        }
    }
    if (conn->http->envCallback) {
        conn->http->envCallback(conn);
    }
}


/*
    Add variables to the params. This comes from the query string and urlencoded post data.
    Make variables for each keyword in a query string. The buffer must be url encoded 
    (ie. key=value&key2=value2..., spaces converted to '+' and all else should be %HEX encoded).
 */
static void addParamsFromBuf(HttpConn *conn, cchar *buf, ssize len)
{
    MprJson     *params, *prior;
    char        *newValue, *decoded, *keyword, *value, *tok;

    assert(conn);
    params = httpGetParams(conn);
    decoded = mprAlloc(len + 1);
    decoded[len] = '\0';
    memcpy(decoded, buf, len);

    keyword = stok(decoded, "&", &tok);
    while (keyword != 0) {
        if ((value = strchr(keyword, '=')) != 0) {
            *value++ = '\0';
            value = mprUriDecode(value);
        } else {
            value = MPR->emptyString;
        }
        keyword = mprUriDecode(keyword);
        if (*keyword) {
            /*
                Append to existing keywords
             */
            prior = mprLookupJsonObj(params, keyword);
            if (prior && prior->type == MPR_JSON_VALUE) {
                if (*value) {
                    newValue = sjoin(prior->value, " ", value, NULL);
                    mprSetJson(params, keyword, newValue);
                }
            } else {
                mprSetJson(params, keyword, value);
            }
        }
        keyword = stok(0, "&", &tok);
    }
}


PUBLIC void httpAddQueryParams(HttpConn *conn) 
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->parsedUri->query && !(rx->flags & HTTP_ADDED_QUERY_PARAMS)) {
        addParamsFromBuf(conn, rx->parsedUri->query, slen(rx->parsedUri->query));
        rx->flags |= HTTP_ADDED_QUERY_PARAMS;
    }
}


PUBLIC int httpAddBodyParams(HttpConn *conn)
{
    HttpRx      *rx;
    HttpQueue   *q;
    MprBuf      *content;

    rx = conn->rx;
    q = conn->readq;

    if (rx->eof && !(rx->flags & HTTP_ADDED_BODY_PARAMS)) {
        if (q->first && q->first->content) {
            httpJoinPackets(q, -1);
            content = q->first->content;
            if (rx->form || rx->upload) {
                mprAddNullToBuf(content);
                mprTrace(6, "Form body data: length %d, \"%s\"", mprGetBufLength(content), mprGetBufStart(content));
                addParamsFromBuf(conn, mprGetBufStart(content), mprGetBufLength(content));

            } else if (sstarts(rx->mimeType, "application/json")) {
                if (mprParseJsonInto(httpGetBodyInput(conn), httpGetParams(conn)) == 0) {
                    return MPR_ERR_BAD_FORMAT;
                }
            }
        }
        rx->flags |= HTTP_ADDED_BODY_PARAMS;
    }
    return 0;
}


PUBLIC void httpAddJsonParams(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->eof && sstarts(rx->mimeType, "application/json")) {
        if (!(rx->flags & HTTP_ADDED_BODY_PARAMS)) {
            mprParseJsonInto(httpGetBodyInput(conn), httpGetParams(conn));
            rx->flags |= HTTP_ADDED_BODY_PARAMS;
        }
    }
}


PUBLIC MprJson *httpGetParams(HttpConn *conn)
{ 
    if (conn->rx->params == 0) {
        conn->rx->params = mprCreateJson(MPR_JSON_OBJ);
    }
    return conn->rx->params;
}


PUBLIC int httpTestParam(HttpConn *conn, cchar *var)
{
    return mprLookupJsonObj(httpGetParams(conn), var) != 0;
}


PUBLIC cchar *httpGetParam(HttpConn *conn, cchar *var, cchar *defaultValue)
{
    cchar       *value;

    value = mprLookupJson(httpGetParams(conn), var);
    return (value) ? value : defaultValue;
}


PUBLIC int httpGetIntParam(HttpConn *conn, cchar *var, int defaultValue)
{
    cchar       *value;

    value = mprLookupJson(httpGetParams(conn), var);
    return (value) ? (int) stoi(value) : defaultValue;
}


/*
    Return the request parameters as a string. 
    This will return the exact same string regardless of the order of form parameters.
 */
PUBLIC char *httpGetParamsString(HttpConn *conn)
{
    HttpRx      *rx;

    assert(conn);
    rx = conn->rx;

    if (rx->paramString == 0) {
        rx->paramString = mprJsonToString(rx->params, 0);
    }
    return rx->paramString;
}


PUBLIC void httpRemoveParam(HttpConn *conn, cchar *var) 
{
    mprRemoveJson(httpGetParams(conn), var);
}


PUBLIC void httpSetParam(HttpConn *conn, cchar *var, cchar *value) 
{
    mprSetJson(httpGetParams(conn), var, value);
}


PUBLIC void httpSetIntParam(HttpConn *conn, cchar *var, int value) 
{
    mprSetJson(httpGetParams(conn), var, sfmt("%d", value));
}


PUBLIC bool httpMatchParam(HttpConn *conn, cchar *var, cchar *value)
{
    return smatch(value, httpGetParam(conn, var, " __UNDEF__ "));
}


PUBLIC void httpAddUploadFile(HttpConn *conn, cchar *id, HttpUploadFile *upfile)
{
    HttpRx   *rx;

    rx = conn->rx;
    if (rx->files == 0) {
        rx->files = mprCreateHash(-1, MPR_HASH_STABLE);
    }
    mprAddKey(rx->files, id, upfile);
}


PUBLIC void httpRemoveUploadFile(HttpConn *conn, cchar *id)
{
    HttpRx    *rx;
    HttpUploadFile  *upfile;

    rx = conn->rx;

    upfile = (HttpUploadFile*) mprLookupKey(rx->files, id);
    if (upfile) {
        mprDeletePath(upfile->filename);
        upfile->filename = 0;
    }
}


PUBLIC void httpRemoveAllUploadedFiles(HttpConn *conn)
{
    HttpRx          *rx;
    HttpUploadFile  *upfile;
    MprKey          *kp;

    rx = conn->rx;

    for (kp = 0; rx->files && (kp = mprGetNextKey(rx->files, kp)) != 0; ) {
        upfile = (HttpUploadFile*) kp->data;
        if (upfile->filename) {
            mprDeletePath(upfile->filename);
            upfile->filename = 0;
        }
    }
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
