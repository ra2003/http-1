/*
    sockFilter.c - Web Sockets filter.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

#if BIT_WEB_SOCKETS
/********************************** Locals ************************************/

#define WSS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/********************************** Forwards **********************************/

static int matchSock(HttpConn *conn, HttpRoute *route, int dir);
static void openSock(HttpQueue *q);
static void outgoingSockService(HttpQueue *q);
static void setSockPrefix(HttpQueue *q, HttpPacket *packet);

/*********************************** Code *************************************/
/* 
   Loadable module initialization
 */
int httpOpenSockFilter(Http *http)
{
    HttpStage     *filter;

    mprLog(5, "Open sock filter");
    if ((filter = httpCreateFilter(http, "sockFilter", HTTP_STAGE_ALL, NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    http->sockFilter = filter;
    filter->match = matchSock; 
    filter->open = openSock; 
    filter->outgoingService = outgoingSockService; 
    return 0;
}

//  MOB
char *sha1(char *s)
{
   return s;
} 

/*
    This is called twice: once for TX and once for RX
 */
static int matchSock(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpRx      *rx;
    char        *value;

    //  MOB - what is called first TX or RX?

    rx = conn->rx;
    if (rx->upgrade && scaselessmatch(rx->upgrade, "websocket")) {
        if (!rx->sockKey) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad Sec-WebSocketKey");
            return HTTP_ROUTE_REJECT;
        }
        value = mprDecode64(sha1(sjoin(rx->sockKey, WSS_MAGIC, NULL)));
        httpSetHeader(conn, "Sec-WebSocket-Accept", value);
        httpSetHeader(conn, "Sec-WebSocket-Protocol", "chat");
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
#if UNUSED
    if (dir & HTTP_STAGE_TX) {
    } else {
    }
#endif
}


static void openSock(HttpQueue *q)
{
    HttpConn    *conn;

    conn = q->conn;
    q->packetSize = min(conn->limits->sockSize, q->max);
}


/*  
    Filter sock headers and leave behind pure data. This is called for socked and unsocked data.
    Socked data format is:
        Sock spec <CRLF>
        Data <CRLF>
        Sock spec (size == 0) <CRLF>
        <CRLF>
    Sock spec is: "HEX_COUNT; sock length DECIMAL_COUNT\r\n". The "; sock length DECIMAL_COUNT is optional.
    As an optimization, use "\r\nSIZE ...\r\n" as the delimiter so that the CRLF after data does not special consideration.
    Achive this by parseHeaders reversing the input start by 2.

    Return number of bytes available to read.
    NOTE: may set rx->eof and return 0 bytes on EOF.
 */
ssize httpFilterSockData(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    HttpRx      *rx;
    MprBuf      *buf;
    ssize       sockSize, nbytes;
    char        *start, *cp;
    int         bad;

    conn = q->conn;
    rx = conn->rx;
    mprAssert(packet);
    buf = packet->content;
    mprAssert(buf);

    switch (rx->sockState) {
    case HTTP_CHUNK_UNCHUNKED:
        nbytes = mprGetBufLength(buf);
        if (conn->http10 && nbytes == 0 && mprIsSocketEof(conn->sock)) {
            rx->eof = 1;
        }
        return (ssize) min(rx->remainingContent, nbytes);

    case HTTP_CHUNK_DATA:
        mprLog(7, "sockFilter: data %d bytes, rx->remainingContent %d", httpGetPacketLength(packet), rx->remainingContent);
        if (rx->remainingContent > 0) {
            return (ssize) min(rx->remainingContent, mprGetBufLength(buf));
        }
        /* End of sock - prep for the next sock */
        rx->remainingContent = HTTP_BUFSIZE;
        rx->sockState = HTTP_CHUNK_START;
        /* Fall through */

    case HTTP_CHUNK_START:
        /*  
            Validate:  "\r\nSIZE.*\r\n"
         */
        if (mprGetBufLength(buf) < 5) {
            return MPR_ERR_NOT_READY;
        }
        start = mprGetBufStart(buf);
        bad = (start[0] != '\r' || start[1] != '\n');
        for (cp = &start[2]; cp < buf->end && *cp != '\n'; cp++) {}
        if (*cp != '\n' && (cp - start) < 80) {
            return MPR_ERR_NOT_READY;
        }
        bad += (cp[-1] != '\r' || cp[0] != '\n');
        if (bad) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad sock specification");
            return 0;
        }
        sockSize = (int) stoiradix(&start[2], 16, NULL);
        if (!isxdigit((int) start[2]) || sockSize < 0) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad sock specification");
            return 0;
        }
        if (sockSize == 0) {
            /*
                Last sock. Consume the final "\r\n".
             */
            if ((cp + 2) >= buf->end) {
                return MPR_ERR_NOT_READY;
            }
            cp += 2;
            bad += (cp[-1] != '\r' || cp[0] != '\n');
            if (bad) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad final sock specification");
                return 0;
            }
        }
        mprAdjustBufStart(buf, (cp - start + 1));
        /* Remaining content is set to the next sock size */
        rx->remainingContent = sockSize;
        if (sockSize == 0) {
            rx->sockState = HTTP_CHUNK_EOF;
            rx->eof = 1;
        } else {
            rx->sockState = HTTP_CHUNK_DATA;
        }
        mprLog(7, "sockFilter: start incoming sock of %d bytes", sockSize);
        return min(sockSize, mprGetBufLength(buf));

    default:
        httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad sock state %d", rx->sockState);
    }
    return 0;
}


static void outgoingSockService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpPacket  *packet;
    HttpTx      *tx;
    cchar       *value;

    conn = q->conn;
    tx = conn->tx;

    if (!(q->flags & HTTP_QUEUE_SERVICED)) {
        /*
            If we don't know the content length (tx->length < 0) and if the last packet is the end packet. Then
            we have all the data. Thus we can determine the actual content length and can bypass the sock handler.
         */
        if (tx->length < 0 && (value = mprLookupKey(tx->headers, "Content-Length")) != 0) {
            tx->length = stoi(value);
        }
        if (tx->length < 0 && tx->sockSize < 0) {
            if (q->last->flags & HTTP_PACKET_END) {
                if (q->count > 0) {
                    tx->length = q->count;
                }
            } else {
                tx->sockSize = min(conn->limits->sockSize, q->max);
            }
        }
        if (tx->flags & HTTP_TX_USE_OWN_HEADERS) {
            tx->sockSize = -1;
        }
    }
    if (tx->sockSize <= 0) {
        httpDefaultOutgoingServiceStage(q);
    } else {
        for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
            if (!(packet->flags & HTTP_PACKET_HEADER)) {
                httpPutBackPacket(q, packet);
                httpJoinPackets(q, tx->sockSize);
                packet = httpGetPacket(q);
                if (httpGetPacketLength(packet) > tx->sockSize) {
                    httpResizePacket(q, packet, tx->sockSize);
                }
            }
            if (!httpWillNextQueueAcceptPacket(q, packet)) {
                httpPutBackPacket(q, packet);
                return;
            }
            if (!(packet->flags & HTTP_PACKET_HEADER)) {
                setSockPrefix(q, packet);
            }
            httpPutPacketToNext(q, packet);
        }
    }
}


static void setSockPrefix(HttpQueue *q, HttpPacket *packet)
{
    if (packet->prefix) {
        return;
    }
    packet->prefix = mprCreateBuf(32, 32);
    /*  
        NOTE: prefixes don't count in the queue length. No need to adjust q->count
     */
    if (httpGetPacketLength(packet)) {
        mprPutFmtToBuf(packet->prefix, "\r\n%x\r\n", httpGetPacketLength(packet));
    } else {
        mprPutStringToBuf(packet->prefix, "\r\n0\r\n\r\n");
    }
}


#endif /* BIT_WEB_SOCKETS */
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
