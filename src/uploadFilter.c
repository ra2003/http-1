/*
    uploadFilter.c - Upload file filter.

    The upload filter processes post data according to RFC-1867 ("multipart/form-data" post data).
    It saves the uploaded files in a configured upload directory.

    The upload filter is configured in the standard pipeline before the request is parsed and routed.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/

#include    "http.h"

/*********************************** Locals ***********************************/
/*
    Upload state machine states
 */
#define HTTP_UPLOAD_REQUEST_HEADER        1   /* Request header */
#define HTTP_UPLOAD_BOUNDARY              2   /* Boundary divider */
#define HTTP_UPLOAD_CONTENT_HEADER        3   /* Content part header */
#define HTTP_UPLOAD_CONTENT_DATA          4   /* Content encoded data */
#define HTTP_UPLOAD_CONTENT_END           5   /* End of multipart message */

/*
    Per upload context
 */
typedef struct Upload {
    HttpUploadFile  *currentFile;       /* Current file context */
    MprFile         *file;              /* Current file I/O object */
    char            *boundary;          /* Boundary signature */
    ssize           boundaryLen;        /* Length of boundary */
    int             contentState;       /* Input states */
    char            *clientFilename;    /* Current file filename */
    char            *tmpPath;           /* Current temp filename for upload data */
    char            *name;              /* Form field name keyword value */
} Upload;

/********************************** Forwards **********************************/

static void addUploadFile(HttpStream *stream, HttpUploadFile *upfile);
static Upload *allocUpload(HttpQueue *q);
static void cleanUploadedFiles(HttpStream *stream);
static void closeUpload(HttpQueue *q);
static char *getBoundary(char *buf, ssize bufLen, char *boundary, ssize boundaryLen, bool *pureData);
static cchar *getUploadDir(HttpStream *stream);
static void incomingUpload(HttpQueue *q, HttpPacket *packet);
static void manageHttpUploadFile(HttpUploadFile *file, int flags);
static void manageUpload(Upload *up, int flags);
static int openUpload(HttpQueue *q);
static int  processUploadBoundary(HttpQueue *q, char *line);
static int  processUploadHeader(HttpQueue *q, char *line);
static int  processUploadData(HttpQueue *q);
static void renameUploadedFiles(HttpStream *stream);
static void startUpload(HttpQueue *q);
static bool validUploadChars(cchar *uri);

/************************************* Code ***********************************/

PUBLIC int httpOpenUploadFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("uploadFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->uploadFilter = filter;
    filter->flags |= HTTP_STAGE_INTERNAL;
    filter->open = openUpload;
    filter->close = closeUpload;
    filter->start = startUpload;
    filter->incoming = incomingUpload;
    return 0;
}


/*
    Initialize the upload filter for a new request
 */
static Upload *allocUpload(HttpQueue *q)
{
    HttpStream  *stream;
    HttpRx      *rx;
    Upload      *up;
    cchar       *uploadDir;
    char        *boundary;

    stream = q->stream;
    rx = stream->rx;
    if ((up = mprAllocObj(Upload, manageUpload)) == 0) {
        return 0;
    }
    q->queueData = up;
    up->contentState = HTTP_UPLOAD_BOUNDARY;

    uploadDir = getUploadDir(stream);
    httpSetParam(stream, "UPLOAD_DIR", uploadDir);

    if ((boundary = strstr(rx->mimeType, "boundary=")) != 0) {
        boundary += 9;
        up->boundary = sjoin("--", boundary, NULL);
        up->boundaryLen = strlen(up->boundary);
    }
    if (up->boundaryLen == 0 || *up->boundary == '\0') {
        httpError(stream, HTTP_CODE_BAD_REQUEST, "Bad boundary");
        return 0;
    }
    return up;
}


static void manageUpload(Upload *up, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(up->currentFile);
        mprMark(up->file);
        mprMark(up->boundary);
        mprMark(up->clientFilename);
        mprMark(up->tmpPath);
        mprMark(up->name);
    }
}


static void freeUpload(HttpQueue *q)
{
    HttpUploadFile  *file;
    Upload          *up;

    if ((up = q->queueData) != 0) {
        if (up->currentFile) {
            file = up->currentFile;
            file->filename = 0;
        }
        q->queueData = 0;
    }
}


static int openUpload(HttpQueue *q)
{
    /* Necessary because we want closeUpload to be able to clean files */
    return 0;
}


static void closeUpload(HttpQueue *q)
{
    Upload      *up;

    if ((up = q->queueData) != 0) {
        cleanUploadedFiles(q->stream);
        freeUpload(q);
    }
}


/*
    Start is invoked when the full request pipeline is started. For upload, this actually happens after
    all input has been received.
 */
static void startUpload(HttpQueue *q)
{
    Upload      *up;

    if ((up = q->queueData) != 0) {
        renameUploadedFiles(q->stream);
    }
}


/*
    Incoming data acceptance routine. The service queue is used, but not a service routine as the data is processed
    immediately. Partial data is buffered on the service queue until a correct mime boundary is seen.
 */
static void incomingUpload(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;
    HttpRx      *rx;
    MprBuf      *content;
    Upload      *up;
    char        *line, *nextTok;
    ssize       count;
    int         done, rc;

    assert(packet);

    stream = q->stream;
    rx = stream->rx;
    if (!rx->upload || stream->error) {
        httpPutPacketToNext(q, packet);
        return;
    }
    if ((up = q->queueData) == 0) {
        if ((up = allocUpload(q)) == 0) {
            return;
        }
    }
    if (packet->flags & HTTP_PACKET_END) {
        if (up->contentState != HTTP_UPLOAD_CONTENT_END) {
            httpError(stream, HTTP_CODE_BAD_REQUEST, "Client supplied insufficient upload data");
        }
        httpPutPacketToNext(q, packet);
        return;
    }

    /*
        Put the packet data onto the service queue for buffering. This aggregates input data incase we don't have
        a complete mime record yet.
     */
    httpJoinPacketForService(q, packet, 0);

    packet = q->first;
    content = packet->content;
    count = httpGetPacketLength(packet);

    for (done = 0, line = 0; !done; ) {
        if  (up->contentState == HTTP_UPLOAD_BOUNDARY || up->contentState == HTTP_UPLOAD_CONTENT_HEADER) {
            /*
                Parse the next input line
             */
            line = mprGetBufStart(content);
            if ((nextTok = memchr(line, '\n', mprGetBufLength(content))) == 0) {
                /* Incomplete line */
                break;
            }
            *nextTok++ = '\0';
            mprAdjustBufStart(content, (int) (nextTok - line));
            line = strim(line, "\r", MPR_TRIM_END);
        }
        switch (up->contentState) {
        case HTTP_UPLOAD_BOUNDARY:
            if (processUploadBoundary(q, line) < 0) {
                done++;
            }
            break;

        case HTTP_UPLOAD_CONTENT_HEADER:
            if (processUploadHeader(q, line) < 0) {
                done++;
            }
            break;

        case HTTP_UPLOAD_CONTENT_DATA:
            rc = processUploadData(q);
            if (rc < 0) {
                done++;
            }
            if (httpGetPacketLength(packet) < up->boundaryLen) {
                /*  Incomplete boundary - return to get more data */
                done++;
            }
            break;

        case HTTP_UPLOAD_CONTENT_END:
            done++;
            break;
        }
    }
    q->count -= (count - httpGetPacketLength(packet));
    assert(q->count >= 0);

    if (httpGetPacketLength(packet) == 0) {
        /*
            Quicker to remove the buffer so the packets don't have to be joined the next time
         */
        httpGetPacket(q);
    } else {
        /*
            Compact the buffer to prevent memory growth. There is often residual data after the boundary for the next block.
         */
        if (packet != rx->headerPacket) {
            mprCompactBuf(content);
        }
    }
}


/*
    Process the mime boundary division
    Returns  < 0 on a request or state error
            == 0 if successful
 */
static int processUploadBoundary(HttpQueue *q, char *line)
{
    HttpStream  *stream;
    Upload      *up;

    stream = q->stream;
    up = q->queueData;

    /*
        Expecting a multipart boundary string
     */
    if (strncmp(up->boundary, line, up->boundaryLen) != 0) {
        httpError(stream, HTTP_CODE_BAD_REQUEST, "Bad upload state. Incomplete boundary");
        return MPR_ERR_BAD_STATE;
    }
    if (line[up->boundaryLen] && strcmp(&line[up->boundaryLen], "--") == 0) {
        up->contentState = HTTP_UPLOAD_CONTENT_END;
    } else {
        up->contentState = HTTP_UPLOAD_CONTENT_HEADER;
    }
    return 0;
}


/*
    Expecting content headers. A blank line indicates the start of the data.
    Returns  < 0  Request or state error
    Returns == 0  Successfully parsed the input line.
 */
static int processUploadHeader(HttpQueue *q, char *line)
{
    HttpStream      *stream;
    HttpUploadFile  *file;
    Upload          *up;
    cchar           *uploadDir;
    char            *key, *headerTok, *rest, *nextPair, *value;

    stream = q->stream;
    up = q->queueData;

    if (line[0] == '\0') {
        up->contentState = HTTP_UPLOAD_CONTENT_DATA;
        return 0;
    }

    headerTok = line;
    stok(line, ": ", &rest);

    if (scaselesscmp(headerTok, "Content-Disposition") == 0) {
        /*
            The content disposition header describes either a form
            variable or an uploaded file.

            Content-Disposition: form-data; name="field1"
            >>blank line
            Field Data
            ---boundary

            Content-Disposition: form-data; name="field1" ->
                filename="user.file"
            >>blank line
            File data
            ---boundary
         */
        key = rest;
        up->name = up->clientFilename = 0;
        while (key && stok(key, ";\r\n", &nextPair)) {

            key = strim(key, " ", MPR_TRIM_BOTH);
            stok(key, "= ", &value);
            value = strim(value, "\"", MPR_TRIM_BOTH);

            if (scaselesscmp(key, "form-data") == 0) {
                /* Nothing to do */

            } else if (scaselesscmp(key, "name") == 0) {
                up->name = sclone(value);

            } else if (scaselesscmp(key, "filename") == 0) {
                if (up->name == 0) {
                    httpError(stream, HTTP_CODE_BAD_REQUEST, "Bad upload state. Missing name field");
                    return MPR_ERR_BAD_STATE;
                }
                /*
                    Client filenames must be simple filenames without illegal characters or path separators.
                    We are deliberately restrictive here to assist users that may use the clientFilename in shell scripts.
                    They MUST still sanitize for their environment, but some extra caution is worthwhile.
                 */
                if (*value == '.' || !validUploadChars(value)) {
                    httpError(stream, HTTP_CODE_BAD_REQUEST, "Bad upload client filename.");
                    return MPR_ERR_BAD_STATE;
                }
                up->clientFilename = mprNormalizePath(value);

                /*
                    Create the file to hold the uploaded data
                 */
                uploadDir = getUploadDir(stream);
                up->tmpPath = mprGetTempPath(uploadDir);
                if (up->tmpPath == 0) {
                    if (!mprPathExists(uploadDir, X_OK)) {
                        mprLog("http error", 0, "Cannot access upload directory %s", uploadDir);
                    }
                    httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR,
                        "Cannot create upload temp file %s. Check upload temp dir %s", up->tmpPath, uploadDir);
                    return MPR_ERR_CANT_OPEN;
                }
                httpLog(stream->trace, "upload.file", "context", "clientFilename:'%s', filename:'%s'",
                    up->clientFilename, up->tmpPath);

                up->file = mprOpenFile(up->tmpPath, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
                if (up->file == 0) {
                    httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot open upload temp file %s", up->tmpPath);
                    return MPR_ERR_BAD_STATE;
                }
                /*
                    Create the file
                 */
                file = up->currentFile = mprAllocObj(HttpUploadFile, manageHttpUploadFile);
                file->clientFilename = up->clientFilename;
                file->filename = up->tmpPath;
                file->name = up->name;
                addUploadFile(stream, file);
            }
            key = nextPair;
        }

    } else if (scaselesscmp(headerTok, "Content-Type") == 0) {
        if (up->clientFilename) {
            up->currentFile->contentType = sclone(rest);
        }
    }
    return 0;
}


static void manageHttpUploadFile(HttpUploadFile *file, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(file->name);
        mprMark(file->filename);
        mprMark(file->clientFilename);
        mprMark(file->contentType);
    }
}


static void defineFileFields(HttpQueue *q, Upload *up)
{
    HttpStream      *stream;
    HttpUploadFile  *file;
    char            *key;

    stream = q->stream;
#if DEPRECATED || 1
    if (stream->tx->handler && stream->tx->handler == stream->http->ejsHandler) {
        return;
    }
#endif
    up = q->queueData;
    file = up->currentFile;
    key = sjoin("FILE_CLIENT_FILENAME_", up->name, NULL);
    httpSetParam(stream, key, file->clientFilename);

    key = sjoin("FILE_CONTENT_TYPE_", up->name, NULL);
    httpSetParam(stream, key, file->contentType);

    key = sjoin("FILE_FILENAME_", up->name, NULL);
    httpSetParam(stream, key, file->filename);

    key = sjoin("FILE_SIZE_", up->name, NULL);
    httpSetIntParam(stream, key, (int) file->size);
}


static int writeToFile(HttpQueue *q, char *data, ssize len)
{
    HttpStream      *stream;
    HttpUploadFile  *file;
    HttpLimits      *limits;
    Upload          *up;
    ssize           rc;

    stream = q->stream;
    limits = stream->limits;
    up = q->queueData;
    file = up->currentFile;

    if ((file->size + len) > limits->uploadSize) {
        /*
            Abort the connection as we don't want the load of receiving the entire body
         */
        httpLimitError(stream, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE, "Uploaded file exceeds maximum %lld",
            limits->uploadSize);
        return MPR_ERR_CANT_WRITE;
    }
    if (len > 0) {
        /*
            File upload. Write the file data.
         */
        rc = mprWriteFile(up->file, data, len);
        if (rc != len) {
            httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR,
                "Cannot write to upload temp file %s, rc %zd, errno %d", up->tmpPath, rc, mprGetOsError());
            return MPR_ERR_CANT_WRITE;
        }
        file->size += len;
        stream->rx->bytesUploaded += len;
    }
    return 0;
}


/*
    Process the content data.
    Returns < 0 on error
            == 0 when more data is needed
            == 1 when data successfully written
 */
static int processUploadData(HttpQueue *q)
{
    HttpStream      *stream;
    HttpPacket      *packet;
    MprBuf          *content;
    Upload          *up;
    ssize           size, dataLen;
    bool            pureData;
    char            *data, *bp, *key;

    stream = q->stream;
    up = q->queueData;
    content = q->first->content;
    packet = 0;

    size = mprGetBufLength(content);
    if (size < up->boundaryLen) {
        /*  Incomplete boundary. Return and get more data */
        return 0;
    }
    bp = getBoundary(mprGetBufStart(content), size, up->boundary, up->boundaryLen, &pureData);
    if (bp == 0) {
        if (up->clientFilename) {
            /*
                No signature found yet. probably more data to come. Must handle split boundaries.
             */
            data = mprGetBufStart(content);
            dataLen = pureData ? size : (size - (up->boundaryLen - 1));
            if (dataLen > 0) {
                if (writeToFile(q, mprGetBufStart(content), dataLen) < 0) {
                    return MPR_ERR_CANT_WRITE;
                }
            }
            mprAdjustBufStart(content, dataLen);
            return 0;       /* Get more data */
        }
    }
    data = mprGetBufStart(content);
    dataLen = (bp) ? (bp - data) : mprGetBufLength(content);

    if (dataLen > 0) {
        mprAdjustBufStart(content, dataLen);
        /*
            This is the CRLF before the boundary
         */
        if (dataLen >= 2 && data[dataLen - 2] == '\r' && data[dataLen - 1] == '\n') {
            dataLen -= 2;
        }
        if (up->clientFilename) {
            /*
                Write the last bit of file data and add to the list of files and define environment variables
             */
            if (writeToFile(q, data, dataLen) < 0) {
                return MPR_ERR_CANT_WRITE;
            }
            defineFileFields(q, up);

        } else {
            /*
                Normal string form data variables
             */
            data[dataLen] = '\0';
            key = mprUriDecode(up->name);
            data = mprUriDecode(data);
            httpSetParam(stream, key, data);

            if (packet == 0) {
                packet = httpCreatePacket(ME_BUFSIZE);
            }
            if (httpGetPacketLength(packet) > 0) {
                /*
                    Need to add www-form-urlencoding separators
                 */
                mprPutCharToBuf(packet->content, '&');
            } else {
                stream->rx->mimeType = sclone("application/x-www-form-urlencoded");
            }
            mprPutToBuf(packet->content, "%s=%s", up->name, data);
        }
    }
    if (up->clientFilename) {
        /*
            Now have all the data (we've seen the boundary)
         */
        mprCloseFile(up->file);
        up->file = 0;
        up->clientFilename = 0;
    }
    if (packet) {
        httpPutPacketToNext(q, packet);
    }
    up->contentState = HTTP_UPLOAD_BOUNDARY;
    return 0;
}


/*
    Find the boundary signature in memory. Returns pointer to the first match.
 */
static char *getBoundary(char *buf, ssize bufLen, char *boundary, ssize boundaryLen, bool *pureData)
{
    char    *cp, *endp;
    char    first;

    assert(buf);
    assert(boundary);
    assert(boundaryLen > 0);

    first = *boundary & 0xff;
    endp = &buf[bufLen];

    for (cp = buf; cp < endp; cp++) {
        if ((cp = memchr(cp, first, endp - cp)) == 0) {
            *pureData = 1;
            return 0;
        }
        /* Potential boundary */
        if ((endp - cp) < boundaryLen) {
            *pureData = 0;
            return 0;
        }
        if (memcmp(cp, boundary, boundaryLen) == 0) {
            *pureData = 0;
            return cp;
        }
    }
    *pureData = 0;
    return 0;
}


static void addUploadFile(HttpStream *stream, HttpUploadFile *upfile)
{
    HttpRx   *rx;

    rx = stream->rx;
    if (rx->files == 0) {
        rx->files = mprCreateList(0, MPR_LIST_STABLE);
    }
    mprAddItem(rx->files, upfile);
}


static void cleanUploadedFiles(HttpStream *stream)
{
    HttpRx          *rx;
    HttpUploadFile  *file;
    int             index;

    rx = stream->rx;

    for (ITERATE_ITEMS(rx->files, file, index)) {
        if (file->filename && rx->route) {
            if (rx->route->autoDelete && !rx->route->renameUploads) {
                mprDeletePath(file->filename);
            }
            file->filename = 0;
        }
    }
}


static void renameUploadedFiles(HttpStream *stream)
{
    HttpRx          *rx;
    HttpUploadFile  *file;
    cchar           *path, *uploadDir;
    int             index;

    rx = stream->rx;
    uploadDir = getUploadDir(stream);

    for (ITERATE_ITEMS(rx->files, file, index)) {
        if (file->filename && rx->route) {
            if (rx->route->renameUploads) {
                path = mprJoinPath(uploadDir, file->clientFilename);
                if (rename(file->filename, path) != 0) {
                    mprLog("http error", 0, "Cannot rename %s to %s", file->filename, path);
                }
                file->filename = path;
            }
        }
    }
}


static cchar *getUploadDir(HttpStream *stream)
{
    cchar   *uploadDir;

    if ((uploadDir = httpGetDir(stream->host->defaultRoute, "upload")) == 0) {
#if ME_WIN_LIKE
        uploadDir = mprNormalizePath(getenv("TEMP"));
#else
        uploadDir = sclone("/tmp");
#endif
    }
    return uploadDir;
}


static bool validUploadChars(cchar *uri)
{
    ssize   pos;

    if (uri == 0) {
        return 1;
    }
    pos = strspn(uri, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?#[]@!$&'()*+,;=% ");
    if (pos < slen(uri)) {
        return 0;
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
