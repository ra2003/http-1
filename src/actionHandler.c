/*
    actionHandler.c -- Action handler

    This handler maps URIs to actions that are C functions that have been registered via httpDefineAction.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/*********************************** Code *************************************/

static void startAction(HttpQueue *q)
{
    HttpConn    *conn;
    HttpAction     action;
    cchar       *name;

    mprLog(5, "Start actionHandler");
    conn = q->conn;
    assure(!conn->error);
    assure(!conn->tx->finalized);

    name = conn->rx->pathInfo;
    if ((action = mprLookupKey(conn->tx->handler->stageData, name)) == 0) {
        mprError("Can't find action: %s", name);
    } else {
        (*action)(conn);
    }
}


PUBLIC void httpDefineAction(cchar *name, HttpAction action)
{
    HttpStage   *stage;

    if ((stage = httpLookupStage(MPR->httpService, "actionHandler")) == 0) {
        mprError("Can't find actionHandler");
        return;
    }
    mprAddKey(stage->stageData, name, action);
}


PUBLIC int httpOpenActionHandler(Http *http)
{
    HttpStage     *stage;

    if ((stage = httpCreateHandler(http, "actionHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    http->actionHandler = stage;
    if ((stage->stageData = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
        return MPR_ERR_MEMORY;
    }
    stage->start = startAction;
    return 0;
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
