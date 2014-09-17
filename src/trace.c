/*
    trace.c -- Trace data
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.

    Event type default labels:

        request: 1
        result:  2
        context: 3
        form:    4
        body:    5
        debug:   5
 */

/********************************* Includes ***********************************/

#include    "http.h"

/*********************************** Code *************************************/

static void manageTrace(HttpTrace *trace, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(trace->file);
        mprMark(trace->format);
        mprMark(trace->lastTime);
        mprMark(trace->buf);
        mprMark(trace->path);
        mprMark(trace->events);
        mprMark(trace->mutex);
    }
}

/*
    Parent may be null
 */
PUBLIC HttpTrace *httpCreateTrace(HttpTrace *parent)
{
    HttpTrace   *trace;

    if ((trace = mprAllocObj(HttpTrace, manageTrace)) == 0) {
        return 0;
    }
    if (parent) {
        *trace = *parent;
        trace->parent = parent;
    } else {
        if ((trace->events = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
            return 0;
        }
        mprAddKey(trace->events, "request", ITOP(1));
        mprAddKey(trace->events, "result", ITOP(2));
        mprAddKey(trace->events, "error", ITOP(2));
        mprAddKey(trace->events, "context", ITOP(3));
        mprAddKey(trace->events, "form", ITOP(4));
        mprAddKey(trace->events, "body", ITOP(5));
        mprAddKey(trace->events, "debug", ITOP(5));

        trace->size = HTTP_TRACE_MAX_SIZE;
        trace->formatter = httpDetailTraceFormatter;
        trace->logger = httpWriteTraceLogFile;
        trace->mutex = mprCreateLock();
    }
    return trace;
}


PUBLIC void httpSetTraceContentSize(HttpTrace *trace, ssize size)
{
    trace->maxContent = size;
}


PUBLIC void httpSetTraceEventLevel(HttpTrace *trace, cchar *type, int level)
{
    assert(trace);
    mprAddKey(trace->events, type, ITOP(level));
}


PUBLIC int httpGetTraceLevel()
{
    return HTTP->traceLevel;
}


PUBLIC void httpSetTraceFormat(HttpTrace *trace, cchar *format)
{
    trace->format = sclone(format);
}


PUBLIC HttpTraceFormatter httpSetTraceFormatter(HttpTrace *trace, HttpTraceFormatter callback)
{
    HttpTraceFormatter  prior;

    prior = trace->formatter;
    trace->formatter = callback;
    return prior;
}


PUBLIC void httpSetTraceFormatterName(HttpTrace *trace, cchar *name)
{
    HttpTraceFormatter  formatter;

    if (name && smatch(name, "common")) {
        if ((trace->events = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
            return;
        }
        mprAddKey(trace->events, "complete", ITOP(0));
        formatter = httpCommonTraceFormatter;
    } else {
       formatter = httpDetailTraceFormatter;
    }
    httpSetTraceFormatter(trace, formatter);
}


PUBLIC void httpSetTraceLevel(int level)
{
    if (level < 0) {
        level = 0;
    } else if (level > 5) {
        level = 5;
    }
    HTTP->traceLevel = level;
}


PUBLIC void httpSetTraceLogger(HttpTrace *trace, HttpTraceLogger callback)
{
    trace->logger = callback;
}


/*
    Internal convenience: Used for incoming and outgoing packets.
 */
PUBLIC bool httpTraceBody(HttpConn *conn, bool outgoing, HttpPacket *packet, ssize len)
{
    cchar   *event, *type;

    if (!conn) {
        return 0;
    }
    if (len < 0) {
        len = httpGetPacketLength(packet);
    }
    if (outgoing) {
        if (conn->endpoint) {
            type = "body";
            event = "tx.body.data";
        } else {
            if (sstarts(conn->tx->mimeType, "application/x-www-form-urlencoded")) {
                type = "form";
                event = "tx.body.form";
            } else {
                type = "body";
                event = "tx.body.data";
            }
        }
    } else {
        if (conn->endpoint) {
            if (conn->rx->form) {
                type = "form";
                event = "rx.body.form";
            } else {
                type = "body";
                event = "rx.body.data";
            }
        } else {
            type = "body";
            event = "rx.body.data";
        }
    }
    return httpTracePacket(conn, event, type, packet, "length: %zd", len);
}


/*
    Trace request body content
 */
PUBLIC bool httpTraceContent(HttpConn *conn, cchar *event, cchar *type, cchar *buf, ssize len, cchar *values, ...)
{
    Http        *http;
    HttpTrace   *trace;
    va_list     ap;
    int         level;

    assert(conn);
    assert(buf);

    http = HTTP;
    if (http->traceLevel == 0) {
        return 0;
    }
    if (conn) {
        if (conn->rx->skipTrace) {
            return 0;
        }
        trace = conn->trace;
    } else {
        trace = http->trace;
    }
    level = PTOI(mprLookupKey(trace->events, type));
    if (level == 0 || level > http->traceLevel) {
        return 0;
    }
    if (conn) {
        if ((smatch(event, "rx.body.data") && (conn->rx->bytesRead >= conn->trace->maxContent)) ||
            (smatch(event, "tx.body.data") && (conn->tx->bytesWritten >= conn->trace->maxContent))) {
            if (!conn->rx->webSocket) {
                conn->rx->skipTrace = 1;
                httpTrace(conn, event, type, "msg: 'Abbreviating body trace'");
            }
            return 0;
        }
    }
    if (values) {
        va_start(ap, values);
        values = sfmtv(values, ap);
        va_end(ap);
    }
    httpFormatTrace(trace, conn, event, type, values, buf, len);
    return 1;
}


/*
    Trace any packet
 */
PUBLIC bool httpTracePacket(HttpConn *conn, cchar *event, cchar *type, HttpPacket *packet, cchar *values, ...)
{
    va_list     ap;
    int         level;

    assert(conn);
    assert(packet);

    if (!conn || conn->http->traceLevel == 0 || conn->rx->skipTrace) {
        return 0;
    }
    level = PTOI(mprLookupKey(conn->trace->events, type));
    if (level == 0 || level > conn->http->traceLevel) { \
        return 0;
    }
    if (packet->prefix) {
        httpTraceContent(conn, event, type, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix), 0);
    }
    if (values) {
        va_start(ap, values);
        values = sfmtv(values, ap);
        va_end(ap);
    }
    if (packet->content) {
        if (values) {
            httpTraceContent(conn, event, type, mprGetBufStart(packet->content), httpGetPacketLength(packet), "%s", values);
        } else {
            httpTraceContent(conn, event, type, mprGetBufStart(packet->content), httpGetPacketLength(packet), 0);
        }
    }
    return 1;
}


/*
    Inner routine for httpTrace()
    Conn may be null.
 */
PUBLIC bool httpTraceProc(HttpConn *conn, cchar *event, cchar *type, cchar *values, ...)
{
    HttpTrace   *trace;
    va_list     ap;

    assert(conn);
    assert(event && *event);
    assert(type && *type);

    if (conn && conn->rx->skipTrace) {
        return 0;
    }
    trace = conn ? conn->trace : HTTP->trace;

    if (values) {
        va_start(ap, values);
        values = sfmtv(values, ap);
        va_end(ap);
    }
    httpFormatTrace(trace, conn, event, type, values, 0, 0);
    return 1;
}



PUBLIC void httpFormatTrace(HttpTrace *trace, HttpConn *conn, cchar *event, cchar *type, cchar *values, cchar *buf, 
    ssize len)
{
    (trace->formatter)(trace, conn, event, type, values, buf, len);
}


/*
    Low-level write routine to be used only by formatters
 */
PUBLIC void httpWriteTrace(HttpTrace *trace, cchar *buf, ssize len)
{
    (trace->logger)(trace, buf, len);
}


/*
    Get a printable version of a buffer. Return a pointer to the start of printable data.
    This will use the tx or rx mime type if possible.
    Skips UTF encoding prefixes
 */
PUBLIC cchar *httpMakePrintable(HttpTrace *trace, HttpConn *conn, cchar *event, cchar *buf, ssize *lenp)
{
    cchar   *start, *cp, *digits;
    char    *data, *dp;
    ssize   len;
    int     i;

    if (conn) {
        if (smatch(event, "rx.body")) {
            if (sstarts(mprLookupMime(0, conn->rx->mimeType), "text/")) {
                return buf;
            }
        } else if (smatch(event, "tx.body")) {
            if (sstarts(mprLookupMime(0, conn->tx->mimeType), "text/")) {
                return buf;
            }
        }
    }
    start = buf;
    len = *lenp;
    if (len > 3 && start[0] == (char) 0xef && start[1] == (char) 0xbb && start[2] == (char) 0xbf) {
        /* Step over UTF encoding */
        start += 3;
        *lenp -= 3;
    }
    len = min(len, trace->maxContent);

    for (i = 0; i < len; i++) {
        if (!isprint((uchar) start[i]) && start[i] != '\n' && start[i] != '\r' && start[i] != '\t') {
            data = mprAlloc(len * 3 + ((len / 16) + 1) + 1);
            digits = "0123456789ABCDEF";
            for (i = 0, cp = start, dp = data; cp < &start[len]; cp++) {
                *dp++ = digits[(*cp >> 4) & 0x0f];
                *dp++ = digits[*cp & 0x0f];
                *dp++ = ' ';
                if ((++i % 16) == 0) {
                    *dp++ = '\n';
                }
            }
            *dp++ = '\n';
            *dp = '\0';
            start = data;
            *lenp = dp - start;
            break;
        }
    }
    return start;
}


/*
    Format a detailed request message
 */
PUBLIC void httpDetailTraceFormatter(HttpTrace *trace, HttpConn *conn, cchar *event, cchar *type, cchar *values, 
    cchar *data, ssize len)
{
    MprBuf      *buf;
    MprTime     now;
    char        *cp;
    int         client, sessionSeqno;

    assert(trace);
    assert(event);
    assert(type);

    lock(trace);
    if (!trace->buf) {
        trace->buf = mprCreateBuf(0, 0);
    }
    buf = trace->buf;
    mprFlushBuf(buf);

    if (conn) {
        now = mprGetTime();
        if (trace->lastMark < (now + TPS)) {
            trace->lastTime = mprGetDate("%T");
            trace->lastMark = now;
        }
        client = conn->address ? conn->address->seqno : 0;
        sessionSeqno = conn->rx->session ? (int) stoi(conn->rx->session->id) : 0;
        mprPutToBuf(buf, "\n%s %d-%d-%d-%d %s", trace->lastTime, client, sessionSeqno, conn->seqno, conn->rx->seqno, event);
    } else {
        mprPutToBuf(buf, "\n%s: %s", trace->lastTime, event);
    }
    if (values) {
        mprPutCharToBuf(buf, ' ');
        for (cp = (char*) values; *cp; cp++) {
            if (cp[0] == ':') {
                cp[0] = '=';
            } else if (cp[0] == ',') {
                cp[0] = ' ';
            }
        }
        mprPutStringToBuf(buf, values);
        mprPutCharToBuf(buf, '\n');
    }
    if (data) {
        mprPutToBuf(buf, "\n----\n");
        data = httpMakePrintable(trace, conn, event, data, &len);
        mprPutBlockToBuf(buf, data, len);
        if (len > 0 && data[len - 1] != '\n') {
            mprPutCharToBuf(buf, '\n');
        }
        mprPutToBuf(buf, "----\n");
    }
    httpWriteTrace(trace, mprGetBufStart(buf), mprGetBufLength(buf));
    unlock(trace);
}


/************************************** TraceLogFile **************************/

static int backupTraceLogFile(HttpTrace *trace)
{
    MprPath     info;

    assert(trace->path);

    if (trace->file == MPR->logFile) {
        return 0;
    }
    if (trace->backupCount > 0 || (trace->flags & MPR_LOG_ANEW)) {
        lock(trace);
        if (trace->path && trace->parent && smatch(trace->parent->path, trace->path)) {
            unlock(trace);
            return backupTraceLogFile(trace->parent);
        }
        mprGetPathInfo(trace->path, &info);
        if (info.valid && ((trace->flags & MPR_LOG_ANEW) || info.size > trace->size)) {
            if (trace->file) {
                mprCloseFile(trace->file);
                trace->file = 0;
            }
            if (trace->backupCount > 0) {
                mprBackupLog(trace->path, trace->backupCount);
            }
        }
        unlock(trace);
    }
    return 0;
}


/*
    Open the request log file
 */
PUBLIC int httpOpenTraceLogFile(HttpTrace *trace)
{
    MprFile     *file;
    int         mode;

    if (!trace->file && trace->path) {
        if (smatch(trace->path, "-")) {
            file = MPR->logFile;
        } else {
            backupTraceLogFile(trace);
            mode = O_CREAT | O_WRONLY | O_TEXT;
            if (trace->flags & MPR_LOG_ANEW) {
                mode |= O_TRUNC;
            }
            if (smatch(trace->path, "stdout")) {
                file = MPR->stdOutput;
            } else if (smatch(trace->path, "stderr")) {
                file = MPR->stdError;
            } else if ((file = mprOpenFile(trace->path, mode, 0664)) == 0) {
                mprLog("error http trace", 0, "Cannot open log file %s", trace->path);
                return MPR_ERR_CANT_OPEN;
            }
        }
        trace->file = file;
        trace->flags &= ~MPR_LOG_ANEW;
    }
    return 0;
}


/*
    Start tracing when instructed via a command line option. No backup, max size or custom format.
 */
PUBLIC int httpStartTracing(cchar *traceSpec)
{
    HttpTrace   *trace;
    char        *lspec;

    if (HTTP == 0 || HTTP->trace == 0 || traceSpec == 0 || *traceSpec == '\0') {
        assert(HTTP);
        return MPR_ERR_BAD_STATE;
    }
    trace = HTTP->trace;
    trace->flags = MPR_LOG_ANEW | MPR_LOG_CMDLINE;
    trace->path = stok(sclone(traceSpec), ":", &lspec);
    HTTP->traceLevel = (int) stoi(lspec);
    return httpOpenTraceLogFile(trace);
}


/*
    Configure the trace log file
 */
PUBLIC int httpSetTraceLogFile(HttpTrace *trace, cchar *path, ssize size, int backup, cchar *format, int flags)
{
    assert(trace);
    assert(path && *path);

    if (format == NULL || *format == '\0') {
        format = ME_HTTP_LOG_FORMAT;
    }
    trace->backupCount = backup;
    trace->flags = flags;
    trace->format = sclone(format);
    trace->size = size;
    trace->path = sclone(path);
    return httpOpenTraceLogFile(trace);
}


/*
    Write a message to the trace log
 */
PUBLIC void httpWriteTraceLogFile(HttpTrace *trace, cchar *buf, ssize len)
{
    static int  skipCheck = 0;

    lock(trace);
    if (trace->backupCount > 0) {
        if ((++skipCheck % 50) == 0) {
            backupTraceLogFile(trace);
        }
    }
    if (!trace->file && trace->path && httpOpenTraceLogFile(trace) < 0) {
        unlock(trace);
        return;
    }
    mprWriteFile(trace->file, buf, len);
    unlock(trace);
}


/*
    Common Log Formatter (NCSA)
    This formatter only emits messages only for connections at their complete event.
 */
PUBLIC void httpCommonTraceFormatter(HttpTrace *trace, HttpConn *conn, cchar *type, cchar *event, cchar *valuesUnused,
    cchar *bufUnused, ssize lenUnused)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprBuf      *buf;
    cchar       *fmt, *cp, *qualifier, *timeText, *value;
    char        c, keyBuf[80];
    int         len;

    assert(trace);
    assert(type && *type);
    assert(event && *event);

    if (!conn) {
        return;
    }
    assert(type && *type);
    assert(event && *event);

    if (!smatch(event, "request.completion")) {
        return;
    }
    rx = conn->rx;
    tx = conn->tx;
    fmt = trace->format;
    if (fmt == 0) {
        fmt = ME_HTTP_LOG_FORMAT;
    }
    len = ME_MAX_URI + 256;
    buf = mprCreateBuf(len, len);

    while ((c = *fmt++) != '\0') {
        if (c != '%' || (c = *fmt++) == '%') {
            mprPutCharToBuf(buf, c);
            continue;
        }
        switch (c) {
        case 'a':                           /* Remote IP */
            mprPutStringToBuf(buf, conn->ip);
            break;

        case 'A':                           /* Local IP */
            mprPutStringToBuf(buf, conn->sock->listenSock->ip);
            break;

        case 'b':
            if (tx->bytesWritten == 0) {
                mprPutCharToBuf(buf, '-');
            } else {
                mprPutIntToBuf(buf, tx->bytesWritten);
            }
            break;

        case 'B':                           /* Bytes written (minus headers) */
            mprPutIntToBuf(buf, (tx->bytesWritten - tx->headerSize));
            break;

        case 'h':                           /* Remote host */
            mprPutStringToBuf(buf, conn->ip);
            break;

        case 'l':                           /* user identity - unknown */
            mprPutCharToBuf(buf, '-');
            break;

        case 'n':                           /* Local host */
            mprPutStringToBuf(buf, rx->parsedUri->host);
            break;

        case 'O':                           /* Bytes written (including headers) */
            mprPutIntToBuf(buf, tx->bytesWritten);
            break;

        case 'r':                           /* First line of request */
            mprPutToBuf(buf, "%s %s %s", rx->method, rx->uri, conn->protocol);
            break;

        case 's':                           /* Response code */
            mprPutIntToBuf(buf, tx->status);
            break;

        case 't':                           /* Time */
            mprPutCharToBuf(buf, '[');
            timeText = mprFormatLocalTime(MPR_DEFAULT_DATE, mprGetTime());
            mprPutStringToBuf(buf, timeText);
            mprPutCharToBuf(buf, ']');
            break;

        case 'u':                           /* Remote username */
            mprPutStringToBuf(buf, conn->username ? conn->username : "-");
            break;

        case '{':                           /* Header line "{header}i" */
            qualifier = fmt;
            if ((cp = schr(qualifier, '}')) != 0) {
                fmt = &cp[1];
                scopy(keyBuf, sizeof(keyBuf), "HTTP_");
                sncopy(&keyBuf[5], sizeof(keyBuf) - 5, qualifier, qualifier - cp);
                switch (*fmt++) {
                case 'i':
                    value = (char*) mprLookupKey(rx->headers, supper(keyBuf));
                    mprPutStringToBuf(buf, value ? value : "-");
                    break;
                default:
                    mprPutSubStringToBuf(buf, qualifier, qualifier - cp);
                }

            } else {
                mprPutCharToBuf(buf, c);
            }
            break;

        case '>':
            if (*fmt == 's') {
                fmt++;
                mprPutIntToBuf(buf, tx->status);
            }
            break;

        default:
            mprPutCharToBuf(buf, c);
            break;
        }
    }
    mprPutCharToBuf(buf, '\n');
    httpWriteTrace(trace, mprBufToString(buf), mprGetBufLength(buf));
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

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
