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
PUBLIC void httpCreateCGIParams(HttpStream *stream)
{
    HttpRx          *rx;
    HttpTx          *tx;
    HttpHost        *host;
    HttpUploadFile  *file;
    MprSocket       *sock;
    MprHash         *svars;
    MprJson         *params;
    int             index;

    rx = stream->rx;
    if ((svars = rx->svars) != 0) {
        /* Do only once */
        return;
    }
    svars = rx->svars = mprCreateHash(HTTP_VAR_HASH_SIZE, MPR_HASH_STABLE);
    tx = stream->tx;
    host = stream->host;
    sock = stream->sock;

    mprAddKey(svars, "ROUTE_HOME", rx->route->home);

    mprAddKey(svars, "AUTH_TYPE", stream->authType);
    mprAddKey(svars, "AUTH_USER", stream->username);
    mprAddKey(svars, "AUTH_ACL", MPR->emptyString);
    mprAddKey(svars, "CONTENT_LENGTH", rx->contentLength);
    mprAddKey(svars, "CONTENT_TYPE", rx->mimeType);
    mprAddKey(svars, "DOCUMENTS", rx->route->documents);
    mprAddKey(svars, "GATEWAY_INTERFACE", sclone("CGI/1.1"));
    mprAddKey(svars, "QUERY_STRING", rx->parsedUri->query);
    mprAddKey(svars, "REMOTE_ADDR", stream->ip);
    mprAddKeyFmt(svars, "REMOTE_PORT", "%d", stream->port);

    /*
        Set to the same as AUTH_USER
     */
    mprAddKey(svars, "REMOTE_USER", stream->username);
    mprAddKey(svars, "REQUEST_METHOD", rx->method);
    mprAddKey(svars, "REQUEST_TRANSPORT", sclone((char*) ((stream->secure) ? "https" : "http")));
    mprAddKey(svars, "SERVER_ADDR", sock->acceptIp);
    mprAddKey(svars, "SERVER_NAME", host->name);
    mprAddKeyFmt(svars, "SERVER_PORT", "%d", sock->acceptPort);
    mprAddKey(svars, "SERVER_PROTOCOL", sclone(httpGetProtocol(stream->net)));
    mprAddKey(svars, "SERVER_SOFTWARE", stream->http->software);

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
        params = httpGetParams(stream);
        assert(params);
        for (ITERATE_ITEMS(rx->files, file, index)) {
            mprWriteJson(params, sfmt("FILE_%d_FILENAME", index), file->filename, MPR_JSON_STRING);
            mprWriteJson(params, sfmt("FILE_%d_CLIENT_FILENAME", index), file->clientFilename, MPR_JSON_STRING);
            mprWriteJson(params, sfmt("FILE_%d_CONTENT_TYPE", index), file->contentType, MPR_JSON_STRING);
            mprWriteJson(params, sfmt("FILE_%d_NAME", index), file->name, MPR_JSON_STRING);
            mprWriteJson(params, sfmt("FILE_%d_SIZE", index), sfmt("%zd", file->size), MPR_JSON_NUMBER);
        }
    }
    if (stream->http->envCallback) {
        stream->http->envCallback(stream);
    }
}


/*
    Add variables to the params. This comes from the query string and urlencoded post data.
    Make variables for each keyword in a query string. The buffer must be url encoded
    (ie. key=value&key2=value2..., spaces converted to '+' and all else should be %HEX encoded).
 */
static void addParamsFromBuf(HttpStream *stream, cchar *buf, ssize len)
{
    MprJson     *params, *prior;
    char        *newValue, *decoded, *keyword, *value, *tok;
    bool        json;

    assert(stream);
    params = httpGetParams(stream);

    /*
        Json encoded parameters tunneled via the query string. This is used to
        provide additional parameters on GET requests.
     */
    json = scontains(buf, "_encoded_json_") ? 1 : 0;
    if (json) {
        value = mprUriDecode(buf);
        mprParseJsonInto(value, params);
        return;
    }

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
            prior = mprGetJsonObj(params, keyword);
#if (ME_EJS_PRODUCT || ME_EJSCRIPT_PRODUCT) && (DEPRECATED || 1)
            if (prior && prior->type == MPR_JSON_VALUE) {
                if (*value) {
                    newValue = sjoin(prior->value, " ", value, NULL);
                    //  Uses SetJson instead of WriteJson which permits embedded . and []
                    mprSetJson(params, keyword, newValue, MPR_JSON_STRING);
                }
            } else {
                mprSetJson(params, keyword, value, MPR_JSON_STRING);
            }
#else
            if (prior && prior->type == MPR_JSON_VALUE) {
                if (*value) {
                    newValue = sjoin(prior->value, " ", value, NULL);
                    mprWriteJson(params, keyword, newValue, MPR_JSON_STRING);
                }
            } else {
                mprWriteJson(params, keyword, value, MPR_JSON_STRING);
            }
#endif
        }
        keyword = stok(0, "&", &tok);
    }
}


PUBLIC void httpAddQueryParams(HttpStream *stream)
{
    HttpRx      *rx;

    rx = stream->rx;
    if (rx->parsedUri->query && !(rx->flags & HTTP_ADDED_QUERY_PARAMS) && !stream->error) {
        addParamsFromBuf(stream, rx->parsedUri->query, slen(rx->parsedUri->query));
        rx->flags |= HTTP_ADDED_QUERY_PARAMS;
    }
}


PUBLIC int httpAddBodyParams(HttpStream *stream)
{
    HttpRx      *rx;
    HttpQueue   *q;
    MprBuf      *content;

    rx = stream->rx;
    q = stream->readq;

    if (rx->eof && (rx->form || rx->upload || rx->json) && !(rx->flags & HTTP_ADDED_BODY_PARAMS) && !rx->route && !stream->error) {
        httpJoinPackets(q, -1);
        if (q->first && q->first->content) {
            content = q->first->content;
            mprAddNullToBuf(content);
            if (rx->json) {
                if (mprParseJsonInto(httpGetBodyInput(stream), httpGetParams(stream)) == 0) {
                    return MPR_ERR_BAD_FORMAT;
                }
            } else {
                addParamsFromBuf(stream, mprGetBufStart(content), mprGetBufLength(content));
            }
        }
        rx->flags |= HTTP_ADDED_BODY_PARAMS;
    }
    return 0;
}


PUBLIC void httpAddJsonParams(HttpStream *stream)
{
    HttpRx      *rx;

    rx = stream->rx;
    if (rx->eof && sstarts(rx->mimeType, "application/json") && !stream->error) {
        if (!(rx->flags & HTTP_ADDED_BODY_PARAMS)) {
            mprParseJsonInto(httpGetBodyInput(stream), httpGetParams(stream));
            rx->flags |= HTTP_ADDED_BODY_PARAMS;
        }
    }
}


PUBLIC MprJson *httpGetParams(HttpStream *stream)
{
    if (stream->rx->params == 0) {
        stream->rx->params = mprCreateJson(MPR_JSON_OBJ);
    }
    return stream->rx->params;
}


PUBLIC int httpTestParam(HttpStream *stream, cchar *var)
{
    return mprGetJsonObj(httpGetParams(stream), var) != 0;
}


PUBLIC MprJson *httpGetParamObj(HttpStream *stream, cchar *var)
{
    return mprGetJsonObj(httpGetParams(stream), var);
}


PUBLIC int httpGetIntParam(HttpStream *stream, cchar *var, int defaultValue)
{
    cchar       *value;

    value = mprGetJson(httpGetParams(stream), var);
    return (value) ? (int) stoi(value) : defaultValue;
}


PUBLIC cchar *httpGetParam(HttpStream *stream, cchar *var, cchar *defaultValue)
{
    cchar       *value;

    value = mprGetJson(httpGetParams(stream), var);
    return (value) ? value : defaultValue;
}


static int sortParam(MprJson **j1, MprJson **j2)
{
    return scmp((*j1)->name, (*j2)->name);
}


/*
    Return the request parameters as a string.
    This will return the exact same string regardless of the order of form parameters.
 */
PUBLIC cchar *httpGetParamsString(HttpStream *stream)
{
    HttpRx      *rx;
    MprJson     *jp, *params;
    MprList     *list;
    char        *buf, *cp;
    ssize       len;
    int         ji, next;

    assert(stream);
    rx = stream->rx;

    if (rx->paramString == 0) {
        if ((params = stream->rx->params) != 0) {
            if ((list = mprCreateList(params->length, 0)) != 0) {
                len = 0;
                for (ITERATE_JSON(params, jp, ji)) {
                    if (jp->type & MPR_JSON_VALUE) {
                        mprAddItem(list, jp);
                        len += slen(jp->name) + slen(jp->value) + 2;
                    }
                }
                if ((buf = mprAlloc(len + 1)) != 0) {
                    mprSortList(list, (MprSortProc) sortParam, 0);
                    cp = buf;
                    for (next = 0; (jp = mprGetNextItem(list, &next)) != 0; ) {
                        strcpy(cp, jp->name); cp += slen(jp->name);
                        *cp++ = '=';
                        strcpy(cp, jp->value); cp += slen(jp->value);
                        *cp++ = '&';
                    }
                    cp[-1] = '\0';
                    rx->paramString = buf;
                }
            }
        }
    }
    return rx->paramString;
}


PUBLIC void httpRemoveParam(HttpStream *stream, cchar *var)
{
    mprRemoveJson(httpGetParams(stream), var);
}


PUBLIC void httpSetParam(HttpStream *stream, cchar *var, cchar *value)
{
    mprSetJson(httpGetParams(stream), var, value, 0);
}


PUBLIC void httpSetIntParam(HttpStream *stream, cchar *var, int value)
{
    mprSetJson(httpGetParams(stream), var, sfmt("%d", value), MPR_JSON_NUMBER);
}


PUBLIC bool httpMatchParam(HttpStream *stream, cchar *var, cchar *value)
{
    return smatch(value, httpGetParam(stream, var, " __UNDEF__ "));
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
