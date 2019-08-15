/*
    pipeline.c -- HTTP pipeline processing.
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forward ***********************************/

static int loadQueue(HttpQueue *q, ssize chunkSize);
static bool matchFilter(HttpStream *stream, HttpStage *filter, HttpRoute *route, int dir);
static void openPipeQueues(HttpStream *stream, HttpQueue *qhead);
static void pairQueues(HttpQueue *head1, HttpQueue *head2);

/*********************************** Code *************************************/
/*
    Called after routing the request (httpRouteRequest)
 */
PUBLIC void httpCreatePipeline(HttpStream *stream)
{
    HttpRx      *rx;
    HttpRoute   *route;

    rx = stream->rx;
    route = rx->route;
    if (httpClientStream(stream) && !route) {
        route = stream->http->clientRoute;
    }
    httpCreateRxPipeline(stream, route);
    httpCreateTxPipeline(stream, route);
}


PUBLIC void httpCreateRxPipeline(HttpStream *stream, HttpRoute *route)
{
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    HttpStage   *stage, *filter;
    int         next;

    assert(stream);
    assert(route);

    rx = stream->rx;
    tx = stream->tx;

    rx->inputPipeline = mprCreateList(-1, MPR_LIST_STABLE);
    if (route) {
        for (next = 0; (filter = mprGetNextItem(route->inputStages, &next)) != 0; ) {
            if (filter->flags & HTTP_STAGE_INTERNAL) {
                continue;
            }
            if (matchFilter(stream, filter, route, HTTP_STAGE_RX) == HTTP_ROUTE_OK) {
                mprAddItem(rx->inputPipeline, filter);
            }
        }
    }
    mprAddItem(rx->inputPipeline, tx->handler ? tx->handler : stream->http->clientHandler);

    q = stream->rxHead->prevQ;
    for (next = 0; (stage = mprGetNextItem(rx->inputPipeline, &next)) != 0; ) {
        q = httpCreateQueue(stream->net, stream, stage, HTTP_QUEUE_RX, q);
        q->flags |= HTTP_QUEUE_REQUEST;
    }
    stream->readq = q;
    if (httpClientStream(stream)) {
        pairQueues(stream->rxHead, stream->txHead);
        httpOpenQueues(stream);
        
    } else if (!rx->streaming) {
        q->max = stream->limits->rxFormSize;
    }
    if (q->net->protocol < 2) {
        q->net->inputq->stream = stream;
    }
}


PUBLIC void httpCreateTxPipeline(HttpStream *stream, HttpRoute *route)
{
    Http        *http;
    HttpNet     *net;
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    HttpStage   *stage, *filter;
    int         next;

    assert(stream);
    if (!route) {
        if (httpServerStream(stream)) {
            mprLog("error http", 0, "Missing route");
            return;
        }
        route = stream->http->clientRoute;
    }
    http = stream->http;
    net = stream->net;
    rx = stream->rx;
    tx = stream->tx;

    tx->outputPipeline = mprCreateList(-1, MPR_LIST_STABLE);
    if (httpServerStream(stream)) {
        if (tx->handler == 0 || tx->finalized) {
            tx->handler = http->passHandler;
        }
        mprAddItem(tx->outputPipeline, tx->handler);
    }
    if (route->outputStages) {
        for (next = 0; (filter = mprGetNextItem(route->outputStages, &next)) != 0; ) {
            if (filter->flags & HTTP_STAGE_INTERNAL) {
                continue;
            }
            if (matchFilter(stream, filter, route, HTTP_STAGE_TX) == HTTP_ROUTE_OK) {
                mprAddItem(tx->outputPipeline, filter);
                tx->flags |= HTTP_TX_HAS_FILTERS;
            }
        }
    }
    /*
        Create the outgoing queues linked from the tx queue head
     */
    q = stream->txHead;
    for (ITERATE_ITEMS(tx->outputPipeline, stage, next)) {
        q = httpCreateQueue(stream->net, stream, stage, HTTP_QUEUE_TX, q);
        q->flags |= HTTP_QUEUE_REQUEST;
    }
    stream->writeq = stream->txHead->nextQ;
    pairQueues(stream->txHead, stream->rxHead);
    pairQueues(stream->rxHead, stream->txHead);
    httpTraceQueues(stream);

    tx->connector = http->netConnector;

    /*
        Open the pipeline stages. This calls the open entrypoints on all stages.
     */
    tx->flags |= HTTP_TX_PIPELINE;
    httpOpenQueues(stream);

    if (stream->error && tx->handler != http->passHandler) {
        tx->handler = http->passHandler;
        httpAssignQueueCallbacks(stream->writeq, tx->handler, HTTP_QUEUE_TX);
    }
    if (net->endpoint) {
        httpLog(stream->trace, "pipeline", "context",
            "route:'%s', handler:'%s', target:'%s', endpoint:'%s:%d', host:'%s', referrer:'%s', filename:'%s'",
            rx->route->pattern, tx->handler->name, rx->route->targetRule, net->endpoint->ip, net->endpoint->port,
            stream->host->name ? stream->host->name : "default", rx->referrer ? rx->referrer : "",
            tx->filename ? tx->filename : "");
    }
}


static void pairQueues(HttpQueue *head1, HttpQueue *head2)
{
    HttpQueue   *q, *rq;

    for (q = head1->nextQ; q != head1; q = q->nextQ) {
        if (q->pair == 0) {
            for (rq = head2->nextQ; rq != head2; rq = rq->nextQ) {
                if (q->stage == rq->stage) {
                    httpPairQueues(q, rq);
                }
            }
        }
    }
}


PUBLIC void httpOpenQueues(HttpStream *stream)
{
    openPipeQueues(stream, stream->rxHead);
    openPipeQueues(stream, stream->txHead);
}


static void openPipeQueues(HttpStream *stream, HttpQueue *qhead)
{
    HttpTx      *tx;
    HttpQueue   *q;

    tx = stream->tx;
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        if (q->open && !(q->flags & (HTTP_QUEUE_OPEN_TRIED))) {
            if (q->pair == 0 || !(q->pair->flags & HTTP_QUEUE_OPEN_TRIED)) {
                loadQueue(q, tx->chunkSize);
                if (q->open) {
                    q->flags |= HTTP_QUEUE_OPEN_TRIED;
                    if (q->stage->open(q) == 0) {
                        q->flags |= HTTP_QUEUE_OPENED;
                    } else {
                        if (!stream->error) {
                            httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot open stage %s", q->stage->name);
                        }

                    }
                }
            }
        }
    }
}


static int loadQueue(HttpQueue *q, ssize chunkSize)
{
    Http        *http;
    HttpStream  *stream;
    HttpStage   *stage;
    MprModule   *module;

    stage = q->stage;
    stream = q->stream;
    http = q->stream->http;

    if (chunkSize > 0) {
        q->packetSize = min(q->packetSize, chunkSize);
    }
    if (stage->flags & HTTP_STAGE_UNLOADED && stage->module) {
        module = stage->module;
        module = mprCreateModule(module->name, module->path, module->entry, http);
        if (mprLoadModule(module) < 0) {
            httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot load module %s", module->name);
            return MPR_ERR_CANT_READ;
        }
        stage->module = module;
    }
    if (stage->module) {
        stage->module->lastActivity = http->now;
    }
    return 0;
}


/*
    Set the fileHandler as the selected handler for the request
    Called by ESP to render a document.
 */
PUBLIC void httpSetFileHandler(HttpStream *stream, cchar *path)
{
    HttpStage   *fp;

    HttpTx      *tx;

    tx = stream->tx;
    if (path && path != tx->filename) {
        httpSetFilename(stream, path, 0);
    }
    tx->entityLength = tx->fileInfo.size;
    fp = tx->handler = HTTP->fileHandler;
    fp->open(stream->writeq);
    fp->start(stream->writeq);
    stream->writeq->service = fp->outgoingService;
    stream->readq->put = fp->incoming;
}


PUBLIC void httpClosePipeline(HttpStream *stream)
{
    HttpQueue   *q, *qhead;

    qhead = stream->txHead;
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        if (q->close && q->flags & HTTP_QUEUE_OPENED) {
            q->flags &= ~HTTP_QUEUE_OPENED;
            q->stage->close(q);
        }
    }
    qhead = stream->rxHead;
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        if (q->close && q->flags & HTTP_QUEUE_OPENED) {
            q->flags &= ~HTTP_QUEUE_OPENED;
            q->stage->close(q);
        }
    }
}


/*
    Start all queues, but do not start the handler
 */
PUBLIC void httpStartPipeline(HttpStream *stream)
{
    HttpQueue   *qhead, *q, *prevQ, *nextQ;
    HttpRx      *rx;

    rx = stream->rx;
    assert(stream->net->endpoint);

    qhead = stream->txHead;
    for (q = qhead->prevQ; q != qhead; q = prevQ) {
        prevQ = q->prevQ;
        if (q->start && !(q->flags & HTTP_QUEUE_STARTED)) {
            if (!(q->stage->flags & HTTP_STAGE_HANDLER)) {
                q->flags |= HTTP_QUEUE_STARTED;
                q->stage->start(q);
            }
        }
    }

    if (rx->needInputPipeline) {
        qhead = stream->rxHead;
        for (q = qhead->nextQ; q != qhead; q = nextQ) {
            nextQ = q->nextQ;
            if (q->start && !(q->flags & HTTP_QUEUE_STARTED)) {
                /* Don't start if tx side already started */
                if (q->pair == 0 || !(q->pair->flags & HTTP_QUEUE_STARTED)) {
                    if (!(q->stage->flags & HTTP_STAGE_HANDLER)) {
                        q->flags |= HTTP_QUEUE_STARTED;
                        q->stage->start(q);
                    }
                }
            }
        }
    }
}


/*
    Called when all input data has been received
 */
PUBLIC void httpReadyHandler(HttpStream *stream)
{
    HttpQueue   *q;

    q = stream->writeq;
    if (q->stage && q->stage->ready && !(q->flags & HTTP_QUEUE_READY)) {
        q->flags |= HTTP_QUEUE_READY;
        q->stage->ready(q);
    }
}


PUBLIC void httpStartHandler(HttpStream *stream)
{
    HttpQueue   *q;
    HttpTx      *tx;

    tx = stream->tx;
    if (!tx->started) {
        tx->started = 1;
        q = stream->writeq;
        if (q->stage->start && !(q->flags & HTTP_QUEUE_STARTED)) {
            q->flags |= HTTP_QUEUE_STARTED;
            q->stage->start(q);
        }
        if (tx->pendingFinalize) {
            tx->finalizedOutput = 0;
            httpFinalizeOutput(stream);
        }
    }
}


PUBLIC bool httpQueuesNeedService(HttpNet *net)
{
    HttpQueue   *q;

    q = net->serviceq;
    return (q->scheduleNext != q);
}


PUBLIC void httpDiscardData(HttpStream *stream, int dir)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead;

    tx = stream->tx;
    if (tx == 0) {
        return;
    }
    qhead = (dir == HTTP_QUEUE_TX) ? stream->txHead : stream->rxHead;
    httpDiscardQueueData(qhead, 1);
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        httpDiscardQueueData(q, 1);
    }
}


static bool matchFilter(HttpStream *stream, HttpStage *filter, HttpRoute *route, int dir)
{
    HttpTx      *tx;

    tx = stream->tx;
    if (filter->match) {
        return filter->match(stream, route, dir);
    }
    if (filter->extensions && tx->ext) {
        return mprLookupKey(filter->extensions, tx->ext) != 0;
    }
    return 1;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
