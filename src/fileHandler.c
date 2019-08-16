/*
    fileHandler.c -- Static file content handler

    This handler manages static file based content such as HTML, GIF /or JPEG pages. It supports all methods including:
    GET, PUT, DELETE, OPTIONS and TRACE. It is event based and does not use worker threads.

    The fileHandler also manages requests for directories that require redirection to an index or responding with
    a directory listing.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void closeFileHandler(HttpQueue *q);
static void handleDeleteRequest(HttpQueue *q);
static void handlePutRequest(HttpQueue *q);
static void incomingFile(HttpQueue *q, HttpPacket *packet);
static int openFileHandler(HttpQueue *q);
static void outgoingFileService(HttpQueue *q);
static ssize readFileData(HttpQueue *q, HttpPacket *packet, MprOff pos, ssize size);
static void readyFileHandler(HttpQueue *q);
static int rewriteFileHandler(HttpStream *stream);
static void startFileHandler(HttpQueue *q);

/*********************************** Code *************************************/
/*
    Loadable module initialization
 */
PUBLIC int httpOpenFileHandler()
{
    HttpStage     *handler;

    /*
        This handler serves requests without using thread workers.
     */
    if ((handler = httpCreateHandler("fileHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    handler->rewrite = rewriteFileHandler;
    handler->open = openFileHandler;
    handler->close = closeFileHandler;
    handler->start = startFileHandler;
    handler->ready = readyFileHandler;
    handler->outgoingService = outgoingFileService;
    handler->incoming = incomingFile;
    HTTP->fileHandler = handler;
    return 0;
}

/*
    Rewrite the request for directories, indexes and compressed content.
 */
static int rewriteFileHandler(HttpStream *stream)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprPath     *info;

    rx = stream->rx;
    tx = stream->tx;
    info = &tx->fileInfo;

    httpMapFile(stream);
    assert(info->checked);

    if (rx->flags & (HTTP_DELETE | HTTP_PUT)) {
        return HTTP_ROUTE_OK;
    }
    if (info->isDir) {
        return httpHandleDirectory(stream);
    }
    if (rx->flags & (HTTP_GET | HTTP_HEAD | HTTP_POST) && info->valid) {
        /*
            The sendFile connector is optimized on some platforms to use the sendfile() system call.
            Set the entity length for the sendFile connector to utilize.
         */
        tx->entityLength = tx->fileInfo.size;
    }
    return HTTP_ROUTE_OK;
}


/*
    This is called after the headers are parsed
 */
static int openFileHandler(HttpQueue *q)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpStream  *stream;
    MprPath     *info;
    char        *date, dbuf[16];
    MprHash     *dateCache;

    stream = q->stream;
    tx = stream->tx;
    rx = stream->rx;
    info = &tx->fileInfo;

    if (stream->error) {
        return MPR_ERR_CANT_OPEN;
    }
    if (rx->flags & (HTTP_GET | HTTP_HEAD | HTTP_POST)) {
        if (!(info->valid || info->isDir)) {
            httpError(stream, HTTP_CODE_NOT_FOUND, "Cannot find document");
            return 0;
        }
        if (!tx->etag) {
            /* Set the etag for caching in the client */
            tx->etag = itos(info->inode + info->size + info->mtime);
        }
        if (info->mtime) {
            dateCache = stream->http->dateCache;
            if ((date = mprLookupKey(dateCache, itosbuf(dbuf, sizeof(dbuf), (int64) info->mtime, 10))) == 0) {
                if (!dateCache || mprGetHashLength(dateCache) > 128) {
                    stream->http->dateCache = dateCache = mprCreateHash(0, 0);
                }
                date = httpGetDateString(&tx->fileInfo);
                mprAddKey(dateCache, itosbuf(dbuf, sizeof(dbuf), (int64) info->mtime, 10), date);
            }
            httpSetHeaderString(stream, "Last-Modified", date);
        }
        if (httpContentNotModified(stream)) {
            httpSetStatus(stream, HTTP_CODE_NOT_MODIFIED);
            httpRemoveHeader(stream, "Content-Encoding");
            httpOmitBody(stream);
        }
        if (!tx->fileInfo.isReg && !tx->fileInfo.isLink) {
            httpLog(stream->trace, "fileHandler.error", "error", "msg:'Document is not a regular file',filename:'%s'",
                tx->filename);
            httpError(stream, HTTP_CODE_NOT_FOUND, "Cannot serve document");

        } else if (tx->fileInfo.size > stream->limits->txBodySize &&
                stream->limits->txBodySize != HTTP_UNLIMITED) {
            httpError(stream, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
                "Http transmission aborted. File size exceeds max body of %lld bytes", stream->limits->txBodySize);

        } else {
            /*
                If using the net connector, open the file if a body must be sent with the response. The file will be
                automatically closed when the request completes.
             */
            if (!(tx->flags & HTTP_TX_NO_BODY)) {
                tx->file = mprOpenFile(tx->filename, O_RDONLY | O_BINARY, 0);
                if (tx->file == 0) {
                    if (rx->referrer && *rx->referrer) {
                        httpLog(stream->trace, "fileHandler.error", "error", "msg:'Cannot open document',filename:'%s',referrer:'%s'",
                            tx->filename, rx->referrer);
                    } else {
                        httpLog(stream->trace, "fileHandler.error", "error", "msg:'Cannot open document',filename:'%s'", tx->filename);
                    }
                    httpError(stream, HTTP_CODE_NOT_FOUND, "Cannot open document");
                }
            }
        }
    } else if (rx->flags & HTTP_DELETE) {
        handleDeleteRequest(q);

    } else if (rx->flags & HTTP_OPTIONS) {
        httpHandleOptions(q->stream);

    } else if (rx->flags & HTTP_PUT) {
        handlePutRequest(q);

    } else {
        httpError(stream, HTTP_CODE_BAD_METHOD, "Unsupported method");
    }
    return 0;
}


/*
    Called when the request is complete
 */
static void closeFileHandler(HttpQueue *q)
{
    HttpTx  *tx;

    tx = q->stream->tx;
    if (tx->file) {
        mprCloseFile(tx->file);
        tx->file = 0;
    }
}


/*
    Called when all the body content has been received, but may not have yet been processed by our incoming()
 */
static void startFileHandler(HttpQueue *q)
{
    HttpStream  *stream;
    HttpTx      *tx;
    HttpPacket  *packet;

    stream = q->stream;
    tx = stream->tx;

    if (stream->rx->flags & HTTP_HEAD) {
        tx->length = tx->entityLength;
        httpFinalizeOutput(stream);

    } else if (stream->rx->flags & HTTP_PUT) {
        /*
            Delay finalizing output until all input data is received incase the socket is disconnected
            httpFinalizeOutput(stream);
         */

    } else if (stream->rx->flags & (HTTP_GET | HTTP_POST)) {
        if ((!(tx->flags & HTTP_TX_NO_BODY)) && (tx->entityLength >= 0 && !stream->error)) {
            /*
                Create a single data packet based on the actual entity (file) length
             */
            packet = httpCreateEntityPacket(0, tx->entityLength, readFileData);

            /*
                Set the content length if not chunking and not using ranges
             */
            if (!tx->outputRanges && tx->chunkSize < 0) {
                tx->length = tx->entityLength;
            }
            httpPutPacket(q, packet);
        }
    } else {
        httpFinalizeOutput(stream);
    }
}


/*
    The ready callback is invoked when all the input body data has been received
    The queue already contains a single data packet representing all the output data.
 */
static void readyFileHandler(HttpQueue *q)
{
    httpScheduleQueue(q);
}


/*
    Populate a packet with file data. Return the number of bytes read or a negative error code.
 */
static ssize readFileData(HttpQueue *q, HttpPacket *packet, MprOff pos, ssize size)
{
    HttpStream  *stream;
    HttpTx      *tx;
    ssize       nbytes;

    stream = q->stream;
    tx = stream->tx;

    if (size <= 0) {
        return 0;
    }
    if (mprGetBufSpace(packet->content) < size) {
        size = mprGetBufSpace(packet->content);
    }
    if (pos >= 0) {
        mprSeekFile(tx->file, SEEK_SET, pos);
    }
    if ((nbytes = mprReadFile(tx->file, mprGetBufStart(packet->content), size)) != size) {
        /*
            As we may have sent some data already to the client, the only thing we can do is abort and hope the client
            notices the short data.
         */
        httpError(stream, HTTP_CODE_SERVICE_UNAVAILABLE, "Cannot read file %s", tx->filename);
        return MPR_ERR_CANT_READ;
    }
    mprAdjustBufEnd(packet->content, nbytes);
    return nbytes;
}


/*
    The service callback will be invoked to service outgoing packets on the service queue. It will only be called
    once all incoming data has been received and then when the downstream queues drain sufficiently to absorb
    more data. This routine may flow control if the downstream stage cannot accept all the file data. It will
    then be re-called as required to send more data.
 */
static void outgoingFileService(HttpQueue *q)
{
    HttpStream  *stream;
    HttpPacket  *data, *packet;
    ssize       size, nbytes;

    stream = q->stream;

#if UNUSED
    /*
        There will be only one entity data packet. PrepPacket will read data into the packet and then
        put the remaining entity packet on the queue where it will be examined again until the down stream queue is full.
     */
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (packet->fill) {
            size = min(packet->esize, q->packetSize);
            size = min(size, q->nextQ->packetSize);
            if (size > 0) {
                data = httpCreateDataPacket(size);
                if ((nbytes = readFileData(q, data, q->ioPos, size)) < 0) {
                    httpError(stream, HTTP_CODE_NOT_FOUND, "Cannot read document");
                    return;
                }
                q->ioPos += nbytes;
                packet->epos += nbytes;
                packet->esize -= nbytes;
                if (packet->esize > 0) {
                    httpPutBackPacket(q, packet);
                }
                /*
                    This may split the packet and put back the tail portion ahead of the just putback entity packet.
                 */
                if (!httpWillNextQueueAcceptPacket(q, data)) {
                    httpPutBackPacket(q, data);
                    if (packet->esize == 0) {
                        httpFinalizeOutput(stream);
                    }
                    break;
                }
                httpPutPacketToNext(q, data);
            }
            if (packet->esize == 0) {
                httpFinalizeOutput(stream);
            }
        } else {
            /* Don't flow control as the packet is already consuming memory */
            httpPutPacketToNext(q, packet);
        }
    }
#else
    /*
        The queue will contain an entity packet which holds the position from which to read in the file.
        If the downstream queue is full, the data packet will be put onto the queue ahead of the entity packet.
        When EOF, and END packet will be added to the queue via httpFinalizeOutput which will then be sent.
     */
    for (packet = q->first; packet; packet = q->first) {
        if (packet->fill) {
            size = min(packet->esize, q->packetSize);
            size = min(size, q->nextQ->packetSize);
            if (size > 0) {
                data = httpCreateDataPacket(size);
                if ((nbytes = readFileData(q, data, q->ioPos, size)) < 0) {
                    httpError(stream, HTTP_CODE_NOT_FOUND, "Cannot read document");
                    return;
                }
                q->ioPos += nbytes;
                packet->epos += nbytes;
                packet->esize -= nbytes;
                if (packet->esize == 0) {
                    httpGetPacket(q);
                }
                /*
                    This may split the packet and put back the tail portion ahead of the just putback entity packet.
                 */
                if (!httpWillNextQueueAcceptPacket(q, data)) {
                    httpPutBackPacket(q, data);
                    return;
                }
                httpPutPacketToNext(q, data);
            } else {
                httpGetPacket(q);
            }
        } else {
            /* Don't flow control as the packet is already consuming memory */
            packet = httpGetPacket(q);
            httpPutPacketToNext(q, packet);
        }
        if (!stream->tx->finalizedOutput && !q->first) {
            httpFinalizeOutput(stream);
        }
    }
#endif
}


/*
    The incoming callback is invoked to receive body data
 */
static void incomingFile(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;
    HttpTx      *tx;
    HttpRx      *rx;
    HttpRange   *range;
    MprBuf      *buf;
    MprFile     *file;
    ssize       len;

    stream = q->stream;
    tx = stream->tx;
    rx = stream->rx;
    file = (MprFile*) q->queueData;

    if (packet->flags & HTTP_PACKET_END) {
        /* End of input */
        if (file) {
            mprCloseFile(file);
            q->queueData = 0;
        }
        if (!tx->etag) {
            /* Set the etag for caching in the client */
            mprGetPathInfo(tx->filename, &tx->fileInfo);
            tx->etag = itos(tx->fileInfo.inode + tx->fileInfo.size + tx->fileInfo.mtime);
        }
        httpFinalizeInput(stream);
        if (rx->flags & HTTP_PUT) {
            httpFinalizeOutput(stream);
        }

    } else if (file) {
        buf = packet->content;
        len = mprGetBufLength(buf);
        if (len > 0) {
            range = rx->inputRange;
            if (range && mprSeekFile(file, SEEK_SET, range->start) != range->start) {
                httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot seek to range start to %lld", range->start);

            } else if (mprWriteFile(file, mprGetBufStart(buf), len) != len) {
                httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot PUT to %s", tx->filename);
            }
        }
    }
}


/*
    This is called to setup for a HTTP PUT request. It is called before receiving the post data via incomingFileData
 */
static void handlePutRequest(HttpQueue *q)
{
    HttpStream  *stream;
    HttpTx      *tx;
    MprFile     *file;
    cchar       *path;

    assert(q->queueData == 0);

    stream = q->stream;
    tx = stream->tx;
    assert(tx->filename);
    assert(tx->fileInfo.checked);

    path = tx->filename;
    if (tx->outputRanges) {
        /*
            Open an existing file with fall-back to create
         */
        if ((file = mprOpenFile(path, O_BINARY | O_WRONLY, 0644)) == 0) {
            if ((file = mprOpenFile(path, O_CREAT | O_TRUNC | O_BINARY | O_WRONLY, 0644)) == 0) {
                httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot create the put URI");
                return;
            }
        } else {
            mprSeekFile(file, SEEK_SET, 0);
        }
    } else {
        if ((file = mprOpenFile(path, O_CREAT | O_TRUNC | O_BINARY | O_WRONLY, 0644)) == 0) {
            httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot create the put URI");
            return;
        }
    }
    if (!tx->fileInfo.isReg) {
        httpSetHeaderString(stream, "Location", stream->rx->uri);
    }
    /*
        These are both success returns. 204 means already existed.
     */
    httpSetStatus(stream, tx->fileInfo.isReg ? HTTP_CODE_NO_CONTENT : HTTP_CODE_CREATED);
    q->queueData = (void*) file;
}


static void handleDeleteRequest(HttpQueue *q)
{
    HttpStream  *stream;
    HttpTx      *tx;

    stream = q->stream;
    tx = stream->tx;
    assert(tx->filename);
    assert(tx->fileInfo.checked);

    if (!tx->fileInfo.isReg) {
        httpError(stream, HTTP_CODE_NOT_FOUND, "Document not found");
        return;
    }
    if (mprDeletePath(tx->filename) < 0) {
        httpError(stream, HTTP_CODE_NOT_FOUND, "Cannot remove document");
        return;
    }
    httpSetStatus(stream, HTTP_CODE_NO_CONTENT);
    httpFinalize(stream);
}


PUBLIC int httpHandleDirectory(HttpStream *stream)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    HttpUri     *req;
    cchar       *index, *pathInfo, *path;
    int         next;

    rx = stream->rx;
    tx = stream->tx;
    req = rx->parsedUri;
    route = rx->route;

    /*
        Manage requests for directories
     */
    if (!sends(req->path, "/")) {
        /*
           Append "/" and do an external redirect. Use the original request URI. Use httpFormatUri to preserve query.
         */
        httpRedirect(stream, HTTP_CODE_MOVED_PERMANENTLY,
            httpFormatUri(0, 0, 0, sjoin(req->path, "/", NULL), req->reference, req->query, 0));
        return HTTP_ROUTE_OK;
    }
    if (route->indexes) {
        /*
            Ends with a "/" so do internal redirection to an index file
         */
        path = 0;
        for (ITERATE_ITEMS(route->indexes, index, next)) {
            /*
                Internal directory redirections. Transparently append index. Test indexes in order.
             */
            path = mprJoinPath(tx->filename, index);
            if (mprPathExists(path, R_OK)) {
                break;
            }
            if (route->map && !(tx->flags & HTTP_TX_NO_MAP)) {
                path = httpMapContent(stream, path);
                if (mprPathExists(path, R_OK)) {
                    break;
                }
            }
            path = 0;
        }
        if (path) {
            pathInfo = sjoin(req->path, index, NULL);
            if (httpSetUri(stream, httpFormatUri(req->scheme, req->host, req->port, pathInfo, req->reference,
                    req->query, 0)) < 0) {
                mprLog("error http", 0, "Cannot handle directory \"%s\"", pathInfo);
                return HTTP_ROUTE_REJECT;
            }
            tx->filename = httpMapContent(stream, path);
            mprGetPathInfo(tx->filename, &tx->fileInfo);
            return HTTP_ROUTE_REROUTE;
        }
    }
#if ME_COM_DIR
    /*
        Directory Listing. Test if a directory listing should be rendered. If so, delegate to the dirHandler.
        Must use the netConnector.
     */
    if (httpShouldRenderDirListing(stream)) {
        tx->handler = stream->http->dirHandler;
        tx->connector = stream->http->netConnector;
        return HTTP_ROUTE_OK;
    }
#endif
    return HTTP_ROUTE_OK;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
