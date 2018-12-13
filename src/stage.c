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
    httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);
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


/*
    Default incoming data routine. Simply transfer the data upstream to the next filter or handler.
    This will join incoming packets.
 */
static void incoming(HttpQueue *q, HttpPacket *packet)
{
    assert(q);
    assert(packet);

    if (q->nextQ->put) {
        httpPutPacketToNext(q, packet);
    } else {
        /*
            No upstream put routine to accept the packet. So put on this queues service routine
            for deferred processing. Join packets by default.
         */
        if (httpGetPacketLength(packet) > 0) {
            if (packet->flags & HTTP_PACKET_SOLO) {
                httpPutForService(q, packet, HTTP_DELAY_SERVICE);
            } else {
                httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
            }
        } else {
            /* Zero length packet means eof */
            httpPutForService(q, packet, HTTP_DELAY_SERVICE);
        }
    }
}


PUBLIC void httpDefaultIncoming(HttpQueue *q, HttpPacket *packet)
{
    assert(q);
    assert(packet);
    httpPutForService(q, packet, HTTP_DELAY_SERVICE);
}


PUBLIC HttpStage *httpCreateStage(cchar *name, int flags, MprModule *module)
{
    HttpStage     *stage;

    assert(name && *name);

    if ((stage = httpLookupStage(name)) != 0) {
        if (!(stage->flags & HTTP_STAGE_UNLOADED)) {
            mprLog("error http", 0, "Stage %s already exists", name);
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
    httpAddStage(stage);
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


PUBLIC HttpStage *httpCloneStage(HttpStage *stage)
{
    HttpStage   *clone;

    if ((clone = mprAllocObj(HttpStage, manageStage)) == 0) {
        return 0;
    }
    *clone = *stage;
    return clone;
}


PUBLIC HttpStage *httpCreateHandler(cchar *name, MprModule *module)
{
    return httpCreateStage(name, HTTP_STAGE_HANDLER, module);
}


PUBLIC HttpStage *httpCreateFilter(cchar *name, MprModule *module)
{
    return httpCreateStage(name, HTTP_STAGE_FILTER, module);
}


PUBLIC HttpStage *httpCreateStreamector(cchar *name, MprModule *module)
{
    return httpCreateStage(name, HTTP_STAGE_CONNECTOR, module);
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
