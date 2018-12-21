/*
    cache.c -- Http request route caching

    Caching operates as both a handler and an output filter. If acceptable cached content is found, the
    cacheHandler will serve it instead of the normal handler. If no content is acceptable and caching is enabled
    for the request, the cacheFilter will capture and save the response.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void cacheAtClient(HttpStream *stream);
static bool fetchCachedResponse(HttpStream *stream);
static char *makeCacheKey(HttpStream *stream);
static void manageHttpCache(HttpCache *cache, int flags);
static int matchCacheFilter(HttpStream *stream, HttpRoute *route, int dir);
static int matchCacheHandler(HttpStream *stream, HttpRoute *route, int dir);
static void outgoingCacheFilterService(HttpQueue *q);
static void readyCacheHandler(HttpQueue *q);
static void saveCachedResponse(HttpStream *stream);
static cchar *setHeadersFromCache(HttpStream *stream, cchar *content);

/************************************ Code ************************************/

PUBLIC int httpOpenCacheHandler()
{
    HttpStage     *handler, *filter;

    /*
        Create the cache handler to serve cached content
     */
    if ((handler = httpCreateHandler("cacheHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->cacheHandler = handler;
    handler->match = matchCacheHandler;
    handler->ready = readyCacheHandler;

    /*
        Create the cache filter to capture and cache response content
     */
    if ((filter = httpCreateFilter("cacheFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->cacheFilter = filter;
    filter->match = matchCacheFilter;
    filter->outgoingService = outgoingCacheFilterService;
    return 0;
}


/*
    See if there is acceptable cached content to serve
 */
static int matchCacheHandler(HttpStream *stream, HttpRoute *route, int dir)
{
    HttpCache   *cache;
    HttpRx      *rx;
    HttpTx      *tx;
    cchar       *mimeType, *ukey;
    int         next;

    rx = stream->rx;
    tx = stream->tx;

    /*
        Find first qualifying cache control entry. Any configured uri,method,extension,type must match.
     */
    for (next = 0; (cache = mprGetNextItem(rx->route->caching, &next)) != 0; ) {
        if (cache->uris) {
            if (cache->flags & HTTP_CACHE_HAS_PARAMS) {
                ukey = sfmt("%s?%s", rx->pathInfo, httpGetParamsString(stream));
            } else {
                ukey = rx->pathInfo;
            }
            if (!mprLookupKey(cache->uris, ukey)) {
                continue;
            }
        }
        if (cache->methods && !mprLookupKey(cache->methods, rx->method)) {
            continue;
        }
        if (cache->extensions && !mprLookupKey(cache->extensions, tx->ext)) {
            continue;
        }
        if (cache->types) {
            if ((mimeType = (char*) mprLookupMime(rx->route->mimeTypes, tx->ext)) == 0) {
                continue;
            }
            if (!mprLookupKey(cache->types, mimeType)) {
                continue;
            }
        }
        tx->cache = cache;

        if (cache->flags & HTTP_CACHE_CLIENT) {
            cacheAtClient(stream);
        }
        if (cache->flags & HTTP_CACHE_SERVER) {
            if (!(cache->flags & HTTP_CACHE_MANUAL) && fetchCachedResponse(stream)) {
                /* Found cached content, so we can use the cache handler */
                return HTTP_ROUTE_OK;
            }
            /*
                Caching is configured but no acceptable cached content yet.
                Create a capture buffer for the cacheFilter.
             */
            if (!tx->cacheBuffer) {
                tx->cacheBuffer = mprCreateBuf(-1, -1);
            }
        }
    }
    /*
        Cannot use the cache handler. Note: may still be using the cache filter.
     */
    return HTTP_ROUTE_REJECT;
}


static void readyCacheHandler(HttpQueue *q)
{
    HttpStream  *stream;
    HttpTx      *tx;
    cchar       *data;

    stream = q->stream;
    tx = stream->tx;

    if (tx->cachedContent) {
        if ((data = setHeadersFromCache(stream, tx->cachedContent)) != 0) {
            tx->length = slen(data);
            httpWriteString(q, data);
        }
    }
    httpFinalize(stream);
}


static int matchCacheFilter(HttpStream *stream, HttpRoute *route, int dir)
{
    if ((dir & HTTP_STAGE_TX) && stream->tx->cacheBuffer) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_OMIT_FILTER;
}


/*
    This will be enabled when caching is enabled for the route and there is no acceptable cache data to use.
    OR - manual caching has been enabled.
 */
static void outgoingCacheFilterService(HttpQueue *q)
{
    HttpPacket  *packet, *data;
    HttpStream  *stream;
    HttpTx      *tx;
    MprKey      *kp;
    cchar       *cachedData;
    ssize       size;

    stream = q->stream;
    tx = stream->tx;
    cachedData = 0;

    if (tx->status < 200 || tx->status > 299) {
        tx->cacheBuffer = 0;
    }

    /*
        This routine will save cached responses to tx->cacheBuffer.
        It will also send cached data if the X-SendCache header is present. Normal caching is done by cacheHandler.
     */
    if (mprLookupKey(stream->tx->headers, "X-SendCache") != 0) {
        if (fetchCachedResponse(stream)) {
            httpLog(stream->trace, "cache.sendcache", "context", "msg:'Using cached content'");
            cachedData = setHeadersFromCache(stream, tx->cachedContent);
            tx->length = slen(cachedData);
        }
    }
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!httpWillNextQueueAcceptPacket(q, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        if (packet->flags & HTTP_PACKET_DATA) {
            if (cachedData) {
                /*
                    Using X-SendCache. Discard the packet.
                 */
                continue;

            } else if (tx->cacheBuffer) {
                /*
                    Save the response packet to the cache buffer. Will write below in saveCachedResponse.
                 */
                if (mprGetBufLength(tx->cacheBuffer) == 0) {
                    /*
                        Add defined headers to the start of the cache buffer. Separate with a double newline.
                     */
                    mprPutToBuf(tx->cacheBuffer, "X-Status: %d\n", tx->status);
                    for (kp = 0; (kp = mprGetNextKey(tx->headers, kp)) != 0; ) {
                        mprPutToBuf(tx->cacheBuffer, "%s: %s\n", kp->key, (char*) kp->data);
                    }
                    mprPutCharToBuf(tx->cacheBuffer, '\n');
                }
                size = mprGetBufLength(packet->content);
                if ((tx->cacheBufferLength + size) < stream->limits->cacheItemSize) {
                    mprPutBlockToBuf(tx->cacheBuffer, mprGetBufStart(packet->content), mprGetBufLength(packet->content));
                    tx->cacheBufferLength += size;
                } else {
                    tx->cacheBuffer = 0;
                    httpLog(stream->trace, "cache.big", "context", "msg:'Item too big to cache',size:%zu,limit:%u",
                        tx->cacheBufferLength + size, stream->limits->cacheItemSize);
                }
            }

        } else if (packet->flags & HTTP_PACKET_END) {
            if (cachedData) {
                /*
                    Using X-SendCache but there was no data packet to replace. So do the write here.
                 */
                data = httpCreateDataPacket((ssize) tx->length);
                mprPutBlockToBuf(data->content, cachedData, (ssize) tx->length);
                httpPutPacketToNext(q, data);

            } else if (tx->cacheBuffer) {
                /*
                    Save the cache buffer to the cache store
                 */
                saveCachedResponse(stream);
            }
        }
        httpPutPacketToNext(q, packet);
    }
}


/*
    Find a qualifying cache control entry. Any configured uri,method,extension,type must match.
 */
static void cacheAtClient(HttpStream *stream)
{
    HttpTx      *tx;
    HttpCache   *cache;
    cchar       *value;

    tx = stream->tx;
    cache = stream->tx->cache;

    if (tx->status == HTTP_CODE_OK && !mprLookupKey(tx->headers, "Cache-Control")) {
        if ((value = mprLookupKey(stream->tx->headers, "Cache-Control")) != 0) {
            if (strstr(value, "max-age") == 0) {
                httpAppendHeader(stream, "Cache-Control", "public, max-age=%lld", cache->clientLifespan / TPS);
            }
        } else {
            httpAddHeader(stream, "Cache-Control", "public, max-age=%lld", cache->clientLifespan / TPS);
            /*
                Old HTTP/1.0 clients don't understand Cache-Control
             */
            httpAddHeaderString(stream, "Expires", mprFormatUniversalTime(MPR_HTTP_DATE,
                mprGetTime() + cache->clientLifespan));
        }
    }
}


/*
    See if there is acceptable cached content for this request. If so, return true.
    Will setup tx->cacheBuffer as a side-effect if the output should be captured and cached.
 */
static bool fetchCachedResponse(HttpStream *stream)
{
    HttpTx      *tx;
    MprTime     modified, when;
    cchar       *value, *key, *tag;
    int         status, cacheOk, canUseClientCache;

    tx = stream->tx;

    /*
        Transparent caching. Manual caching must manually call httpWriteCached()
     */
    key = makeCacheKey(stream);
    if ((value = httpGetHeader(stream, "Cache-Control")) != 0 &&
            (scontains(value, "max-age=0") == 0 || scontains(value, "no-cache") == 0)) {
        httpLog(stream->trace, "cache.reload", "context", "msg:'Client reload'");

    } else if ((tx->cachedContent = mprReadCache(stream->host->responseCache, key, &modified, 0)) != 0) {
        /*
            See if a NotModified response can be served. This is much faster than sending the response.
            Observe headers:
                If-None-Match: "ec18d-54-4d706a63"
                If-Modified-Since: Fri, 04 Mar 2014 04:28:19 GMT
            Set status to OK when content must be transmitted.
         */
        cacheOk = 1;
        canUseClientCache = 0;
        tag = mprGetMD5(key);
        if ((value = httpGetHeader(stream, "If-None-Match")) != 0) {
            canUseClientCache = 1;
            if (scmp(value, tag) != 0) {
                cacheOk = 0;
            }
        }
        if (cacheOk && (value = httpGetHeader(stream, "If-Modified-Since")) != 0) {
            canUseClientCache = 1;
            mprParseTime(&when, value, 0, 0);
            if (modified > when) {
                cacheOk = 0;
            }
        }
        status = (canUseClientCache && cacheOk) ? HTTP_CODE_NOT_MODIFIED : HTTP_CODE_OK;
        httpLog(stream->trace, "cache.cached", "context", "msg:'Use cached content',key:'%s',status:%d", key, status);
        httpSetStatus(stream, status);
        httpSetHeaderString(stream, "Etag", mprGetMD5(key));
        httpSetHeaderString(stream, "Last-Modified", mprFormatUniversalTime(MPR_HTTP_DATE, modified));
        httpRemoveHeader(stream, "Content-Encoding");
        return 1;
    }
    httpLog(stream->trace, "cache.none", "context", "msg:'No cached content',key:'%s'", key);
    return 0;
}


static void saveCachedResponse(HttpStream *stream)
{
    HttpTx      *tx;
    MprBuf      *buf;
    MprTime     modified;

    tx = stream->tx;
    assert(tx->finalizedOutput && tx->cacheBuffer);

    buf = tx->cacheBuffer;
    tx->cacheBuffer = 0;
    /*
        Truncate modified time to get a 1 sec resolution. This is the resolution for If-Modified headers.
     */
    modified = mprGetTime() / TPS * TPS;
    mprWriteCache(stream->host->responseCache, makeCacheKey(stream), mprGetBufStart(buf), modified,
        tx->cache->serverLifespan, 0, 0);
}


PUBLIC ssize httpWriteCached(HttpStream *stream)
{
    MprTime     modified;
    cchar       *cacheKey, *data, *content;

    if (!stream->tx->cache) {
        return MPR_ERR_CANT_FIND;
    }
    cacheKey = makeCacheKey(stream);
    if ((content = mprReadCache(stream->host->responseCache, cacheKey, &modified, 0)) == 0) {
        httpLog(stream->trace, "cache.none", "context", "msg:'No response data in cache', key:'%s'", cacheKey);
        return 0;
    }
    httpLog(stream->trace, "cache.cached", "context", "msg:'Used cached response', key:'%s'", cacheKey);
    data = setHeadersFromCache(stream, content);
    httpSetHeaderString(stream, "Etag", mprGetMD5(cacheKey));
    httpSetHeaderString(stream, "Last-Modified", mprFormatUniversalTime(MPR_HTTP_DATE, modified));
    stream->tx->cacheBuffer = 0;
    httpWriteString(stream->writeq, data);
    httpFinalizeOutput(stream);
    return slen(data);
}


PUBLIC ssize httpUpdateCache(HttpStream *stream, cchar *uri, cchar *data, MprTicks lifespan)
{
    cchar   *key;
    ssize   len;

    len = slen(data);
    if (len > stream->limits->cacheItemSize) {
        return MPR_ERR_WONT_FIT;
    }
    if (lifespan <= 0) {
        lifespan = stream->rx->route->lifespan;
    }
    key = sfmt("http::response::%s", uri);
    if (data == 0 || lifespan <= 0) {
        mprRemoveCache(stream->host->responseCache, key);
        return 0;
    }
    return mprWriteCache(stream->host->responseCache, key, data, 0, lifespan, 0, 0);
}


/*
    Add cache configuration to the route. This can be called multiple times.
    Uris, extensions and methods may optionally provide a space or comma separated list of items.
    If URI is NULL or "*", cache all URIs for this route. Otherwise, cache only the given URIs.
    The URIs may contain an ordered set of request parameters. For example: "/user/show?name=john&posts=true"
    Note: the URI should not include the route prefix (scriptName)
    The extensions should not contain ".". The methods may contain "*" for all methods.
 */
PUBLIC void httpAddCache(HttpRoute *route, cchar *methods, cchar *uris, cchar *extensions, cchar *types,
        MprTicks clientLifespan, MprTicks serverLifespan, int flags)
{
    HttpCache   *cache;
    char        *item, *tok;

    cache = 0;
    if (!route->caching) {
        if (route->handler) {
            mprLog("error http cache", 0,
                "Caching handler disabled because SetHandler used in route %s. Use AddHandler instead", route->pattern);
        }
        httpAddRouteHandler(route, "cacheHandler", NULL);
        httpAddRouteFilter(route, "cacheFilter", "", HTTP_STAGE_TX);
        route->caching = mprCreateList(0, MPR_LIST_STABLE);

    } else if (flags & HTTP_CACHE_RESET) {
        route->caching = mprCreateList(0, MPR_LIST_STABLE);

    } else if (route->parent && route->caching == route->parent->caching) {
        route->caching = mprCloneList(route->parent->caching);
    }
    if ((cache = mprAllocObj(HttpCache, manageHttpCache)) == 0) {
        return;
    }
    if (extensions) {
        cache->extensions = mprCreateHash(0, MPR_HASH_STABLE);
        for (item = stok(sclone(extensions), " \t,", &tok); item; item = stok(0, " \t,", &tok)) {
            if (*item && !smatch(item, "*")) {
                mprAddKey(cache->extensions, item, cache);
            }
        }
    } else if (types) {
        cache->types = mprCreateHash(0, MPR_HASH_STABLE);
        for (item = stok(sclone(types), " \t,", &tok); item; item = stok(0, " \t,", &tok)) {
            if (*item && !smatch(item, "*")) {
                mprAddKey(cache->types, item, cache);
            }
        }
    } else if (flags & HTTP_CACHE_STATIC) {
        cache->extensions = mprCreateHash(0, MPR_HASH_STABLE);
        mprAddKey(cache->extensions, "css", cache);
        mprAddKey(cache->extensions, "gif", cache);
        mprAddKey(cache->extensions, "ico", cache);
        mprAddKey(cache->extensions, "jpg", cache);
        mprAddKey(cache->extensions, "js", cache);
        mprAddKey(cache->extensions, "html", cache);
        mprAddKey(cache->extensions, "png", cache);
        mprAddKey(cache->extensions, "pdf", cache);
        mprAddKey(cache->extensions, "ttf", cache);
        mprAddKey(cache->extensions, "txt", cache);
        mprAddKey(cache->extensions, "xml", cache);
        mprAddKey(cache->extensions, "woff", cache);
    }
    if (methods) {
        cache->methods = mprCreateHash(0, MPR_HASH_CASELESS | MPR_HASH_STABLE);
        for (item = stok(sclone(methods), " \t,", &tok); item; item = stok(0, " \t,", &tok)) {
            if (smatch(item, "*")) {
                methods = 0;
            } else {
                mprAddKey(cache->methods, item, cache);
            }
        }
    }
    if (uris) {
        cache->uris = mprCreateHash(0, MPR_HASH_STABLE);
        for (item = stok(sclone(uris), " \t,", &tok); item; item = stok(0, " \t,", &tok)) {
            mprAddKey(cache->uris, item, cache);
            if (schr(item, '?')) {
                flags |= HTTP_CACHE_UNIQUE;
            }
        }
    }
    if (clientLifespan <= 0) {
        clientLifespan = route->lifespan;
    }
    cache->clientLifespan = clientLifespan;
    if (serverLifespan <= 0) {
        serverLifespan = route->lifespan;
    }
    cache->serverLifespan = serverLifespan;
    cache->flags = flags;
    mprAddItem(route->caching, cache);
}


static void manageHttpCache(HttpCache *cache, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(cache->extensions);
        mprMark(cache->methods);
        mprMark(cache->types);
        mprMark(cache->uris);
    }
}


static char *makeCacheKey(HttpStream *stream)
{
    HttpRx      *rx;

    rx = stream->rx;
    if (stream->tx->cache->flags & HTTP_CACHE_UNIQUE) {
        return sfmt("http::response::%s%s?%s", rx->route->prefix, rx->pathInfo, httpGetParamsString(stream));
    } else {
        return sfmt("http::response::%s%s", rx->route->prefix, rx->pathInfo);
    }
}


/*
    Parse cached content of the form:  headers \n\n data
    Set headers in the current request and return a reference to the data portion
 */
static cchar *setHeadersFromCache(HttpStream *stream, cchar *content)
{
    cchar   *data;
    char    *header, *headers, *key, *value, *tok;

    if ((data = strstr(content, "\n\n")) == 0) {
        data = content;
    } else {
        headers = snclone(content, data - content);
        data += 2;
        for (header = stok(headers, "\n", &tok); header; header = stok(NULL, "\n", &tok)) {
            key = ssplit(header, ": ", &value);
            if (smatch(key, "X-Status")) {
                stream->tx->status = (int) stoi(value);
            } else {
                httpAddHeaderString(stream, key, value);
            }
        }
    }
    return data;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
