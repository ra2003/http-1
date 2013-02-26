#
#   http-freebsd-default.mk -- Makefile to build Http Library for freebsd
#

PRODUCT         := http
VERSION         := 1.3.0
BUILD_NUMBER    := 0
PROFILE         := default
ARCH            := $(shell uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/')
OS              := freebsd
CC              := /usr/bin/gcc
LD              := /usr/bin/ld
CONFIG          := $(OS)-$(ARCH)-$(PROFILE)
LBIN            := $(CONFIG)/bin

CFLAGS          += -fPIC  -w
DFLAGS          += -D_REENTRANT -DPIC  $(patsubst %,-D%,$(filter BIT_%,$(MAKEFLAGS)))
IFLAGS          += -I$(CONFIG)/inc -Isrc
LDFLAGS         += '-g'
LIBPATHS        += -L$(CONFIG)/bin
LIBS            += -lpthread -lm -ldl

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

BIT_PACK_EST          := 1

BIT_ROOT_PREFIX       := 
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


ifeq ($(BIT_PACK_EST),1)
TARGETS += $(CONFIG)/bin/libest.so
endif
TARGETS     += $(CONFIG)/bin/ca.crt
TARGETS     += $(CONFIG)/bin/libmpr.so
TARGETS     += $(CONFIG)/bin/libmprssl.so
TARGETS     += $(CONFIG)/bin/makerom
TARGETS     += $(CONFIG)/bin/libhttp.so
TARGETS     += $(CONFIG)/bin/http

unexport CDPATH

ifndef SHOW
.SILENT:
endif

all compile: prep $(TARGETS)

.PHONY: prep

prep:
	@echo "      [Info] Use "make SHOW=1" to trace executed commands."
	@if [ "$(CONFIG)" = "" ] ; then echo WARNING: CONFIG not set ; exit 255 ; fi
	@if [ "$(BIT_APP_PREFIX)" = "" ] ; then echo WARNING: BIT_APP_PREFIX not set ; exit 255 ; fi
	@[ ! -x $(CONFIG)/bin ] && mkdir -p $(CONFIG)/bin; true
	@[ ! -x $(CONFIG)/inc ] && mkdir -p $(CONFIG)/inc; true
	@[ ! -x $(CONFIG)/obj ] && mkdir -p $(CONFIG)/obj; true
	@[ ! -f $(CONFIG)/inc/bit.h ] && cp projects/http-freebsd-default-bit.h $(CONFIG)/inc/bit.h ; true
	@[ ! -f $(CONFIG)/inc/bitos.h ] && cp src/bitos.h $(CONFIG)/inc/bitos.h ; true
	@if ! diff $(CONFIG)/inc/bit.h projects/http-freebsd-default-bit.h >/dev/null ; then\
		echo cp projects/http-freebsd-default-bit.h $(CONFIG)/inc/bit.h  ; \
		cp projects/http-freebsd-default-bit.h $(CONFIG)/inc/bit.h  ; \
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

#
#   est.h
#
$(CONFIG)/inc/est.h: $(DEPS_1)
	@echo '      [File] $(CONFIG)/inc/est.h'
	mkdir -p "$(CONFIG)/inc"
	cp "src/deps/est/est.h" "$(CONFIG)/inc/est.h"

#
#   bit.h
#
$(CONFIG)/inc/bit.h: $(DEPS_2)

#
#   bitos.h
#
DEPS_3 += $(CONFIG)/inc/bit.h

src/bitos.h: $(DEPS_3)

#
#   estLib.o
#
DEPS_4 += $(CONFIG)/inc/bit.h
DEPS_4 += $(CONFIG)/inc/est.h
DEPS_4 += src/bitos.h

$(CONFIG)/obj/estLib.o: \
    src/deps/est/estLib.c $(DEPS_4)
	@echo '   [Compile] src/deps/est/estLib.c'
	$(CC) -c -o $(CONFIG)/obj/estLib.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/deps/est/estLib.c

ifeq ($(BIT_PACK_EST),1)
#
#   libest
#
DEPS_5 += $(CONFIG)/inc/est.h
DEPS_5 += $(CONFIG)/obj/estLib.o

$(CONFIG)/bin/libest.so: $(DEPS_5)
	@echo '      [Link] libest'
	$(CC) -shared -o $(CONFIG)/bin/libest.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/estLib.o $(LIBS)
endif

#
#   ca-crt
#
DEPS_6 += src/deps/est/ca.crt

$(CONFIG)/bin/ca.crt: $(DEPS_6)
	@echo '      [File] $(CONFIG)/bin/ca.crt'
	mkdir -p "$(CONFIG)/bin"
	cp "src/deps/est/ca.crt" "$(CONFIG)/bin/ca.crt"

#
#   mpr.h
#
$(CONFIG)/inc/mpr.h: $(DEPS_7)
	@echo '      [File] $(CONFIG)/inc/mpr.h'
	mkdir -p "$(CONFIG)/inc"
	cp "src/deps/mpr/mpr.h" "$(CONFIG)/inc/mpr.h"

#
#   mprLib.o
#
DEPS_8 += $(CONFIG)/inc/bit.h
DEPS_8 += $(CONFIG)/inc/mpr.h
DEPS_8 += src/bitos.h

$(CONFIG)/obj/mprLib.o: \
    src/deps/mpr/mprLib.c $(DEPS_8)
	@echo '   [Compile] src/deps/mpr/mprLib.c'
	$(CC) -c -o $(CONFIG)/obj/mprLib.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/mprLib.c

#
#   libmpr
#
DEPS_9 += $(CONFIG)/inc/mpr.h
DEPS_9 += $(CONFIG)/obj/mprLib.o

$(CONFIG)/bin/libmpr.so: $(DEPS_9)
	@echo '      [Link] libmpr'
	$(CC) -shared -o $(CONFIG)/bin/libmpr.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/mprLib.o $(LIBS)

#
#   mprSsl.o
#
DEPS_10 += $(CONFIG)/inc/bit.h
DEPS_10 += $(CONFIG)/inc/mpr.h
DEPS_10 += $(CONFIG)/inc/est.h

$(CONFIG)/obj/mprSsl.o: \
    src/deps/mpr/mprSsl.c $(DEPS_10)
	@echo '   [Compile] src/deps/mpr/mprSsl.c'
	$(CC) -c -o $(CONFIG)/obj/mprSsl.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/mprSsl.c

#
#   libmprssl
#
DEPS_11 += $(CONFIG)/bin/libmpr.so
ifeq ($(BIT_PACK_EST),1)
    DEPS_11 += $(CONFIG)/bin/libest.so
endif
DEPS_11 += $(CONFIG)/obj/mprSsl.o

ifeq ($(BIT_PACK_EST),1)
    LIBS_11 += -lest
endif
LIBS_11 += -lmpr

$(CONFIG)/bin/libmprssl.so: $(DEPS_11)
	@echo '      [Link] libmprssl'
	$(CC) -shared -o $(CONFIG)/bin/libmprssl.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/mprSsl.o $(LIBS_11) $(LIBS_11) $(LIBS)

#
#   makerom.o
#
DEPS_12 += $(CONFIG)/inc/bit.h
DEPS_12 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/makerom.o: \
    src/deps/mpr/makerom.c $(DEPS_12)
	@echo '   [Compile] src/deps/mpr/makerom.c'
	$(CC) -c -o $(CONFIG)/obj/makerom.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/makerom.c

#
#   makerom
#
DEPS_13 += $(CONFIG)/bin/libmpr.so
DEPS_13 += $(CONFIG)/obj/makerom.o

LIBS_13 += -lmpr

$(CONFIG)/bin/makerom: $(DEPS_13)
	@echo '      [Link] makerom'
	$(CC) -o $(CONFIG)/bin/makerom $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/makerom.o $(LIBS_13) $(LIBS_13) $(LIBS) $(LDFLAGS)

#
#   bitos.h
#
$(CONFIG)/inc/bitos.h: $(DEPS_14)
	@echo '      [File] $(CONFIG)/inc/bitos.h'
	mkdir -p "$(CONFIG)/inc"
	cp "src/bitos.h" "$(CONFIG)/inc/bitos.h"

#
#   http.h
#
$(CONFIG)/inc/http.h: $(DEPS_15)
	@echo '      [File] $(CONFIG)/inc/http.h'
	mkdir -p "$(CONFIG)/inc"
	cp "src/http.h" "$(CONFIG)/inc/http.h"

#
#   http.h
#
src/http.h: $(DEPS_16)

#
#   actionHandler.o
#
DEPS_17 += $(CONFIG)/inc/bit.h
DEPS_17 += src/http.h
DEPS_17 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/actionHandler.o: \
    src/actionHandler.c $(DEPS_17)
	@echo '   [Compile] src/actionHandler.c'
	$(CC) -c -o $(CONFIG)/obj/actionHandler.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/actionHandler.c

#
#   auth.o
#
DEPS_18 += $(CONFIG)/inc/bit.h
DEPS_18 += src/http.h

$(CONFIG)/obj/auth.o: \
    src/auth.c $(DEPS_18)
	@echo '   [Compile] src/auth.c'
	$(CC) -c -o $(CONFIG)/obj/auth.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/auth.c

#
#   basic.o
#
DEPS_19 += $(CONFIG)/inc/bit.h
DEPS_19 += src/http.h

$(CONFIG)/obj/basic.o: \
    src/basic.c $(DEPS_19)
	@echo '   [Compile] src/basic.c'
	$(CC) -c -o $(CONFIG)/obj/basic.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/basic.c

#
#   cache.o
#
DEPS_20 += $(CONFIG)/inc/bit.h
DEPS_20 += src/http.h

$(CONFIG)/obj/cache.o: \
    src/cache.c $(DEPS_20)
	@echo '   [Compile] src/cache.c'
	$(CC) -c -o $(CONFIG)/obj/cache.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/cache.c

#
#   chunkFilter.o
#
DEPS_21 += $(CONFIG)/inc/bit.h
DEPS_21 += src/http.h

$(CONFIG)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_21)
	@echo '   [Compile] src/chunkFilter.c'
	$(CC) -c -o $(CONFIG)/obj/chunkFilter.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/chunkFilter.c

#
#   client.o
#
DEPS_22 += $(CONFIG)/inc/bit.h
DEPS_22 += src/http.h

$(CONFIG)/obj/client.o: \
    src/client.c $(DEPS_22)
	@echo '   [Compile] src/client.c'
	$(CC) -c -o $(CONFIG)/obj/client.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/client.c

#
#   conn.o
#
DEPS_23 += $(CONFIG)/inc/bit.h
DEPS_23 += src/http.h

$(CONFIG)/obj/conn.o: \
    src/conn.c $(DEPS_23)
	@echo '   [Compile] src/conn.c'
	$(CC) -c -o $(CONFIG)/obj/conn.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/conn.c

#
#   digest.o
#
DEPS_24 += $(CONFIG)/inc/bit.h
DEPS_24 += src/http.h

$(CONFIG)/obj/digest.o: \
    src/digest.c $(DEPS_24)
	@echo '   [Compile] src/digest.c'
	$(CC) -c -o $(CONFIG)/obj/digest.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/digest.c

#
#   endpoint.o
#
DEPS_25 += $(CONFIG)/inc/bit.h
DEPS_25 += src/http.h

$(CONFIG)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_25)
	@echo '   [Compile] src/endpoint.c'
	$(CC) -c -o $(CONFIG)/obj/endpoint.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/endpoint.c

#
#   error.o
#
DEPS_26 += $(CONFIG)/inc/bit.h
DEPS_26 += src/http.h

$(CONFIG)/obj/error.o: \
    src/error.c $(DEPS_26)
	@echo '   [Compile] src/error.c'
	$(CC) -c -o $(CONFIG)/obj/error.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/error.c

#
#   host.o
#
DEPS_27 += $(CONFIG)/inc/bit.h
DEPS_27 += src/http.h

$(CONFIG)/obj/host.o: \
    src/host.c $(DEPS_27)
	@echo '   [Compile] src/host.c'
	$(CC) -c -o $(CONFIG)/obj/host.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/host.c

#
#   http.o
#
DEPS_28 += $(CONFIG)/inc/bit.h
DEPS_28 += src/http.h

$(CONFIG)/obj/http.o: \
    src/http.c $(DEPS_28)
	@echo '   [Compile] src/http.c'
	$(CC) -c -o $(CONFIG)/obj/http.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/http.c

#
#   httpService.o
#
DEPS_29 += $(CONFIG)/inc/bit.h
DEPS_29 += src/http.h

$(CONFIG)/obj/httpService.o: \
    src/httpService.c $(DEPS_29)
	@echo '   [Compile] src/httpService.c'
	$(CC) -c -o $(CONFIG)/obj/httpService.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/httpService.c

#
#   log.o
#
DEPS_30 += $(CONFIG)/inc/bit.h
DEPS_30 += src/http.h

$(CONFIG)/obj/log.o: \
    src/log.c $(DEPS_30)
	@echo '   [Compile] src/log.c'
	$(CC) -c -o $(CONFIG)/obj/log.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/log.c

#
#   netConnector.o
#
DEPS_31 += $(CONFIG)/inc/bit.h
DEPS_31 += src/http.h

$(CONFIG)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_31)
	@echo '   [Compile] src/netConnector.c'
	$(CC) -c -o $(CONFIG)/obj/netConnector.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/netConnector.c

#
#   packet.o
#
DEPS_32 += $(CONFIG)/inc/bit.h
DEPS_32 += src/http.h

$(CONFIG)/obj/packet.o: \
    src/packet.c $(DEPS_32)
	@echo '   [Compile] src/packet.c'
	$(CC) -c -o $(CONFIG)/obj/packet.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/packet.c

#
#   pam.o
#
DEPS_33 += $(CONFIG)/inc/bit.h
DEPS_33 += src/http.h

$(CONFIG)/obj/pam.o: \
    src/pam.c $(DEPS_33)
	@echo '   [Compile] src/pam.c'
	$(CC) -c -o $(CONFIG)/obj/pam.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/pam.c

#
#   passHandler.o
#
DEPS_34 += $(CONFIG)/inc/bit.h
DEPS_34 += src/http.h

$(CONFIG)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_34)
	@echo '   [Compile] src/passHandler.c'
	$(CC) -c -o $(CONFIG)/obj/passHandler.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/passHandler.c

#
#   pipeline.o
#
DEPS_35 += $(CONFIG)/inc/bit.h
DEPS_35 += src/http.h

$(CONFIG)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_35)
	@echo '   [Compile] src/pipeline.c'
	$(CC) -c -o $(CONFIG)/obj/pipeline.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/pipeline.c

#
#   queue.o
#
DEPS_36 += $(CONFIG)/inc/bit.h
DEPS_36 += src/http.h

$(CONFIG)/obj/queue.o: \
    src/queue.c $(DEPS_36)
	@echo '   [Compile] src/queue.c'
	$(CC) -c -o $(CONFIG)/obj/queue.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/queue.c

#
#   rangeFilter.o
#
DEPS_37 += $(CONFIG)/inc/bit.h
DEPS_37 += src/http.h

$(CONFIG)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_37)
	@echo '   [Compile] src/rangeFilter.c'
	$(CC) -c -o $(CONFIG)/obj/rangeFilter.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/rangeFilter.c

#
#   route.o
#
DEPS_38 += $(CONFIG)/inc/bit.h
DEPS_38 += src/http.h

$(CONFIG)/obj/route.o: \
    src/route.c $(DEPS_38)
	@echo '   [Compile] src/route.c'
	$(CC) -c -o $(CONFIG)/obj/route.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/route.c

#
#   rx.o
#
DEPS_39 += $(CONFIG)/inc/bit.h
DEPS_39 += src/http.h

$(CONFIG)/obj/rx.o: \
    src/rx.c $(DEPS_39)
	@echo '   [Compile] src/rx.c'
	$(CC) -c -o $(CONFIG)/obj/rx.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/rx.c

#
#   sendConnector.o
#
DEPS_40 += $(CONFIG)/inc/bit.h
DEPS_40 += src/http.h

$(CONFIG)/obj/sendConnector.o: \
    src/sendConnector.c $(DEPS_40)
	@echo '   [Compile] src/sendConnector.c'
	$(CC) -c -o $(CONFIG)/obj/sendConnector.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/sendConnector.c

#
#   session.o
#
DEPS_41 += $(CONFIG)/inc/bit.h
DEPS_41 += src/http.h

$(CONFIG)/obj/session.o: \
    src/session.c $(DEPS_41)
	@echo '   [Compile] src/session.c'
	$(CC) -c -o $(CONFIG)/obj/session.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/session.c

#
#   stage.o
#
DEPS_42 += $(CONFIG)/inc/bit.h
DEPS_42 += src/http.h

$(CONFIG)/obj/stage.o: \
    src/stage.c $(DEPS_42)
	@echo '   [Compile] src/stage.c'
	$(CC) -c -o $(CONFIG)/obj/stage.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/stage.c

#
#   trace.o
#
DEPS_43 += $(CONFIG)/inc/bit.h
DEPS_43 += src/http.h

$(CONFIG)/obj/trace.o: \
    src/trace.c $(DEPS_43)
	@echo '   [Compile] src/trace.c'
	$(CC) -c -o $(CONFIG)/obj/trace.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/trace.c

#
#   tx.o
#
DEPS_44 += $(CONFIG)/inc/bit.h
DEPS_44 += src/http.h

$(CONFIG)/obj/tx.o: \
    src/tx.c $(DEPS_44)
	@echo '   [Compile] src/tx.c'
	$(CC) -c -o $(CONFIG)/obj/tx.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/tx.c

#
#   uploadFilter.o
#
DEPS_45 += $(CONFIG)/inc/bit.h
DEPS_45 += src/http.h

$(CONFIG)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_45)
	@echo '   [Compile] src/uploadFilter.c'
	$(CC) -c -o $(CONFIG)/obj/uploadFilter.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/uploadFilter.c

#
#   uri.o
#
DEPS_46 += $(CONFIG)/inc/bit.h
DEPS_46 += src/http.h

$(CONFIG)/obj/uri.o: \
    src/uri.c $(DEPS_46)
	@echo '   [Compile] src/uri.c'
	$(CC) -c -o $(CONFIG)/obj/uri.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/uri.c

#
#   var.o
#
DEPS_47 += $(CONFIG)/inc/bit.h
DEPS_47 += src/http.h

$(CONFIG)/obj/var.o: \
    src/var.c $(DEPS_47)
	@echo '   [Compile] src/var.c'
	$(CC) -c -o $(CONFIG)/obj/var.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/var.c

#
#   webSock.o
#
DEPS_48 += $(CONFIG)/inc/bit.h
DEPS_48 += src/http.h

$(CONFIG)/obj/webSock.o: \
    src/webSock.c $(DEPS_48)
	@echo '   [Compile] src/webSock.c'
	$(CC) -c -o $(CONFIG)/obj/webSock.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/webSock.c

#
#   libhttp
#
DEPS_49 += $(CONFIG)/bin/libmpr.so
DEPS_49 += $(CONFIG)/inc/bitos.h
DEPS_49 += $(CONFIG)/inc/http.h
DEPS_49 += $(CONFIG)/obj/actionHandler.o
DEPS_49 += $(CONFIG)/obj/auth.o
DEPS_49 += $(CONFIG)/obj/basic.o
DEPS_49 += $(CONFIG)/obj/cache.o
DEPS_49 += $(CONFIG)/obj/chunkFilter.o
DEPS_49 += $(CONFIG)/obj/client.o
DEPS_49 += $(CONFIG)/obj/conn.o
DEPS_49 += $(CONFIG)/obj/digest.o
DEPS_49 += $(CONFIG)/obj/endpoint.o
DEPS_49 += $(CONFIG)/obj/error.o
DEPS_49 += $(CONFIG)/obj/host.o
DEPS_49 += $(CONFIG)/obj/http.o
DEPS_49 += $(CONFIG)/obj/httpService.o
DEPS_49 += $(CONFIG)/obj/log.o
DEPS_49 += $(CONFIG)/obj/netConnector.o
DEPS_49 += $(CONFIG)/obj/packet.o
DEPS_49 += $(CONFIG)/obj/pam.o
DEPS_49 += $(CONFIG)/obj/passHandler.o
DEPS_49 += $(CONFIG)/obj/pipeline.o
DEPS_49 += $(CONFIG)/obj/queue.o
DEPS_49 += $(CONFIG)/obj/rangeFilter.o
DEPS_49 += $(CONFIG)/obj/route.o
DEPS_49 += $(CONFIG)/obj/rx.o
DEPS_49 += $(CONFIG)/obj/sendConnector.o
DEPS_49 += $(CONFIG)/obj/session.o
DEPS_49 += $(CONFIG)/obj/stage.o
DEPS_49 += $(CONFIG)/obj/trace.o
DEPS_49 += $(CONFIG)/obj/tx.o
DEPS_49 += $(CONFIG)/obj/uploadFilter.o
DEPS_49 += $(CONFIG)/obj/uri.o
DEPS_49 += $(CONFIG)/obj/var.o
DEPS_49 += $(CONFIG)/obj/webSock.o

LIBS_49 += -lmpr

$(CONFIG)/bin/libhttp.so: $(DEPS_49)
	@echo '      [Link] libhttp'
	$(CC) -shared -o $(CONFIG)/bin/libhttp.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/actionHandler.o $(CONFIG)/obj/auth.o $(CONFIG)/obj/basic.o $(CONFIG)/obj/cache.o $(CONFIG)/obj/chunkFilter.o $(CONFIG)/obj/client.o $(CONFIG)/obj/conn.o $(CONFIG)/obj/digest.o $(CONFIG)/obj/endpoint.o $(CONFIG)/obj/error.o $(CONFIG)/obj/host.o $(CONFIG)/obj/http.o $(CONFIG)/obj/httpService.o $(CONFIG)/obj/log.o $(CONFIG)/obj/netConnector.o $(CONFIG)/obj/packet.o $(CONFIG)/obj/pam.o $(CONFIG)/obj/passHandler.o $(CONFIG)/obj/pipeline.o $(CONFIG)/obj/queue.o $(CONFIG)/obj/rangeFilter.o $(CONFIG)/obj/route.o $(CONFIG)/obj/rx.o $(CONFIG)/obj/sendConnector.o $(CONFIG)/obj/session.o $(CONFIG)/obj/stage.o $(CONFIG)/obj/trace.o $(CONFIG)/obj/tx.o $(CONFIG)/obj/uploadFilter.o $(CONFIG)/obj/uri.o $(CONFIG)/obj/var.o $(CONFIG)/obj/webSock.o $(LIBS_49) $(LIBS_49) $(LIBS)

#
#   http
#
DEPS_50 += $(CONFIG)/bin/libhttp.so
DEPS_50 += $(CONFIG)/obj/http.o

LIBS_50 += -lhttp
LIBS_50 += -lmpr

$(CONFIG)/bin/http: $(DEPS_50)
	@echo '      [Link] http'
	$(CC) -o $(CONFIG)/bin/http $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/http.o $(LIBS_50) $(LIBS_50) $(LIBS) -lmpr $(LDFLAGS)

#
#   version
#
version: $(DEPS_51)
	@echo 1.3.0-0

#
#   stop
#
stop: $(DEPS_52)
	

#
#   installBinary
#
DEPS_53 += stop

installBinary: $(DEPS_53)

#
#   start
#
start: $(DEPS_54)
	

#
#   install
#
DEPS_55 += stop
DEPS_55 += installBinary
DEPS_55 += start

install: $(DEPS_55)
	

#
#   uninstall
#
DEPS_56 += stop

uninstall: $(DEPS_56)

