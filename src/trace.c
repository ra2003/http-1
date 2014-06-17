/*
    trace.c -- Trace data
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.

    - How to record timestamps in log file?
 */

/********************************* Includes ***********************************/

#include    "http.h"

/*********************************** Code *************************************/

static void manageTrace(HttpTrace *trace, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(trace->file);
        mprMark(trace->format);
        mprMark(trace->mutex);
        mprMark(trace->path);
        mprMark(trace->events);
    }
}

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
        mprAddKey(trace->events, "first", ITOP(1));
        mprAddKey(trace->events, "error", ITOP(1));
        mprAddKey(trace->events, "complete", ITOP(2));
        mprAddKey(trace->events, "connection", ITOP(3));
        mprAddKey(trace->events, "headers", ITOP(3));
        mprAddKey(trace->events, "context", ITOP(3));
        mprAddKey(trace->events, "close", ITOP(3));
        mprAddKey(trace->events, "rx", ITOP(4));
        mprAddKey(trace->events, "tx", ITOP(5));

        trace->size = HTTP_TRACE_MAX_SIZE;
        trace->formatter = httpDetailTraceFormatter;
        trace->logger = httpWriteTraceLogFile;
        trace->mutex = mprCreateLock();
    }
    return trace;
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


PUBLIC void httpSetTraceLevel(int level)
{
    Http    *http;

    if (level < 0) {
        level = 0;
    } else if (level > 5) {
        level = 5;
    }
    http = MPR->httpService;
    http->traceLevel = level;
}


//  MOB order
PUBLIC int httpGetTraceLevel()
{
    Http    *http;

    http = MPR->httpService;
    return http->traceLevel;
}


PUBLIC void httpSetTraceEventLevel(HttpTrace *trace, cchar *event, int level)
{
    assert(trace);
    mprAddKey(trace->events, event, ITOP(level));
}


PUBLIC void httpSetTraceContentSize(HttpTrace *trace, ssize size)
{
    trace->maxContent = size;
}


PUBLIC void httpSetTraceLogger(HttpTrace *trace, HttpTraceLogger callback)
{
    trace->logger = callback;
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


/*
    Trace a simple message
 */
PUBLIC void httpTraceProc(HttpConn *conn, cchar *event, cchar *msg, cchar *values, ...)
{
    va_list     ap;

    assert(conn);
    assert(event && *event);

    if (!conn->rx->skipTrace) {
        if (values) {
            va_start(ap, values);
            values = sfmtv(values, ap);
            va_end(ap);
        }
        httpFormatTrace(conn, event, msg, values, 0, 0);
    }
}


/*
    Trace body content
 */
PUBLIC void httpTraceContent(HttpConn *conn, cchar *event, cchar *buf, ssize len, cchar *msg, cchar *values, ...)
{
    va_list     ap;

    assert(conn);
    assert(buf);

    if (!httpShouldTrace(conn, event) || conn->rx->skipTrace) {
        return;
    }
    if ((smatch(event, "rx.body") && (conn->rx->bytesRead >= conn->trace->size)) ||
        (smatch(event, "tx.body") && (conn->tx->bytesWritten >= conn->trace->size))) {
        if (!conn->rx->skipTrace && !conn->rx->webSocket) {
            conn->rx->skipTrace = 1;
            httpTrace(conn, event, "Abbreviating body trace", 0, 0);
        }
        return;
    }
    if (values) {
        va_start(ap, values);
        values = sfmtv(values, ap);
        va_end(ap);
    }
    httpFormatTrace(conn, event, msg, values, buf, len);
}


/*
    Trace a packet
 */
PUBLIC void httpTracePacket(HttpConn *conn, cchar *event, HttpPacket *packet, cchar *msg, cchar *values, ...)
{
    va_list     ap;

    assert(conn);
    assert(packet);

    if (packet->prefix) {
        mprAddNullToBuf(packet->prefix);
        httpTraceContent(conn, event, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix), 0, 0);
    }
    if (values) {
        va_start(ap, values);
        values = sfmtv(values, ap);
        va_end(ap);
    }
    if (packet->content) {
        mprAddNullToBuf(packet->content);
        httpTraceContent(conn, event, mprGetBufStart(packet->content), httpGetPacketLength(packet), msg, values);
    }
}


PUBLIC void httpFormatTrace(HttpConn *conn, cchar *event, cchar *msg, cchar *values, cchar *buf, ssize len)
{
    (conn->trace->formatter)(conn, event, msg, values, buf, len);
}


/*
    Low-level write routine to be used only by formatters
 */
PUBLIC void httpWriteTrace(HttpConn *conn, cchar *buf, ssize len)
{
    (conn->trace->logger)(conn, buf, len);
}


/*
    Get a printable version of a buffer. Return a pointer to the start of printable data.
    This will use the tx or rx mime type if possible.
    Skips UTF encoding prefixes
 */
PUBLIC cchar *httpMakePrintable(HttpConn *conn, cchar *event, cchar *buf, ssize *lenp)
{
    cchar   *start, *cp, *digits;
    char    *data, *dp;
    ssize   len;
    int     i;

    if (smatch(event, "rx.body")) {
        if (sstarts(mprLookupMime(0, conn->rx->mimeType), "text/")) {
            return buf;
        }
    } else if (smatch(event, "tx.body")) {
        if (sstarts(mprLookupMime(0, conn->tx->mimeType), "text/")) {
            return buf;
        }
    }
    start = buf;
    len = *lenp;
    if (len > 3 && start[0] == (char) 0xef && start[1] == (char) 0xbb && start[2] == (char) 0xbf) {
        /* Step over UTF encoding */
        start += 3;
        *lenp -= 3;
    }
    //  MOB - must enforce trace->maxContent
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
PUBLIC void httpDetailTraceFormatter(HttpConn *conn, cchar *event, cchar *msg, cchar *values, cchar *data, ssize len)
{
    char    *boundary, buf[256];
    int     client, sessionSeqno;

    assert(conn);
    assert(event);

    client = conn->address ? conn->address->seqno : 0;
    sessionSeqno = conn->rx->session ? (int) stoi(conn->rx->session->id) : 0;
    fmt(buf, sizeof(buf), "\n%s %d-%d-%d-%d ", mprGetDate(MPR_LOG_DATE), client, sessionSeqno, conn->seqno, 
        conn->rx->seqno);
    lock(conn->trace);
    httpWriteTrace(conn, buf, slen(buf));
    if (msg) {
        //  MOB - what if msg contains commas?
        msg = fmt(buf, sizeof(buf), "%s, msg=\"%s\", ", event, msg);
        httpWriteTrace(conn, buf, slen(buf));
    } else {
        msg = fmt(buf, sizeof(buf), "%s, ", event);
        httpWriteTrace(conn, buf, slen(buf));
    }
    if (values) {
        //  MOB - what if msg contains commas?
        httpWriteTrace(conn, values, slen(values));
    }
    if (data) {
        boundary = " --details--\n";
        httpWriteTrace(conn, boundary, slen(boundary));
        data = httpMakePrintable(conn, event, data, &len);
        httpWriteTrace(conn, data, len);
        httpWriteTrace(conn, &boundary[1], slen(boundary) - 1);
    } else {
        httpWriteTrace(conn, "\n", 1);
    }
    unlock(conn->trace);
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
    Http        *http;
    HttpTrace   *trace;
    char        *lspec;

    if ((http = MPR->httpService) == 0 || http->trace == 0 || traceSpec == 0 || *traceSpec == '\0') {
        return MPR_ERR_BAD_STATE;
    }
    trace = http->trace;
    trace->flags = MPR_LOG_ANEW | MPR_LOG_CMDLINE;
    trace->path = stok(sclone(traceSpec), ":", &lspec);
    http->traceLevel = (int) stoi(lspec);
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
PUBLIC void httpWriteTraceLogFile(HttpConn *conn, cchar *buf, ssize len)
{
    HttpTrace   *trace;
    static int  skipCheck = 0;

    trace = conn->trace;
    lock(trace);
    if (trace->backupCount > 0) {
        if ((++skipCheck % 50) == 0) {
            backupTraceLogFile(trace);
            if (!trace->file && httpOpenTraceLogFile(trace) < 0) {
                unlock(trace);
                return;
            }
        }
    }
    mprWriteFile(trace->file, buf, len);
    unlock(trace);
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
