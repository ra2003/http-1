#
#   http-solaris-default.sh -- Build It Shell Script to build Http Library
#

PRODUCT="http"
VERSION="1.3.0"
BUILD_NUMBER="0"
PROFILE="default"
ARCH="x86"
ARCH="`uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/'`"
OS="solaris"
CONFIG="${OS}-${ARCH}-${PROFILE}"
CC="/usr/bin/gcc"
LD="/usr/bin/ld"
CFLAGS="-fPIC -O2 -w"
DFLAGS="-D_REENTRANT -DPIC"
IFLAGS="-I${CONFIG}/inc -Isrc"
LDFLAGS=""
LIBPATHS="-L${CONFIG}/bin"
LIBS="-llxnet -lrt -lsocket -lpthread -lm -ldl"

[ ! -x ${CONFIG}/inc ] && mkdir -p ${CONFIG}/inc ${CONFIG}/obj ${CONFIG}/lib ${CONFIG}/bin

[ ! -f ${CONFIG}/inc/bit.h ] && cp projects/http-${OS}-${PROFILE}-bit.h ${CONFIG}/inc/bit.h
[ ! -f ${CONFIG}/inc/bitos.h ] && cp ${SRC}/src/bitos.h ${CONFIG}/inc/bitos.h
if ! diff ${CONFIG}/inc/bit.h projects/http-${OS}-${PROFILE}-bit.h >/dev/null ; then
	cp projects/http-${OS}-${PROFILE}-bit.h ${CONFIG}/inc/bit.h
fi

rm -rf ${CONFIG}/inc/est.h
cp -r src/deps/est/est.h ${CONFIG}/inc/est.h

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/estLib.o -fPIC -O2 ${DFLAGS} -I${CONFIG}/inc -Isrc src/deps/est/estLib.c

${LDFLAGS}${LDFLAGS}${CC} -shared -o ${CONFIG}/bin/libest.so ${LIBPATHS} ${CONFIG}/obj/estLib.o ${LIBS}

rm -rf ${CONFIG}/bin/ca.crt
cp -r src/deps/est/ca.crt ${CONFIG}/bin/ca.crt

rm -rf ${CONFIG}/inc/mpr.h
cp -r src/deps/mpr/mpr.h ${CONFIG}/inc/mpr.h

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/mprLib.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/deps/mpr/mprLib.c

${LDFLAGS}${LDFLAGS}${CC} -shared -o ${CONFIG}/bin/libmpr.so ${LIBPATHS} ${CONFIG}/obj/mprLib.o ${LIBS}

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/mprSsl.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/deps/mpr/mprSsl.c

${LDFLAGS}${LDFLAGS}${CC} -shared -o ${CONFIG}/bin/libmprssl.so ${LIBPATHS} ${CONFIG}/obj/mprSsl.o -lest -lmpr ${LIBS}

rm -rf ${CONFIG}/inc/bitos.h
cp -r src/bitos.h ${CONFIG}/inc/bitos.h

rm -rf ${CONFIG}/inc/http.h
cp -r src/http.h ${CONFIG}/inc/http.h

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/actionHandler.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/actionHandler.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/auth.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/auth.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/basic.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/basic.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/cache.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/cache.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/chunkFilter.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/chunkFilter.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/client.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/client.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/conn.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/conn.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/digest.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/digest.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/endpoint.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/endpoint.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/error.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/error.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/host.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/host.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/httpService.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/httpService.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/log.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/log.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/netConnector.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/netConnector.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/packet.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/packet.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/pam.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/pam.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/passHandler.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/passHandler.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/pipeline.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/pipeline.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/queue.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/queue.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/rangeFilter.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/rangeFilter.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/route.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/route.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/rx.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/rx.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/sendConnector.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/sendConnector.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/session.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/session.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/stage.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/stage.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/trace.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/trace.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/tx.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/tx.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/uploadFilter.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/uploadFilter.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/uri.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/uri.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/var.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/var.c

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/webSock.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/webSock.c

${LDFLAGS}${LDFLAGS}${CC} -shared -o ${CONFIG}/bin/libhttp.so ${LIBPATHS} ${CONFIG}/obj/actionHandler.o ${CONFIG}/obj/auth.o ${CONFIG}/obj/basic.o ${CONFIG}/obj/cache.o ${CONFIG}/obj/chunkFilter.o ${CONFIG}/obj/client.o ${CONFIG}/obj/conn.o ${CONFIG}/obj/digest.o ${CONFIG}/obj/endpoint.o ${CONFIG}/obj/error.o ${CONFIG}/obj/host.o ${CONFIG}/obj/httpService.o ${CONFIG}/obj/log.o ${CONFIG}/obj/netConnector.o ${CONFIG}/obj/packet.o ${CONFIG}/obj/pam.o ${CONFIG}/obj/passHandler.o ${CONFIG}/obj/pipeline.o ${CONFIG}/obj/queue.o ${CONFIG}/obj/rangeFilter.o ${CONFIG}/obj/route.o ${CONFIG}/obj/rx.o ${CONFIG}/obj/sendConnector.o ${CONFIG}/obj/session.o ${CONFIG}/obj/stage.o ${CONFIG}/obj/trace.o ${CONFIG}/obj/tx.o ${CONFIG}/obj/uploadFilter.o ${CONFIG}/obj/uri.o ${CONFIG}/obj/var.o ${CONFIG}/obj/webSock.o -lmpr ${LIBS}

${LDFLAGS}${LDFLAGS}${CC} -c -o ${CONFIG}/obj/http.o ${CFLAGS} ${DFLAGS} -I${CONFIG}/inc -Isrc src/http.c

${LDFLAGS}${LDFLAGS}${CC} -o ${CONFIG}/bin/http ${LIBPATHS} ${CONFIG}/obj/http.o -lhttp ${LIBS} -lmpr -lhttp -llxnet -lrt -lsocket -lpthread -lm -ldl -lmpr 

#  Omit build script undefined
