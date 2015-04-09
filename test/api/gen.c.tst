/**
    gen.c.tst - General tests for HTTP

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "testme.h"
#include    "http.h"

/*********************************** Locals ***********************************/

int timeout = 10 * 1000 * 1000;

/************************************ Code ************************************/

static void initHttp()
{
    MprSocket   *sp;
    HttpUri     *uri;

    sp = mprCreateSocket(NULL);
    ttrue(sp);

    /*
        Test if we have network connectivity. If not, then skip further tests.
     */

    uri = httpCreateUri(tget("TM_HTTP", ":4100"), 0);
    if (mprConnectSocket(sp, "127.0.0.1", uri->port, 0) < 0) {
        tskip("no internet connection");
        exit(0);
    }
    mprCloseSocket(sp, 0);
}


static void createHttp()
{
    Http        *http;

    http = httpCreate(HTTP_SERVER_SIDE);
    ttrue(http != 0);
    httpDestroy(http);
}


static void basicHttpGet()
{
    Http        *http;
    HttpConn    *conn;
    MprOff      length;
    int         rc, status;

    http = httpCreate(HTTP_CLIENT_SIDE);
    ttrue(http != 0);
    if (tget("TM_DEBUG", 0)) {
        httpStartTracing("stdout:4");
    }
    conn = httpCreateConn(NULL, 0);
    rc = httpConnect(conn, "GET", tget("TM_HTTP", ":4100"), NULL);
    ttrue(rc >= 0);
    if (rc >= 0) {
        httpWait(conn, HTTP_STATE_COMPLETE, timeout);
        status = httpGetStatus(conn);
        ttrue(status == 200 || status == 302);
        if (status != 200 && status != 302) {
            mprLog("http test", 0, "HTTP response status %d", status);
        }
        ttrue(httpGetError(conn) != 0);
        length = httpGetContentLength(conn);
        ttrue(length != 0);
    }
    httpDestroy(http);
}


static void stealSocket()
{
    Http        *http;
    HttpConn    *conn;
    MprSocket   *sp, *prior;
    Socket      fd;
    int         rc, priorState;

    http = httpCreate(HTTP_CLIENT_SIDE);
    ttrue(http != 0);

    /*
        Test httpStealSocket
     */
    conn = httpCreateConn(NULL, 0);
    ttrue(conn != 0);
    rc = httpConnect(conn, "GET", tget("TM_HTTP", ":4100"), NULL);
    ttrue(rc >= 0);
    if (rc >= 0) {
        ttrue(conn->sock != 0);
        ttrue(conn->sock->fd != INVALID_SOCKET);
        prior = conn->sock;
        sp = httpStealSocket(conn);
        ttrue(sp != conn->sock);
        ttrue(prior == conn->sock);

        mprNop(prior);

        ttrue(conn->state == HTTP_STATE_COMPLETE);
        ttrue(sp->fd != INVALID_SOCKET);
        ttrue(conn->sock->fd == INVALID_SOCKET);
        mprCloseSocket(sp, 0);
    }


    /*
        Test httpStealSocketHandle
     */
    conn = httpCreateConn(NULL, 0);
    ttrue(conn != 0);
    rc = httpConnect(conn, "GET", tget("TM_HTTP", ":4100"), NULL);
    ttrue(rc >= 0);
    if (rc >= 0) {
        ttrue(conn->sock != 0);
        ttrue(conn->sock->fd != INVALID_SOCKET);
        priorState = conn->state;
        fd = httpStealSocketHandle(conn);
        ttrue(conn->state == priorState);
        ttrue(fd != INVALID_SOCKET);
        ttrue(conn->sock->fd == INVALID_SOCKET);
        closesocket(fd);
    }
    httpDestroy(http);
}


int main(int argc, char **argv)
{
    mprCreate(argc, argv, 0);
#ifdef BIN
    mprSetModuleSearchPath(BIN);
#endif
    mprVerifySslPeer(NULL, 0);

    if (tget("TM_DEBUG", 0)) {
        mprSetDebugMode(1);
        mprStartLogging("stdout:4", 0);
    }
    initHttp();
    createHttp();
    basicHttpGet();
    stealSocket();
    return 0;
};

/*
    @copy   default
    
    Copyright (c) Embedthis Software. All Rights Reserved.
    Copyright (c) Michael O'Brien. All Rights Reserved.
    
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

