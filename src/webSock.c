/*
    webSock.c - WebSockets support

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

#if BIT_WEB_SOCKETS
/********************************** Locals ************************************/
/*
    Input states
 */
#define WS_BEGIN       0
#define WS_EXT_DATA    1                /* Unused */
#define WS_MSG         2
#define WS_CLOSED      3

/*
    Web Sockets message codes
 */
#define OP_CONT     0x0         /* Continuation */
#define OP_TEXT     0x1         /* Text data */
#define OP_BINARY   0x2         /* Binary data */
#define OP_CONTROL  0x8         /* Start of control codes */
#define OP_CLOSE    0x8         /* Close connection */
#define OP_PING     0x9         /* Ping request */
#define OP_PONG     0xA

static int opcodes[8] = {
    OP_CLOSE, OP_TEXT, OP_BINARY, OP_PING, OP_PONG, OP_CLOSE, OP_CLOSE, OP_CLOSE,
};
static char *codetxt[16] = {
    "continuation", "text", "binary", "reserved", "reserved", "reserved", "reserved", "reserved",
    "close", "ping", "pong", "reserved", "reserved", "reserved", "reserved", "reserved",
};

/*
    Frame format

     Byte 0          Byte 1          Byte 2          Byte 3
     0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
    +-+-+-+-+-------+-+-------------+-------------------------------+
    |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
    |I|S|S|S|  (4)  |A|     (7)     |             (16/63)           |
    |N|V|V|V|       |S|             |   (if payload len==126/127)   |
    | |1|2|3|       |K|             |                               |
    +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
    |     Extended payload length continued, if payload len == 127  |
    + - - - - - - - - - - - - - - - +-------------------------------+
    |                               |Masking-key, if MASK set to 1  |
    +-------------------------------+-------------------------------+
    | Masking-key (continued)       |          Payload Data         |
    +-------------------------------- - - - - - - - - - - - - - - - +
    :                     Payload Data continued ...                :
    + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
    |                     Payload Data continued ...                |
    +---------------------------------------------------------------+

    Single message has 
        fin == 1
    Fragmented message has
        fin == 0, opcode != 0
        fin == 0, opcode == 0
        fin == 1, opcode == 0

    Common first byte codes:
        0x9B    Fin | /SET

    NOTE: control frames (opcode >= 8) can be sent between fragmented frames
 */
#define GET_FIN(v)              (((v) >> 7) & 0x1)          /* Final fragment */
#define GET_RSV(v)              (((v) >> 4) & 0x7)          /* Reserved (used for extensions) */
#define GET_CODE(v)             ((v) & 0xf)                 /* Packet opcode */
#define GET_MASK(v)             (((v) >> 7) & 0x1)          /* True if dataMask in frame (client send) */
#define GET_LEN(v)              ((v) & 0x7f)                /* Low order 7 bits of length */

#define SET_FIN(v)              (((v) & 0x1) << 7)
#define SET_MASK(v)             (((v) & 0x1) << 7)
#define SET_CODE(v)             ((v) & 0xf)                                                       
#define SET_LEN(len, n)         ((uchar)(((len) >> ((n) * 8)) & 0xff))

/********************************** Forwards **********************************/

static void closeWebSock(HttpQueue *q);
static void incomingWebSockData(HttpQueue *q, HttpPacket *packet);
static int matchWebSock(HttpConn *conn, HttpRoute *route, int dir);
static void openWebSock(HttpQueue *q);
static void outgoingWebSockService(HttpQueue *q);
static void notifyWebSock(HttpConn *conn, int event, int status);
static bool validUTF8(cchar *str, ssize len);
static void webSockPing(HttpConn *conn);

/*********************************** Code *************************************/
/* 
   Loadable module initialization
 */
int httpOpenWebSockFilter(Http *http)
{
    HttpStage     *filter;

    mprAssert(http);

    mprLog(5, "Open WebSock filter");
    if ((filter = httpCreateFilter(http, "webSocketFilter", HTTP_STAGE_ALL, NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    http->webSocketFilter = filter;
    filter->match = matchWebSock; 
    filter->open = openWebSock; 
    filter->close = closeWebSock; 
    filter->outgoingService = outgoingWebSockService; 
    filter->incoming = incomingWebSockData; 
    return 0;
}


/*
    Match if the filter is required for this request. This is called twice: once for TX and once for RX.
 */
static int matchWebSock(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpRx      *rx;
    HttpTx      *tx;
    char        *kind, *tok;

    mprAssert(conn);
    mprAssert(route);

    rx = conn->rx;
    tx = conn->tx;
    mprAssert(rx);
    mprAssert(tx);

    if (!conn->endpoint && tx->parsedUri && tx->parsedUri->wss) {
        /* ws:// URI. Client web sockets */
        return HTTP_ROUTE_OK;
    }
    /*
        Deliberately not checking Origin as it offers illusory security
     */
    if (!smatch(rx->method, "GET") || !rx->hostHeader || !rx->upgrade || !rx->webSockKey || !rx->webSockVersion) {
        return HTTP_ROUTE_REJECT;
    }
    if (dir & HTTP_STAGE_RX) {
        if (rx->upgrade && scaselessmatch(rx->upgrade, "websocket")) {
            if (!rx->webSockKey) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad Sec-WebSocket-Key");
                return HTTP_ROUTE_REJECT;
            }
            if (rx->webSockVersion < WEB_SOCKETS_VERSION) {
                httpSetHeader(conn, "Sec-WebSocket-Version", "%d", WEB_SOCKETS_VERSION);
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Version");
                return HTTP_ROUTE_REJECT;
            }
#if FUTURE && MOB
            if (route->callback) {
                return (route->callback)(conn);
            }
#endif
            /* Just select the first protocol */
            if (route->webSocketsProtocol) {
                for (kind = stok(sclone(rx->webSockProtocols), " \t,", &tok); kind; kind = stok(NULL, " \t,", &tok)) {
                    if (smatch(route->webSocketsProtocol, kind)) {
                        break;
                    }
                }
                if (!kind) {
                    httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Protocol");
                    return HTTP_ROUTE_REJECT;
                }
                conn->rx->subProtocol = sclone(kind);
            } else {
                /* Just pick the first protocol */
                conn->rx->subProtocol = stok(sclone(rx->webSockProtocols), " ,", NULL);
            }
            httpSetStatus(conn, HTTP_CODE_SWITCHING);
            httpSetHeader(conn, "Connection", "Upgrade");
            httpSetHeader(conn, "Upgrade", "WebSocket");
            httpSetHeader(conn, "Sec-WebSocket-Accept", mprGetSHABase64(sjoin(rx->webSockKey, WEB_SOCKETS_MAGIC, NULL)));
            httpSetHeader(conn, "Sec-WebSocket-Protocol", conn->rx->subProtocol);
            httpSetHeader(conn, "X-Request-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
            httpSetHeader(conn, "X-Inactivity-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);

            if (route->webSocketsPingPeriod) {
                rx->pingEvent = mprCreateEvent(conn->dispatcher, "webSocket", route->webSocketsPingPeriod, 
                    webSockPing, conn, MPR_EVENT_CONTINUOUS);
            }
            conn->keepAliveCount = -1;
            conn->upgraded = 1;
            rx->eof = 0;
            rx->remainingContent = MAXINT;
            return HTTP_ROUTE_OK;
        }
    } else if (conn->upgraded) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


static void webSockPing(HttpConn *conn)
{
    mprAssert(conn->rx);
    httpSendBlock(conn, WS_MSG_PING, NULL, 0);
}


static void webSockTimeout(HttpConn *conn)
{
    httpSendClose(conn, WS_STATUS_POLICY_VIOLATION, "Request timeout");
}


static void openWebSock(HttpQueue *q)
{
    HttpConn    *conn;
    HttpPacket  *packet;

    mprAssert(q);
    mprLog(5, "websock: Open WebSocket filter");

    conn = q->conn;
    q->packetSize = min(conn->limits->stageBufferSize, q->max);
    conn->rx->closeStatus = WS_STATUS_NO_STATUS;
    conn->timeoutCallback = webSockTimeout;

//  ZZ MOB - Check count of web sockets
    if ((packet = httpGetPacket(conn->writeq)) != 0) {
        mprAssert(packet->flags & HTTP_PACKET_HEADER);
        //  MOB - or should this be httpPutPacketToNext
        httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);
    }
    conn->responded = 0;
}


static void closeWebSock(HttpQueue *q)
{
    HttpRx  *rx;

    rx = q->conn->rx;
    if (rx && rx->pingEvent) {
        mprRemoveEvent(rx->pingEvent);
        rx->pingEvent = 0;
    }
}


static int processPacket(HttpQueue *q, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpConn    *conn;
    MprBuf      *content;
    char        *cp;

    conn = q->conn;
    rx = conn->rx;
    content = packet->content;
    mprAssert(content);
    mprLog(2, "websock: Process packet \"%s\", data length %d", codetxt[rx->opcode & 0xf], mprGetBufLength(content));

    switch (rx->opcode) {
    case OP_BINARY:
    case OP_TEXT:
        if (rx->maskOffset >= 0) {
            if (rx->opcode == OP_TEXT) {
                //  MOB - validate UTF8?
                for (cp = content->start; cp < content->end; cp++) {
                    *cp = *cp ^ rx->dataMask[rx->maskOffset++ & 0x3];
                }
            } else {
                for (cp = content->start; cp < content->end; cp++) {
                    *cp = *cp ^ rx->dataMask[rx->maskOffset++ & 0x3];
                }
            }
        } 
        if (rx->opcode == OP_TEXT && !validUTF8(content->start, mprGetBufLength(content))) {
            if (!rx->route->ignoreEncodingErrors) {
                mprError("websock: Text packet has invalid UTF8");
                return WS_STATUS_INVALID_UTF8;
            }
        }
        packet->type = (rx->opcode == OP_TEXT) ? WS_MSG_TEXT : WS_MSG_BINARY;
        if (rx->opcode == OP_TEXT) {
            mprLog(2, "websock: Text packet \"%s\"", content->start);
        }
        if (rx->finalFrame) {
            /* Preserve packet boundaries */
            packet->flags |= HTTP_PACKET_SOLO;
            httpPutPacketToNext(q, packet);
            httpServiceQueues(q->conn);
            rx->currentPacket = 0;
            notifyWebSock(conn, HTTP_NOTIFY_READABLE, packet->type);
        }
        rx->state = WS_BEGIN;
        return 0;

    case OP_CLOSE:
        cp = content->start;
        if (httpGetPacketLength(packet) >= 2) {
            rx->closeStatus = ((uchar) cp[0]) << 8 | (uchar) cp[1];
            if (httpGetPacketLength(packet) >= 4) {
                mprAddNullToBuf(content);
                rx->closeReason = sclone(&content->start[2]);
            }
        }
        mprLog(2, "websock: close status %d, reason %s, already closing %d", rx->closeStatus, rx->closeReason, rx->closing);
        if (rx->closing) {
            httpDisconnect(conn);
        } else {
            /* Acknowledge the close. Echo the received status */
            //  MOB - echo close doesn't seem to work on Chrome
            httpSendClose(conn, WS_STATUS_OK, NULL);
            rx->eof = 1;
        }
        /* Advance from the content state */
        httpSetState(conn, HTTP_STATE_READY);
        rx->state = WS_CLOSED;
        return 0;

    case OP_PING:
        httpSendBlock(conn, WS_MSG_PONG, mprGetBufStart(content), mprGetBufLength(content));
        rx->state = WS_BEGIN;
        return 0;

    case OP_PONG:
        /* Do nothing */
        rx->state = WS_BEGIN;
        return 0;

    default:
        mprError("websock: Bad frame type %d", rx->opcode);
        break;
    }
    /* Should not get here */
    rx->state = WS_CLOSED;
    return WS_STATUS_PROTOCOL_ERROR;
}


static void incomingWebSockData(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpPacket  *tail;
    HttpLimits  *limits;
    char        *fp;
    ssize       len;
    int         i, error, mask, lenBytes;

    conn = q->conn;
    rx = conn->rx;
    limits = conn->limits;
    mprAssert(packet);
    mprLog(5, "websock: incoming data. Packet type: %d", packet->type);

    if (packet->flags & HTTP_PACKET_END) {
        /* EOF packet means the socket has been abortively closed */
#if UNUSED
        httpSendClose(conn, WS_STATUS_OK, NULL);
#endif
        rx->closing = 1;
        rx->state = WS_CLOSED;
        rx->closeStatus = WS_STATUS_COMMS_ERROR;
    }
    while (1) {
        mprLog(5, "websock: incoming state %d", rx->state);
        error = 0;
        switch (rx->state) {
        case WS_CLOSED:
            mprLog(5, "websock: incoming closed. Finalizing");
            notifyWebSock(conn, HTTP_NOTIFY_CLOSED, rx->closeStatus);
            /* Finalize for safety. The handler/callback should have done this above */
            httpFinalize(conn);
            return;

        case WS_BEGIN:
            if (httpGetPacketLength(packet) < 2) {
                /* Need more data */
                return;
            }
            fp = packet->content->start;
            if (GET_RSV(*fp) != 0) {
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            rx->finalFrame = GET_FIN(*fp);
            rx->opcode = GET_CODE(*fp);
            fp++;
            len = GET_LEN(*fp);
            mask = GET_MASK(*fp);
            lenBytes = 1;
            if (len == 126) {
                lenBytes += 2;
            } else if (len == 127) {
                lenBytes += 8;
            }
            if (httpGetPacketLength(packet) < (lenBytes + (mask * 4))) {
                /* Return if we don't have the required packet control fields */
                return;
            }
            fp++;
            if (rx->opcode) {
                if (rx->opcode < OP_CONTROL) {
                    rx->messageCode = rx->opcode;
                } else {
                    /* Control frame, must not be fragmented */
                    if (!rx->finalFrame) {
                        error = WS_STATUS_PROTOCOL_ERROR;
                        break;
                    }
                }
            }
            while (--lenBytes > 0) {
                len <<= 8;
                len += (uchar) *fp++;
            }
            rx->frameLength = len;
            rx->state = WS_MSG;
            rx->maskOffset = mask ? 0 : -1;
            if (mask) {
                for (i = 0; i < 4; i++) {
                    rx->dataMask[i] = *fp++;
                }
            }
            mprAssert(packet->content);
            mprAdjustBufStart(packet->content, fp - packet->content->start);
            rx->state = WS_MSG;
            mprLog(2, "websock: Begin new packet \"%s\", fin %d, mask %d, length %d", codetxt[rx->opcode & 0xf],
                rx->finalFrame, mask, len);
            break;

#if UNUSED && KEEP
        case WS_EXT_DATA:
            mprAssert(packet);
            mprLog(2, "websock: EXT DATA - RESERVED");
            rx->state = WS_MSG;
            break;
#endif

        case WS_MSG:
            mprAssert(packet);
            len = min(httpGetPacketLength(packet), rx->frameLength);
            if (len >= conn->limits->webSocketsFrameSize) {
                mprError("WebSocket frame is too large %d/%d", len, limits->webSocketsFrameSize);
                packet = rx->currentPacket = 0;
                error = WS_STATUS_FRAME_TOO_LARGE;

            } else if ((len + httpGetPacketLength(rx->currentPacket)) > conn->limits->webSocketsMessageSize) {
                error = WS_STATUS_MESSAGE_TOO_LARGE;
                mprError("WebSocket message is too large %d/%d", len, limits->webSocketsMessageSize);
                packet = rx->currentPacket = 0;
                error = WS_STATUS_MESSAGE_TOO_LARGE;

            } else {
                if (rx->currentPacket) {
                    mprLog(2, "websock: Joining data packet %d/%d", httpGetPacketLength(rx->currentPacket),
                        httpGetPacketLength(packet));
                    if (rx->currentPacket->type != packet->type) {
                        mprError("WebSocket has frames of different types: %d and %d", 
                            rx->currentPacket->type, packet->type);
                        rx->currentPacket = packet = 0;
                        packet = rx->currentPacket = 0;
                        error = WS_STATUS_UNSUPPORTED_TYPE;
                    } else {
                        httpJoinPacket(rx->currentPacket, packet);
                        packet = rx->currentPacket;
                    }
                } else {
                    rx->currentPacket = packet;
                }
            }
            /*
                Split packet if it contains data for the next frame. Do this even if discarding a frame.
             */
            tail = 0;
            if (httpGetPacketLength(packet) > rx->frameLength) {
                if ((tail = httpSplitPacket(packet, rx->frameLength)) != 0) {
                    httpPutBackPacket(q, tail);
                    mprLog(2, "websock: Split data packet, %d/%d", rx->frameLength, httpGetPacketLength(tail));
                }
            }
            /*
                Must discard message if closing.       
                The WS spec is broken in that packet boundaries cannot be guaranteed.
                We try to preserve message boundaries, but if the packet is too large, we sent it to the user regardless.
             */
            if (!error && !rx->closing) {
                len = httpGetPacketLength(packet);
                if ((rx->finalFrame && len == rx->frameLength) || len >= limits->webSocketsPacketSize) {
                    if (len >= limits->webSocketsPacketSize) {
                        mprLog(4, "websock: Message packet size exceeds limit %d/%d", len, limits->webSocketsPacketSize); 
                    }
                    error = processPacket(q, packet);
                }
            }
            packet = tail;
        }
        if (error) {
            mprLog(0, "websock: WebSockets error Status %d", error);
            httpSendClose(conn, error, NULL);
            rx->state = WS_CLOSED;
            notifyWebSock(conn, HTTP_NOTIFY_ERROR, error);
        }
    }
}


/*
    Send a text message. Caller must submit valid UTF8.
 */
ssize httpSend(HttpConn *conn, cchar *fmt, ...)
{
    va_list     args;
    char        *buf;

    va_start(args, fmt);
    buf = sfmtv(fmt, args);
    va_end(args);
    return httpSendBlock(conn, WS_MSG_TEXT, buf, slen(buf));
}


ssize httpSendBlock(HttpConn *conn, int type, cchar *buf, ssize len)
{
    HttpPacket  *packet;

    mprLog(2, "websock: Send message \"%s\", len %d", codetxt[type & 0xf], len);
    if ((packet = httpCreateDataPacket(len)) == 0) {
        return MPR_ERR_MEMORY;
    }
    packet->type = type;
    if (len < 0) {
        len = slen(buf);
    }
    if (len > 0) {
        if (mprPutBlockToBuf(packet->content, buf, len) != len) {
            return MPR_ERR_MEMORY;
        }
    }
    httpPutForService(conn->writeq, packet, HTTP_SCHEDULE_QUEUE);
    httpServiceQueues(conn);
    return len;
}


void httpSendClose(HttpConn *conn, int status, cchar *reason)
{
    HttpPacket  *packet;
    HttpRx      *rx;
    char        *msg;
    ssize       len;

    rx = conn->rx;
    if (rx->closing) {
        return;
    }
    /* 
        NOTE: this sets an expectation that the close message will be acknowledged by the peer, but we don't change state 
     */
    rx->closing = 1;
    if ((packet = httpCreateDataPacket(2)) == 0) {
        return;
    }
    packet->type = WS_MSG_CLOSE;
    len = 2 + (reason ? (slen(reason) + 1) : 0);
    if ((msg = mprAlloc(len)) == 0) {
        return;
    }
    msg[0] = (status >> 8) & 0xff;
    msg[1] = status & 0xff;
    if (reason) {
        scopy(&msg[2], len - 2, reason);
    }
    mprLog(5, "websock: sendClose");
    httpSendBlock(conn, WS_MSG_CLOSE, msg, len);
}


static void outgoingWebSockService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpPacket  *packet;
    char        *ep, *fp, *prefix, dataMask[4];
    ssize       len;
    int         i, mask, code;

    conn = q->conn;
    mprLog(5, "websock: outgoing service");

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!(packet->flags & (HTTP_PACKET_END | HTTP_PACKET_HEADER))) {
            httpResizePacket(q, packet, conn->limits->stageBufferSize);
            if (!httpWillNextQueueAcceptPacket(q, packet)) {
                httpPutBackPacket(q, packet);
                return;
            }
            len = httpGetPacketLength(packet);
            packet->prefix = mprCreateBuf(16, 16);
            code = opcodes[packet->type & 0x7];
            prefix = packet->prefix->start;
            mask = conn->endpoint ? 0 : 1;
            *prefix++ = SET_FIN(1) | SET_CODE(code);
            if (len <= 125) {
                *prefix++ = SET_MASK(mask) | SET_LEN(len, 0);
            } else if (len <= 65535) {
                *prefix++ = SET_MASK(mask) | 126;
                *prefix++ = SET_LEN(len, 1);
                *prefix++ = SET_LEN(len, 0);
            } else {
                *prefix++ = SET_MASK(mask) | 127;
                for (i = 7; i >= 0; i--) {
                    *prefix++ = SET_LEN(len, i);
                }
            }
            if (!conn->endpoint) {
                mprGetRandomBytes(dataMask, sizeof(dataMask), 0);
                for (i = 0; i < 4; i++) {
                    *prefix++ = dataMask[i];
                }
                fp = packet->content->start;
                ep = packet->content->end;
                for (i = 0; fp < ep; fp++) {
                    *fp = *fp ^ dataMask[i++ & 0x3];
                }
            }
            *prefix = '\0';
            mprAdjustBufEnd(packet->prefix, prefix - packet->prefix->start);
            mprLog(5, "websock: outgoing service, data packet len %d", httpGetPacketLength(packet));
        }
        httpPutPacketToNext(q, packet);
    }
}


//  MOB - naming
char *httpGetWebSockProtocol(HttpConn *conn)
{
    return conn->rx->subProtocol;
}


char *httpGetCloseReason(HttpConn *conn)
{
    return conn->rx->closeReason;
}


bool httpWasOrderlyClose(HttpConn *conn)
{
    return conn->rx->closeStatus != WS_STATUS_COMMS_ERROR;
}


void httpSetWebSocketProtocols(HttpConn *conn, cchar *protocols)
{
    conn->protocols = sclone(protocols);
}


HttpWebSocketNotifier httpSetWebSocketNotifier(HttpConn *conn, HttpWebSocketNotifier notifier)
{
    HttpWebSocketNotifier prior;

    prior = conn->webSocketNotifier;
    conn->webSocketNotifier = notifier;
    return prior;
}


static void notifyWebSock(HttpConn *conn, int event, int status)
{
    if (conn->webSocketNotifier) {
        (conn->webSocketNotifier)(conn, event, status);
    }
}


static bool validUTF8(cchar *str, ssize len)
{
    cuchar      *cp, *end;
    int         nbytes, i;
  
    cp = (cuchar*) str;
    end = (cuchar*) &str[len];
    for (; cp < end && *cp; cp += nbytes) {
        if (!(*cp & 0x80)) {
            nbytes = 1;
        } else if ((*cp & 0xc0) == 0x80) {
            return 0;
        } else if ((*cp & 0xe0) == 0xc0) {
            nbytes = 2;
        } else if ((*cp & 0xf0) == 0xe0) {
            nbytes = 3;
        } else if ((*cp & 0xf8) == 0xf0) {
            nbytes = 4;
        } else if ((*cp & 0xfc) == 0xf8) {
            nbytes = 5;
        } else if ((*cp & 0xfe) == 0xfc) {
            nbytes = 6;
        } else {
            nbytes = 1;
        }
        for (i = 1; i < nbytes; i++) {
            if ((cp[i] & 0xc0) != 0x80) {
                return 0;
            }
        }
        mprAssert(nbytes >= 1);
    } 
    return 1;
}


/*
    Upgrade a client socket to use Web Sockets
 */
int httpWebSockUpgrade(HttpConn *conn)
{
    char    num[16];

    mprLog(2, "websock: Upgrade socket");
    httpSetStatus(conn, HTTP_CODE_SWITCHING);
    httpSetHeader(conn, "Upgrade", "websocket");
    httpSetHeader(conn, "Connection", "Upgrade");
    mprGetRandomBytes(num, sizeof(num), 0);
    conn->tx->webSockKey = mprEncode64Block(num, sizeof(num));
    httpSetHeader(conn, "Sec-WebSocket-Key", conn->tx->webSockKey);
    httpSetHeader(conn, "Sec-WebSocket-Protocol", conn->protocols ? conn->protocols : "chat");
    httpSetHeader(conn, "Sec-WebSocket-Version", "13");
#if UNUSED
    //  MOB what does origin really mean
    httpSetHeader(conn, "Origin", conn->host->name);
    httpSetHeader(conn, "X-Request-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
    httpSetHeader(conn, "X-Inactivity-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
#endif
    conn->upgraded = 1;
    conn->keepAliveCount = -1;
    conn->rx->remainingContent = MAXINT;
    return 0;
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
