#
#   http-macosx-static.sh -- Build It Shell Script to build Http Library
#

PRODUCT="http"
VERSION="1.3.0"
BUILD_NUMBER="0"
PROFILE="static"
ARCH="x64"
ARCH="`uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/'`"
OS="macosx"
CONFIG="${OS}-${ARCH}-${PROFILE}"
CC="/usr/bin/clang"
LD="/usr/bin/ld"
CFLAGS="-O3   -w"
DFLAGS=""
IFLAGS="-I${CONFIG}/inc -Isrc"
LDFLAGS="-Wl,-rpath,@executable_path/ -Wl,-rpath,@loader_path/"
LIBPATHS="-L${CONFIG}/bin"
LIBS="-lpthread -lm -ldl"

[ ! -x ${CONFIG}/inc ] && mkdir -p ${CONFIG}/inc ${CONFIG}/obj ${CONFIG}/lib ${CONFIG}/bin

[ ! -f ${CONFIG}/inc/bit.h ] && cp projects/http-${OS}-${PROFILE}-bit.h ${CONFIG}/inc/bit.h
[ ! -f ${CONFIG}/inc/bitos.h ] && cp ${SRC}/src/bitos.h ${CONFIG}/inc/bitos.h
if ! diff ${CONFIG}/inc/bit.h projects/http-${OS}-${PROFILE}-bit.h >/dev/null ; then
	cp projects/http-${OS}-${PROFILE}-bit.h ${CONFIG}/inc/bit.h
fi

rm -rf ${CONFIG}/inc/est.h
cp -r src/deps/est/est.h ${CONFIG}/inc/est.h

${DFLAGS}${CC} -c -o ${CONFIG}/obj/estLib.o -arch x86_64 -O3 -I${CONFIG}/inc -Isrc src/deps/est/estLib.c

${DFLAGS}/usr/bin/ar -cr ${CONFIG}/bin/libest.a ${CONFIG}/obj/estLib.o

rm -rf ${CONFIG}/bin/ca.crt
cp -r src/deps/est/ca.crt ${CONFIG}/bin/ca.crt

rm -rf ${CONFIG}/inc/mpr.h
cp -r src/deps/mpr/mpr.h ${CONFIG}/inc/mpr.h

${DFLAGS}${CC} -c -o ${CONFIG}/obj/mprLib.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/deps/mpr/mprLib.c

${DFLAGS}/usr/bin/ar -cr ${CONFIG}/bin/libmpr.a ${CONFIG}/obj/mprLib.o

${DFLAGS}${CC} -c -o ${CONFIG}/obj/mprSsl.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/deps/mpr/mprSsl.c

${DFLAGS}/usr/bin/ar -cr ${CONFIG}/bin/libmprssl.a ${CONFIG}/obj/mprSsl.o

rm -rf ${CONFIG}/inc/bitos.h
cp -r src/bitos.h ${CONFIG}/inc/bitos.h

rm -rf ${CONFIG}/inc/http.h
cp -r src/http.h ${CONFIG}/inc/http.h

${DFLAGS}${CC} -c -o ${CONFIG}/obj/actionHandler.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/actionHandler.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/auth.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/auth.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/basic.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/basic.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/cache.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/cache.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/chunkFilter.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/chunkFilter.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/client.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/client.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/conn.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/conn.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/digest.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/digest.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/endpoint.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/endpoint.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/error.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/error.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/host.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/host.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/httpService.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/httpService.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/log.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/log.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/netConnector.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/netConnector.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/packet.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/packet.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/pam.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/pam.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/passHandler.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/passHandler.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/pipeline.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/pipeline.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/queue.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/queue.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/rangeFilter.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/rangeFilter.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/route.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/route.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/rx.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/rx.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/sendConnector.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/sendConnector.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/session.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/session.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/stage.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/stage.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/trace.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/trace.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/tx.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/tx.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/uploadFilter.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/uploadFilter.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/uri.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/uri.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/var.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/var.c

${DFLAGS}${CC} -c -o ${CONFIG}/obj/webSock.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/webSock.c

${DFLAGS}/usr/bin/ar -cr ${CONFIG}/bin/libhttp.a ${CONFIG}/obj/actionHandler.o ${CONFIG}/obj/auth.o ${CONFIG}/obj/basic.o ${CONFIG}/obj/cache.o ${CONFIG}/obj/chunkFilter.o ${CONFIG}/obj/client.o ${CONFIG}/obj/conn.o ${CONFIG}/obj/digest.o ${CONFIG}/obj/endpoint.o ${CONFIG}/obj/error.o ${CONFIG}/obj/host.o ${CONFIG}/obj/httpService.o ${CONFIG}/obj/log.o ${CONFIG}/obj/netConnector.o ${CONFIG}/obj/packet.o ${CONFIG}/obj/pam.o ${CONFIG}/obj/passHandler.o ${CONFIG}/obj/pipeline.o ${CONFIG}/obj/queue.o ${CONFIG}/obj/rangeFilter.o ${CONFIG}/obj/route.o ${CONFIG}/obj/rx.o ${CONFIG}/obj/sendConnector.o ${CONFIG}/obj/session.o ${CONFIG}/obj/stage.o ${CONFIG}/obj/trace.o ${CONFIG}/obj/tx.o ${CONFIG}/obj/uploadFilter.o ${CONFIG}/obj/uri.o ${CONFIG}/obj/var.o ${CONFIG}/obj/webSock.o

${DFLAGS}${CC} -c -o ${CONFIG}/obj/http.o -arch x86_64 ${CFLAGS} -I${CONFIG}/inc -Isrc src/http.c

${DFLAGS}${CC} -o ${CONFIG}/bin/http -arch x86_64 ${LDFLAGS} ${LIBPATHS} ${CONFIG}/obj/http.o -lhttp ${LIBS} -lmpr

#  Omit build script undefined
