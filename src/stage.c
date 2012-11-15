/*
    stage.c -- Stages are the building blocks of the Http request pipeline.

    Stages support the extensible and modular processing of HTTP requests. Handlers are a kind of stage that are the 
    first line processing of a request. Connectors are the last stage in a chain to send/receive data over a network.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************* Forwards ***********************************/

static void manageStage(HttpStage *stage, int flags);

/*********************************** Code *************************************/
/*  
    Put packets on the service queue.
 */
static void outgoing(HttpQueue *q, HttpPacket *packet)
{
    int     enableService;

    /*  
        Handlers service routines must only be auto-enabled if in the running state.
     */
    enableService = !(q->stage->flags & HTTP_STAGE_HANDLER) || (q->conn->state >= HTTP_STATE_READY) ? 1 : 0;
    httpPutForService(q, packet, enableService);
}


/*  
    Default incoming data routine.  Simply transfer the data upstream to the next filter or handler.
 */
static void incoming(HttpQueue *q, HttpPacket *packet)
{
    assure(q);
    VERIFY_QUEUE(q);
    assure(packet);
    
    if (q->nextQ->put) {
        httpPutPacketToNext(q, packet);
    } else {
        /* This queue is the last queue in the pipeline */
        if (httpGetPacketLength(packet) > 0) {
            if (packet->flags & HTTP_PACKET_SOLO) {
                httpPutForService(q, packet, HTTP_DELAY_SERVICE);
            } else {
                httpJoinPacketForService(q, packet, 0);
            }
        } else {
            /* Zero length packet means eof */
            httpPutForService(q, packet, HTTP_DELAY_SERVICE);
        }
        HTTP_NOTIFY(q->conn, HTTP_EVENT_READABLE, 0);
    }
}


PUBLIC void httpDefaultOutgoingServiceStage(HttpQueue *q)
{
    HttpPacket    *packet;

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!httpWillNextQueueAcceptPacket(q, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        httpPutPacketToNext(q, packet);
    }
}


PUBLIC HttpStage *httpCreateStage(Http *http, cchar *name, int flags, MprModule *module)
{
    HttpStage     *stage;

    assure(http);
    assure(name && *name);

    if ((stage = httpLookupStage(http, name)) != 0) {
        if (!(stage->flags & HTTP_STAGE_UNLOADED)) {
            mprError("Stage %s already exists", name);
            return 0;
        }
    } else if ((stage = mprAllocObj(HttpStage, manageStage)) == 0) {
        return 0;
    }
    stage->flags = flags;
    stage->name = sclone(name);
    stage->incoming = incoming;
    stage->outgoing = outgoing;
    stage->outgoingService = httpDefaultOutgoingServiceStage;
    stage->module = module;
    httpAddStage(http, stage);
    return stage;
}


static void manageStage(HttpStage *stage, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(stage->name);
        mprMark(stage->path);
        mprMark(stage->stageData);
        mprMark(stage->module);
        mprMark(stage->extensions);
    }
}


PUBLIC HttpStage *httpCloneStage(Http *http, HttpStage *stage)
{
    HttpStage   *clone;

    if ((clone = mprAllocObj(HttpStage, manageStage)) == 0) {
        return 0;
    }
    *clone = *stage;
    return clone;
}


PUBLIC HttpStage *httpCreateHandler(Http *http, cchar *name, MprModule *module)
{
    return httpCreateStage(http, name, HTTP_STAGE_HANDLER, module);
}


PUBLIC HttpStage *httpCreateFilter(Http *http, cchar *name, MprModule *module)
{
    return httpCreateStage(http, name, HTTP_STAGE_FILTER, module);
}


PUBLIC HttpStage *httpCreateConnector(Http *http, cchar *name, MprModule *module)
{
    return httpCreateStage(http, name, HTTP_STAGE_CONNECTOR, module);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

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
