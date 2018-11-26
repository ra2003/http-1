/*
    pipeline.c -- HTTP pipeline processing.
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forward ***********************************/

static int loadQueue(HttpQueue *q, ssize chunkSize);
static bool matchFilter(HttpConn *conn, HttpStage *filter, HttpRoute *route, int dir);
static void openPipeQueues(HttpConn *conn, HttpQueue *qhead);
static void pairQueues(HttpQueue *head1, HttpQueue *head2);

/*********************************** Code *************************************/
/*
    Called after routing the request (httpRouteRequest)
 */
PUBLIC void httpCreatePipeline(HttpConn *conn)
{
    HttpRx      *rx;
    HttpRoute   *route;

    rx = conn->rx;
    route = rx->route;
    if (httpClientConn(conn) && !route) {
        route = conn->http->clientRoute;
    }
    httpCreateRxPipeline(conn, route);
    httpCreateTxPipeline(conn, route);
}


PUBLIC void httpCreateRxPipeline(HttpConn *conn, HttpRoute *route)
{
    Http        *http;
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    HttpStage   *stage, *filter;
    int         next;

    assert(conn);
    assert(route);

    http = conn->http;
    rx = conn->rx;
    tx = conn->tx;

    rx->inputPipeline = mprCreateList(-1, MPR_LIST_STABLE);
    if (route) {
        for (next = 0; (filter = mprGetNextItem(route->inputStages, &next)) != 0; ) {
            if (filter->flags & HTTP_STAGE_INTERNAL) {
                continue;
            }
            if (matchFilter(conn, filter, route, HTTP_STAGE_RX) == HTTP_ROUTE_OK) {
                mprAddItem(rx->inputPipeline, filter);
            }
        }
    }
    mprAddItem(rx->inputPipeline, tx->handler ? tx->handler : conn->http->clientHandler);

    q = conn->rxHead->prevQ;
    for (next = 0; (stage = mprGetNextItem(rx->inputPipeline, &next)) != 0; ) {
        q = httpCreateQueue(conn->net, conn, stage, HTTP_QUEUE_RX, q);
        q->flags |= HTTP_QUEUE_REQUEST;
    }
    conn->readq = q;
    if (httpClientConn(conn)) {
        pairQueues(conn->rxHead, conn->txHead);
        httpOpenQueues(conn);
    }
    if (q->net->protocol < 2) {
        q->net->inputq->conn = conn;
    }
}


PUBLIC void httpCreateTxPipeline(HttpConn *conn, HttpRoute *route)
{
    Http        *http;
    HttpNet     *net;
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    HttpStage   *stage, *filter;
    int         next;

    assert(conn);
    if (!route) {
        if (httpServerConn(conn)) {
            mprLog("error http", 0, "Missing route");
            return;
        }
        route = conn->http->clientRoute;
    }
    http = conn->http;
    net = conn->net;
    rx = conn->rx;
    tx = conn->tx;

    tx->outputPipeline = mprCreateList(-1, MPR_LIST_STABLE);
    if (httpServerConn(conn)) {
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
            if (matchFilter(conn, filter, route, HTTP_STAGE_TX) == HTTP_ROUTE_OK) {
                mprAddItem(tx->outputPipeline, filter);
                tx->flags |= HTTP_TX_HAS_FILTERS;
            }
        }
    }
    /*
        Create the outgoing queues linked from the tx queue head
     */
    q = conn->txHead;
    for (ITERATE_ITEMS(tx->outputPipeline, stage, next)) {
        q = httpCreateQueue(conn->net, conn, stage, HTTP_QUEUE_TX, q);
        q->flags |= HTTP_QUEUE_REQUEST;
    }
    conn->writeq = conn->txHead->nextQ;
    pairQueues(conn->txHead, conn->rxHead);
    pairQueues(conn->rxHead, conn->txHead);
    httpTraceQueues(conn);

    tx->connector = http->netConnector;

    /*
        Open the pipeline stages. This calls the open entrypoints on all stages.
     */
    tx->flags |= HTTP_TX_PIPELINE;
    httpOpenQueues(conn);

    if (conn->error && tx->handler != http->passHandler) {
        tx->handler = http->passHandler;
        httpAssignQueueCallbacks(conn->writeq, tx->handler, HTTP_QUEUE_TX);
    }
    if (net->endpoint) {
        httpTrace(conn->trace, "pipeline", "context",
            "route:'%s', handler:'%s', target:'%s', endpoint:'%s:%d', host:'%s', referrer:'%s', filename:'%s'",
            rx->route->pattern, tx->handler->name, rx->route->targetRule, net->endpoint->ip, net->endpoint->port,
            conn->host->name ? conn->host->name : "default", rx->referrer ? rx->referrer : "",
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


PUBLIC void httpOpenQueues(HttpConn *conn)
{
    openPipeQueues(conn, conn->rxHead);
    openPipeQueues(conn, conn->txHead);
}


static void openPipeQueues(HttpConn *conn, HttpQueue *qhead)
{
    HttpTx      *tx;
    HttpQueue   *q;

    tx = conn->tx;
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        if (q->open && !(q->flags & (HTTP_QUEUE_OPEN_TRIED))) {
            if (q->pair == 0 || !(q->pair->flags & HTTP_QUEUE_OPEN_TRIED)) {
                loadQueue(q, tx->chunkSize);
                if (q->open) {
                    q->flags |= HTTP_QUEUE_OPEN_TRIED;
                    if (q->stage->open(q) == 0) {
                        q->flags |= HTTP_QUEUE_OPENED;
                    } else {
                        if (!conn->error) {
                            httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot open stage %s", q->stage->name);
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
    HttpConn    *conn;
    HttpStage   *stage;
    MprModule   *module;

    stage = q->stage;
    conn = q->conn;
    http = q->conn->http;

    if (chunkSize > 0) {
        q->packetSize = min(q->packetSize, chunkSize);
    }
    if (stage->flags & HTTP_STAGE_UNLOADED && stage->module) {
        module = stage->module;
        module = mprCreateModule(module->name, module->path, module->entry, http);
        if (mprLoadModule(module) < 0) {
            httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot load module %s", module->name);
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
PUBLIC void httpSetFileHandler(HttpConn *conn, cchar *path)
{
    HttpStage   *fp;

    HttpTx      *tx;

    tx = conn->tx;
    if (path && path != tx->filename) {
        httpSetFilename(conn, path, 0);
    }
    tx->entityLength = tx->fileInfo.size;
    fp = tx->handler = HTTP->fileHandler;
    fp->open(conn->writeq);
    fp->start(conn->writeq);
    conn->writeq->service = fp->outgoingService;
    conn->readq->put = fp->incoming;
}


PUBLIC void httpClosePipeline(HttpConn *conn)
{
    HttpQueue   *q, *qhead;

    qhead = conn->txHead;
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        if (q->close && q->flags & HTTP_QUEUE_OPENED) {
            q->flags &= ~HTTP_QUEUE_OPENED;
            q->stage->close(q);
        }
    }
    qhead = conn->rxHead;
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
PUBLIC void httpStartPipeline(HttpConn *conn)
{
    HttpQueue   *qhead, *q, *prevQ, *nextQ;
    HttpTx      *tx;
    HttpRx      *rx;

    tx = conn->tx;
    rx = conn->rx;
    assert(conn->net->endpoint);

    qhead = conn->txHead;
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
        qhead = conn->rxHead;
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
PUBLIC void httpReadyHandler(HttpConn *conn)
{
    HttpQueue   *q;

    q = conn->writeq;
    if (q->stage && q->stage->ready && !(q->flags & HTTP_QUEUE_READY)) {
        q->flags |= HTTP_QUEUE_READY;
        q->stage->ready(q);
    }
}


PUBLIC void httpStartHandler(HttpConn *conn)
{
    HttpQueue   *q;
    HttpTx      *tx;

    tx = conn->tx;
    if (!tx->started) {
        tx->started = 1;
        q = conn->writeq;
        if (q->stage->start && !(q->flags & HTTP_QUEUE_STARTED)) {
            q->flags |= HTTP_QUEUE_STARTED;
            q->stage->start(q);
        }
        if (tx->pendingFinalize) {
            tx->finalizedOutput = 0;
            httpFinalizeOutput(conn);
        }
    }
}


PUBLIC bool httpQueuesNeedService(HttpNet *net)
{
    HttpQueue   *q;

    q = net->serviceq;
    return (q->scheduleNext != q);
}


PUBLIC void httpDiscardData(HttpConn *conn, int dir)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead;

    tx = conn->tx;
    if (tx == 0) {
        return;
    }
    qhead = (dir == HTTP_QUEUE_TX) ? conn->txHead : conn->rxHead;
    httpDiscardQueueData(qhead, 1);
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        httpDiscardQueueData(q, 1);
    }
}


static bool matchFilter(HttpConn *conn, HttpStage *filter, HttpRoute *route, int dir)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (filter->match) {
        return filter->match(conn, route, dir);
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
