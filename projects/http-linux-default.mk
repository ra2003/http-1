#
#   http-linux-default.mk -- Makefile to build Http Library for linux
#

PRODUCT         := http
VERSION         := 1.3.0
BUILD_NUMBER    := 0
PROFILE         := default
ARCH            := $(shell uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/')
OS              := linux
CC              := /usr/bin/gcc
LD              := /usr/bin/ld
CONFIG          := $(OS)-$(ARCH)-$(PROFILE)
LBIN            := $(CONFIG)/bin

BIT_ROOT_PREFIX       := /
BIT_BASE_PREFIX       := $(BIT_ROOT_PREFIX)/usr/local
BIT_DATA_PREFIX       := $(BIT_ROOT_PREFIX)/
BIT_STATE_PREFIX      := $(BIT_ROOT_PREFIX)/var
BIT_APP_PREFIX        := $(BIT_BASE_PREFIX)/lib/$(PRODUCT)
BIT_VAPP_PREFIX       := $(BIT_APP_PREFIX)/$(VERSION)
BIT_BIN_PREFIX        := $(BIT_ROOT_PREFIX)/usr/local/bin
BIT_INC_PREFIX        := $(BIT_ROOT_PREFIX)/usr/local/include
BIT_LIB_PREFIX        := $(BIT_ROOT_PREFIX)/usr/local/lib
BIT_MAN_PREFIX        := $(BIT_ROOT_PREFIX)/usr/local/share/man
BIT_SBIN_PREFIX       := $(BIT_ROOT_PREFIX)/usr/local/sbin
BIT_ETC_PREFIX        := $(BIT_ROOT_PREFIX)/etc/$(PRODUCT)
BIT_WEB_PREFIX        := $(BIT_ROOT_PREFIX)/var/www/$(PRODUCT)-default
BIT_LOG_PREFIX        := $(BIT_ROOT_PREFIX)/var/log/$(PRODUCT)
BIT_SPOOL_PREFIX      := $(BIT_ROOT_PREFIX)/var/spool/$(PRODUCT)
BIT_CACHE_PREFIX      := $(BIT_ROOT_PREFIX)/var/spool/$(PRODUCT)/cache
BIT_SRC_PREFIX        := $(BIT_ROOT_PREFIX)$(PRODUCT)-$(VERSION)

CFLAGS          += -fPIC   -w
DFLAGS          += -D_REENTRANT -DPIC  $(patsubst %,-D%,$(filter BIT_%,$(MAKEFLAGS)))
IFLAGS          += -I$(CONFIG)/inc -Isrc
LDFLAGS         += '-Wl,--enable-new-dtags' '-Wl,-rpath,$$ORIGIN/' '-rdynamic'
LIBPATHS        += -L$(CONFIG)/bin
LIBS            += -lpthread -lm -lrt -ldl

DEBUG           := debug
CFLAGS-debug    := -g
DFLAGS-debug    := -DBIT_DEBUG
LDFLAGS-debug   := -g
DFLAGS-release  := 
CFLAGS-release  := -O2
LDFLAGS-release := 
CFLAGS          += $(CFLAGS-$(DEBUG))
DFLAGS          += $(DFLAGS-$(DEBUG))
LDFLAGS         += $(LDFLAGS-$(DEBUG))

unexport CDPATH

all compile: prep \
        $(CONFIG)/bin/libest.so \
        $(CONFIG)/bin/ca.crt \
        $(CONFIG)/bin/libmpr.so \
        $(CONFIG)/bin/libmprssl.so \
        $(CONFIG)/bin/makerom \
        $(CONFIG)/bin/libhttp.so \
        $(CONFIG)/bin/http

.PHONY: prep

prep:
	@if [ "$(CONFIG)" = "" ] ; then echo WARNING: CONFIG not set ; exit 255 ; fi
	@if [ "$(BIT_APP_PREFIX)" = "" ] ; then echo WARNING: BIT_APP_PREFIX not set ; exit 255 ; fi
	@[ ! -x $(CONFIG)/bin ] && mkdir -p $(CONFIG)/bin; true
	@[ ! -x $(CONFIG)/inc ] && mkdir -p $(CONFIG)/inc; true
	@[ ! -x $(CONFIG)/obj ] && mkdir -p $(CONFIG)/obj; true
	@[ ! -f $(CONFIG)/inc/bit.h ] && cp projects/http-linux-default-bit.h $(CONFIG)/inc/bit.h ; true
	@[ ! -f $(CONFIG)/inc/bitos.h ] && cp src/bitos.h $(CONFIG)/inc/bitos.h ; true
	@if ! diff $(CONFIG)/inc/bit.h projects/http-linux-default-bit.h >/dev/null ; then\
		echo cp projects/http-linux-default-bit.h $(CONFIG)/inc/bit.h  ; \
		cp projects/http-linux-default-bit.h $(CONFIG)/inc/bit.h  ; \
	fi; true

clean:
	rm -rf $(CONFIG)/bin/libest.so
	rm -rf $(CONFIG)/bin/ca.crt
	rm -rf $(CONFIG)/bin/libmpr.so
	rm -rf $(CONFIG)/bin/libmprssl.so
	rm -rf $(CONFIG)/bin/makerom
	rm -rf $(CONFIG)/bin/libhttp.so
	rm -rf $(CONFIG)/bin/http
	rm -rf $(CONFIG)/obj/estLib.o
	rm -rf $(CONFIG)/obj/mprLib.o
	rm -rf $(CONFIG)/obj/mprSsl.o
	rm -rf $(CONFIG)/obj/makerom.o
	rm -rf $(CONFIG)/obj/actionHandler.o
	rm -rf $(CONFIG)/obj/auth.o
	rm -rf $(CONFIG)/obj/basic.o
	rm -rf $(CONFIG)/obj/cache.o
	rm -rf $(CONFIG)/obj/chunkFilter.o
	rm -rf $(CONFIG)/obj/client.o
	rm -rf $(CONFIG)/obj/conn.o
	rm -rf $(CONFIG)/obj/digest.o
	rm -rf $(CONFIG)/obj/endpoint.o
	rm -rf $(CONFIG)/obj/error.o
	rm -rf $(CONFIG)/obj/host.o
	rm -rf $(CONFIG)/obj/http.o
	rm -rf $(CONFIG)/obj/httpService.o
	rm -rf $(CONFIG)/obj/log.o
	rm -rf $(CONFIG)/obj/netConnector.o
	rm -rf $(CONFIG)/obj/packet.o
	rm -rf $(CONFIG)/obj/pam.o
	rm -rf $(CONFIG)/obj/passHandler.o
	rm -rf $(CONFIG)/obj/pipeline.o
	rm -rf $(CONFIG)/obj/queue.o
	rm -rf $(CONFIG)/obj/rangeFilter.o
	rm -rf $(CONFIG)/obj/route.o
	rm -rf $(CONFIG)/obj/rx.o
	rm -rf $(CONFIG)/obj/sendConnector.o
	rm -rf $(CONFIG)/obj/session.o
	rm -rf $(CONFIG)/obj/stage.o
	rm -rf $(CONFIG)/obj/trace.o
	rm -rf $(CONFIG)/obj/tx.o
	rm -rf $(CONFIG)/obj/uploadFilter.o
	rm -rf $(CONFIG)/obj/uri.o
	rm -rf $(CONFIG)/obj/var.o
	rm -rf $(CONFIG)/obj/webSock.o

clobber: clean
	rm -fr ./$(CONFIG)

$(CONFIG)/inc/est.h: 
	mkdir -p "$(CONFIG)/inc"
	cp "src/deps/est/est.h" "$(CONFIG)/inc/est.h"

$(CONFIG)/inc/bit.h: 

src/bitos.h: \
    $(CONFIG)/inc/bit.h

$(CONFIG)/obj/estLib.o: \
    src/deps/est/estLib.c\
    $(CONFIG)/inc/bit.h \
    $(CONFIG)/inc/est.h \
    src/bitos.h
	$(CC) -c -o $(CONFIG)/obj/estLib.o -fPIC $(DFLAGS) $(IFLAGS) src/deps/est/estLib.c

$(CONFIG)/bin/libest.so: \
    $(CONFIG)/inc/est.h \
    $(CONFIG)/obj/estLib.o
	$(CC) -shared -o $(CONFIG)/bin/libest.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/estLib.o $(LIBS)

$(CONFIG)/bin/ca.crt: \
    src/deps/est/ca.crt
	mkdir -p "$(CONFIG)/bin"
	cp "src/deps/est/ca.crt" "$(CONFIG)/bin/ca.crt"

$(CONFIG)/inc/mpr.h: 
	mkdir -p "$(CONFIG)/inc"
	cp "src/deps/mpr/mpr.h" "$(CONFIG)/inc/mpr.h"

$(CONFIG)/obj/mprLib.o: \
    src/deps/mpr/mprLib.c\
    $(CONFIG)/inc/bit.h \
    $(CONFIG)/inc/mpr.h \
    src/bitos.h
	$(CC) -c -o $(CONFIG)/obj/mprLib.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/mprLib.c

$(CONFIG)/bin/libmpr.so: \
    $(CONFIG)/inc/mpr.h \
    $(CONFIG)/obj/mprLib.o
	$(CC) -shared -o $(CONFIG)/bin/libmpr.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/mprLib.o $(LIBS)

$(CONFIG)/obj/mprSsl.o: \
    src/deps/mpr/mprSsl.c\
    $(CONFIG)/inc/bit.h \
    $(CONFIG)/inc/mpr.h \
    $(CONFIG)/inc/est.h
	$(CC) -c -o $(CONFIG)/obj/mprSsl.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/mprSsl.c

$(CONFIG)/bin/libmprssl.so: \
    $(CONFIG)/bin/libmpr.so \
    $(CONFIG)/bin/libest.so \
    $(CONFIG)/obj/mprSsl.o
	$(CC) -shared -o $(CONFIG)/bin/libmprssl.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/mprSsl.o -lest -lmpr $(LIBS)

$(CONFIG)/obj/makerom.o: \
    src/deps/mpr/makerom.c\
    $(CONFIG)/inc/bit.h \
    $(CONFIG)/inc/mpr.h
	$(CC) -c -o $(CONFIG)/obj/makerom.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/makerom.c

$(CONFIG)/bin/makerom: \
    $(CONFIG)/bin/libmpr.so \
    $(CONFIG)/obj/makerom.o
	$(CC) -o $(CONFIG)/bin/makerom $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/makerom.o -lmpr $(LIBS) -lmpr -lpthread -lm -lrt -ldl $(LDFLAGS)

$(CONFIG)/inc/bitos.h: 
	mkdir -p "$(CONFIG)/inc"
	cp "src/bitos.h" "$(CONFIG)/inc/bitos.h"

$(CONFIG)/inc/http.h: 
	mkdir -p "$(CONFIG)/inc"
	cp "src/http.h" "$(CONFIG)/inc/http.h"

src/http.h: 

$(CONFIG)/obj/actionHandler.o: \
    src/actionHandler.c\
    $(CONFIG)/inc/bit.h \
    src/http.h \
    $(CONFIG)/inc/mpr.h
	$(CC) -c -o $(CONFIG)/obj/actionHandler.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/actionHandler.c

$(CONFIG)/obj/auth.o: \
    src/auth.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/auth.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/auth.c

$(CONFIG)/obj/basic.o: \
    src/basic.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/basic.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/basic.c

$(CONFIG)/obj/cache.o: \
    src/cache.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/cache.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/cache.c

$(CONFIG)/obj/chunkFilter.o: \
    src/chunkFilter.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/chunkFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/chunkFilter.c

$(CONFIG)/obj/client.o: \
    src/client.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/client.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/client.c

$(CONFIG)/obj/conn.o: \
    src/conn.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/conn.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/conn.c

$(CONFIG)/obj/digest.o: \
    src/digest.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/digest.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/digest.c

$(CONFIG)/obj/endpoint.o: \
    src/endpoint.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/endpoint.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/endpoint.c

$(CONFIG)/obj/error.o: \
    src/error.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/error.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/error.c

$(CONFIG)/obj/host.o: \
    src/host.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/host.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/host.c

$(CONFIG)/obj/http.o: \
    src/http.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/http.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/http.c

$(CONFIG)/obj/httpService.o: \
    src/httpService.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/httpService.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/httpService.c

$(CONFIG)/obj/log.o: \
    src/log.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/log.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/log.c

$(CONFIG)/obj/netConnector.o: \
    src/netConnector.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/netConnector.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/netConnector.c

$(CONFIG)/obj/packet.o: \
    src/packet.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/packet.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/packet.c

$(CONFIG)/obj/pam.o: \
    src/pam.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/pam.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/pam.c

$(CONFIG)/obj/passHandler.o: \
    src/passHandler.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/passHandler.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/passHandler.c

$(CONFIG)/obj/pipeline.o: \
    src/pipeline.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/pipeline.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/pipeline.c

$(CONFIG)/obj/queue.o: \
    src/queue.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/queue.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/queue.c

$(CONFIG)/obj/rangeFilter.o: \
    src/rangeFilter.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/rangeFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/rangeFilter.c

$(CONFIG)/obj/route.o: \
    src/route.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/route.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/route.c

$(CONFIG)/obj/rx.o: \
    src/rx.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/rx.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/rx.c

$(CONFIG)/obj/sendConnector.o: \
    src/sendConnector.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/sendConnector.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/sendConnector.c

$(CONFIG)/obj/session.o: \
    src/session.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/session.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/session.c

$(CONFIG)/obj/stage.o: \
    src/stage.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/stage.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/stage.c

$(CONFIG)/obj/trace.o: \
    src/trace.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/trace.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/trace.c

$(CONFIG)/obj/tx.o: \
    src/tx.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/tx.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/tx.c

$(CONFIG)/obj/uploadFilter.o: \
    src/uploadFilter.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/uploadFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/uploadFilter.c

$(CONFIG)/obj/uri.o: \
    src/uri.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/uri.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/uri.c

$(CONFIG)/obj/var.o: \
    src/var.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/var.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/var.c

$(CONFIG)/obj/webSock.o: \
    src/webSock.c\
    $(CONFIG)/inc/bit.h \
    src/http.h
	$(CC) -c -o $(CONFIG)/obj/webSock.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/webSock.c

$(CONFIG)/bin/libhttp.so: \
    $(CONFIG)/bin/libmpr.so \
    $(CONFIG)/inc/bitos.h \
    $(CONFIG)/inc/http.h \
    $(CONFIG)/obj/actionHandler.o \
    $(CONFIG)/obj/auth.o \
    $(CONFIG)/obj/basic.o \
    $(CONFIG)/obj/cache.o \
    $(CONFIG)/obj/chunkFilter.o \
    $(CONFIG)/obj/client.o \
    $(CONFIG)/obj/conn.o \
    $(CONFIG)/obj/digest.o \
    $(CONFIG)/obj/endpoint.o \
    $(CONFIG)/obj/error.o \
    $(CONFIG)/obj/host.o \
    $(CONFIG)/obj/http.o \
    $(CONFIG)/obj/httpService.o \
    $(CONFIG)/obj/log.o \
    $(CONFIG)/obj/netConnector.o \
    $(CONFIG)/obj/packet.o \
    $(CONFIG)/obj/pam.o \
    $(CONFIG)/obj/passHandler.o \
    $(CONFIG)/obj/pipeline.o \
    $(CONFIG)/obj/queue.o \
    $(CONFIG)/obj/rangeFilter.o \
    $(CONFIG)/obj/route.o \
    $(CONFIG)/obj/rx.o \
    $(CONFIG)/obj/sendConnector.o \
    $(CONFIG)/obj/session.o \
    $(CONFIG)/obj/stage.o \
    $(CONFIG)/obj/trace.o \
    $(CONFIG)/obj/tx.o \
    $(CONFIG)/obj/uploadFilter.o \
    $(CONFIG)/obj/uri.o \
    $(CONFIG)/obj/var.o \
    $(CONFIG)/obj/webSock.o
	$(CC) -shared -o $(CONFIG)/bin/libhttp.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/actionHandler.o $(CONFIG)/obj/auth.o $(CONFIG)/obj/basic.o $(CONFIG)/obj/cache.o $(CONFIG)/obj/chunkFilter.o $(CONFIG)/obj/client.o $(CONFIG)/obj/conn.o $(CONFIG)/obj/digest.o $(CONFIG)/obj/endpoint.o $(CONFIG)/obj/error.o $(CONFIG)/obj/host.o $(CONFIG)/obj/http.o $(CONFIG)/obj/httpService.o $(CONFIG)/obj/log.o $(CONFIG)/obj/netConnector.o $(CONFIG)/obj/packet.o $(CONFIG)/obj/pam.o $(CONFIG)/obj/passHandler.o $(CONFIG)/obj/pipeline.o $(CONFIG)/obj/queue.o $(CONFIG)/obj/rangeFilter.o $(CONFIG)/obj/route.o $(CONFIG)/obj/rx.o $(CONFIG)/obj/sendConnector.o $(CONFIG)/obj/session.o $(CONFIG)/obj/stage.o $(CONFIG)/obj/trace.o $(CONFIG)/obj/tx.o $(CONFIG)/obj/uploadFilter.o $(CONFIG)/obj/uri.o $(CONFIG)/obj/var.o $(CONFIG)/obj/webSock.o -lmpr $(LIBS)

$(CONFIG)/bin/http: \
    $(CONFIG)/bin/libhttp.so \
    $(CONFIG)/obj/http.o
	$(CC) -o $(CONFIG)/bin/http $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/http.o -lhttp $(LIBS) -lmpr -lhttp -lpthread -lm -lrt -ldl -lmpr $(LDFLAGS)

version: 
	@echo 1.3.0-0

stop: 
	

installBinary: stop


start: 
	

install: stop installBinary start
	

uninstall: stop


