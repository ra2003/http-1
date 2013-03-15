#
#   http-freebsd-default.mk -- Makefile to build Http Library for freebsd
#

PRODUCT           := http
VERSION           := 1.3.0
BUILD_NUMBER      := 0
PROFILE           := default
ARCH              := $(shell uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/')
OS                := freebsd
CC                := /usr/bin/gcc
LD                := /usr/bin/ld
CONFIG            := $(OS)-$(ARCH)-$(PROFILE)
LBIN              := $(CONFIG)/bin

BIT_PACK_EST      := 1
BIT_PACK_SSL      := 1

CFLAGS            += -fPIC  -w
DFLAGS            += -D_REENTRANT -DPIC  $(patsubst %,-D%,$(filter BIT_%,$(MAKEFLAGS))) -DBIT_PACK_EST=$(BIT_PACK_EST) -DBIT_PACK_SSL=$(BIT_PACK_SSL) 
IFLAGS            += -I$(CONFIG)/inc -Isrc
LDFLAGS           += '-g'
LIBPATHS          += -L$(CONFIG)/bin
LIBS              += -lpthread -lm -ldl

DEBUG             := debug
CFLAGS-debug      := -g
DFLAGS-debug      := -DBIT_DEBUG
LDFLAGS-debug     := -g
DFLAGS-release    := 
CFLAGS-release    := -O2
LDFLAGS-release   := 
CFLAGS            += $(CFLAGS-$(DEBUG))
DFLAGS            += $(DFLAGS-$(DEBUG))
LDFLAGS           += $(LDFLAGS-$(DEBUG))

BIT_ROOT_PREFIX   := 
BIT_BASE_PREFIX   := $(BIT_ROOT_PREFIX)/usr/local
BIT_DATA_PREFIX   := $(BIT_ROOT_PREFIX)/
BIT_STATE_PREFIX  := $(BIT_ROOT_PREFIX)/var
BIT_APP_PREFIX    := $(BIT_BASE_PREFIX)/lib/$(PRODUCT)
BIT_VAPP_PREFIX   := $(BIT_APP_PREFIX)/$(VERSION)
BIT_BIN_PREFIX    := $(BIT_ROOT_PREFIX)/usr/local/bin
BIT_INC_PREFIX    := $(BIT_ROOT_PREFIX)/usr/local/include
BIT_LIB_PREFIX    := $(BIT_ROOT_PREFIX)/usr/local/lib
BIT_MAN_PREFIX    := $(BIT_ROOT_PREFIX)/usr/local/share/man
BIT_SBIN_PREFIX   := $(BIT_ROOT_PREFIX)/usr/local/sbin
BIT_ETC_PREFIX    := $(BIT_ROOT_PREFIX)/etc/$(PRODUCT)
BIT_WEB_PREFIX    := $(BIT_ROOT_PREFIX)/var/www/$(PRODUCT)-default
BIT_LOG_PREFIX    := $(BIT_ROOT_PREFIX)/var/log/$(PRODUCT)
BIT_SPOOL_PREFIX  := $(BIT_ROOT_PREFIX)/var/spool/$(PRODUCT)
BIT_CACHE_PREFIX  := $(BIT_ROOT_PREFIX)/var/spool/$(PRODUCT)/cache
BIT_SRC_PREFIX    := $(BIT_ROOT_PREFIX)$(PRODUCT)-$(VERSION)


ifeq ($(BIT_PACK_EST),1)
TARGETS           += $(CONFIG)/bin/libest.so
endif
TARGETS           += $(CONFIG)/bin/ca.crt
TARGETS           += $(CONFIG)/bin/libmpr.so
ifeq ($(BIT_PACK_SSL),1)
TARGETS           += $(CONFIG)/bin/libmprssl.so
endif
TARGETS           += $(CONFIG)/bin/makerom
TARGETS           += $(CONFIG)/bin/libhttp.so
TARGETS           += $(CONFIG)/bin/http

unexport CDPATH

ifndef SHOW
.SILENT:
endif

all build compile: prep $(TARGETS)

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
		cp projects/http-freebsd-default-bit.h $(CONFIG)/inc/bit.h  ; \
	fi; true

clean:
	rm -f "$(CONFIG)/bin/libest.so"
	rm -f "$(CONFIG)/bin/ca.crt"
	rm -f "$(CONFIG)/bin/libmpr.so"
	rm -f "$(CONFIG)/bin/libmprssl.so"
	rm -f "$(CONFIG)/bin/makerom"
	rm -f "$(CONFIG)/bin/libhttp.so"
	rm -f "$(CONFIG)/bin/http"
	rm -f "$(CONFIG)/obj/estLib.o"
	rm -f "$(CONFIG)/obj/mprLib.o"
	rm -f "$(CONFIG)/obj/mprSsl.o"
	rm -f "$(CONFIG)/obj/makerom.o"
	rm -f "$(CONFIG)/obj/actionHandler.o"
	rm -f "$(CONFIG)/obj/auth.o"
	rm -f "$(CONFIG)/obj/basic.o"
	rm -f "$(CONFIG)/obj/cache.o"
	rm -f "$(CONFIG)/obj/chunkFilter.o"
	rm -f "$(CONFIG)/obj/client.o"
	rm -f "$(CONFIG)/obj/conn.o"
	rm -f "$(CONFIG)/obj/digest.o"
	rm -f "$(CONFIG)/obj/endpoint.o"
	rm -f "$(CONFIG)/obj/error.o"
	rm -f "$(CONFIG)/obj/host.o"
	rm -f "$(CONFIG)/obj/http.o"
	rm -f "$(CONFIG)/obj/httpService.o"
	rm -f "$(CONFIG)/obj/log.o"
	rm -f "$(CONFIG)/obj/netConnector.o"
	rm -f "$(CONFIG)/obj/packet.o"
	rm -f "$(CONFIG)/obj/pam.o"
	rm -f "$(CONFIG)/obj/passHandler.o"
	rm -f "$(CONFIG)/obj/pipeline.o"
	rm -f "$(CONFIG)/obj/queue.o"
	rm -f "$(CONFIG)/obj/rangeFilter.o"
	rm -f "$(CONFIG)/obj/route.o"
	rm -f "$(CONFIG)/obj/rx.o"
	rm -f "$(CONFIG)/obj/sendConnector.o"
	rm -f "$(CONFIG)/obj/session.o"
	rm -f "$(CONFIG)/obj/stage.o"
	rm -f "$(CONFIG)/obj/trace.o"
	rm -f "$(CONFIG)/obj/tx.o"
	rm -f "$(CONFIG)/obj/uploadFilter.o"
	rm -f "$(CONFIG)/obj/uri.o"
	rm -f "$(CONFIG)/obj/var.o"
	rm -f "$(CONFIG)/obj/webSock.o"

clobber: clean
	rm -fr ./$(CONFIG)



#
#   version
#
version: $(DEPS_1)
	@echo NN 1.3.0-0

#
#   est.h
#
$(CONFIG)/inc/est.h: $(DEPS_2)
	@echo '      [Copy] $(CONFIG)/inc/est.h'
	mkdir -p "$(CONFIG)/inc"
	cp "src/deps/est/est.h" "$(CONFIG)/inc/est.h"

#
#   bit.h
#
$(CONFIG)/inc/bit.h: $(DEPS_3)
	@echo '      [Copy] $(CONFIG)/inc/bit.h'

#
#   bitos.h
#
DEPS_4 += $(CONFIG)/inc/bit.h

src/bitos.h: $(DEPS_4)
	@echo '      [Copy] src/bitos.h'

#
#   estLib.o
#
DEPS_5 += $(CONFIG)/inc/bit.h
DEPS_5 += $(CONFIG)/inc/est.h
DEPS_5 += src/bitos.h

$(CONFIG)/obj/estLib.o: \
    src/deps/est/estLib.c $(DEPS_5)
	@echo '   [Compile] src/deps/est/estLib.c'
	$(CC) -c -o $(CONFIG)/obj/estLib.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/deps/est/estLib.c

ifeq ($(BIT_PACK_EST),1)
#
#   libest
#
DEPS_6 += $(CONFIG)/inc/est.h
DEPS_6 += $(CONFIG)/obj/estLib.o

$(CONFIG)/bin/libest.so: $(DEPS_6)
	@echo '      [Link] libest'
	$(CC) -shared -o $(CONFIG)/bin/libest.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/estLib.o $(LIBS)
endif

#
#   ca-crt
#
DEPS_7 += src/deps/est/ca.crt

$(CONFIG)/bin/ca.crt: $(DEPS_7)
	@echo '      [Copy] $(CONFIG)/bin/ca.crt'
	mkdir -p "$(CONFIG)/bin"
	cp "src/deps/est/ca.crt" "$(CONFIG)/bin/ca.crt"

#
#   mpr.h
#
$(CONFIG)/inc/mpr.h: $(DEPS_8)
	@echo '      [Copy] $(CONFIG)/inc/mpr.h'
	mkdir -p "$(CONFIG)/inc"
	cp "src/deps/mpr/mpr.h" "$(CONFIG)/inc/mpr.h"

#
#   mprLib.o
#
DEPS_9 += $(CONFIG)/inc/bit.h
DEPS_9 += $(CONFIG)/inc/mpr.h
DEPS_9 += src/bitos.h

$(CONFIG)/obj/mprLib.o: \
    src/deps/mpr/mprLib.c $(DEPS_9)
	@echo '   [Compile] src/deps/mpr/mprLib.c'
	$(CC) -c -o $(CONFIG)/obj/mprLib.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/mprLib.c

#
#   libmpr
#
DEPS_10 += $(CONFIG)/inc/mpr.h
DEPS_10 += $(CONFIG)/obj/mprLib.o

$(CONFIG)/bin/libmpr.so: $(DEPS_10)
	@echo '      [Link] libmpr'
	$(CC) -shared -o $(CONFIG)/bin/libmpr.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/mprLib.o $(LIBS)

#
#   mprSsl.o
#
DEPS_11 += $(CONFIG)/inc/bit.h
DEPS_11 += $(CONFIG)/inc/mpr.h
DEPS_11 += $(CONFIG)/inc/est.h

$(CONFIG)/obj/mprSsl.o: \
    src/deps/mpr/mprSsl.c $(DEPS_11)
	@echo '   [Compile] src/deps/mpr/mprSsl.c'
	$(CC) -c -o $(CONFIG)/obj/mprSsl.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/mprSsl.c

ifeq ($(BIT_PACK_SSL),1)
#
#   libmprssl
#
DEPS_12 += $(CONFIG)/bin/libmpr.so
ifeq ($(BIT_PACK_EST),1)
    DEPS_12 += $(CONFIG)/bin/libest.so
endif
DEPS_12 += $(CONFIG)/obj/mprSsl.o

ifeq ($(BIT_PACK_EST),1)
    LIBS_12 += -lest
endif
LIBS_12 += -lmpr

$(CONFIG)/bin/libmprssl.so: $(DEPS_12)
	@echo '      [Link] libmprssl'
	$(CC) -shared -o $(CONFIG)/bin/libmprssl.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/mprSsl.o $(LIBS_12) $(LIBS_12) $(LIBS)
endif

#
#   makerom.o
#
DEPS_13 += $(CONFIG)/inc/bit.h
DEPS_13 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/makerom.o: \
    src/deps/mpr/makerom.c $(DEPS_13)
	@echo '   [Compile] src/deps/mpr/makerom.c'
	$(CC) -c -o $(CONFIG)/obj/makerom.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/makerom.c

#
#   makerom
#
DEPS_14 += $(CONFIG)/bin/libmpr.so
DEPS_14 += $(CONFIG)/obj/makerom.o

LIBS_14 += -lmpr

$(CONFIG)/bin/makerom: $(DEPS_14)
	@echo '      [Link] makerom'
	$(CC) -o $(CONFIG)/bin/makerom $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/makerom.o $(LIBS_14) $(LIBS_14) $(LIBS) $(LDFLAGS)

#
#   bitos.h
#
$(CONFIG)/inc/bitos.h: $(DEPS_15)
	@echo '      [Copy] $(CONFIG)/inc/bitos.h'
	mkdir -p "$(CONFIG)/inc"
	cp "src/bitos.h" "$(CONFIG)/inc/bitos.h"

#
#   http.h
#
$(CONFIG)/inc/http.h: $(DEPS_16)
	@echo '      [Copy] $(CONFIG)/inc/http.h'
	mkdir -p "$(CONFIG)/inc"
	cp "src/http.h" "$(CONFIG)/inc/http.h"

#
#   http.h
#
src/http.h: $(DEPS_17)
	@echo '      [Copy] src/http.h'

#
#   actionHandler.o
#
DEPS_18 += $(CONFIG)/inc/bit.h
DEPS_18 += src/http.h
DEPS_18 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/actionHandler.o: \
    src/actionHandler.c $(DEPS_18)
	@echo '   [Compile] src/actionHandler.c'
	$(CC) -c -o $(CONFIG)/obj/actionHandler.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/actionHandler.c

#
#   auth.o
#
DEPS_19 += $(CONFIG)/inc/bit.h
DEPS_19 += src/http.h

$(CONFIG)/obj/auth.o: \
    src/auth.c $(DEPS_19)
	@echo '   [Compile] src/auth.c'
	$(CC) -c -o $(CONFIG)/obj/auth.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/auth.c

#
#   basic.o
#
DEPS_20 += $(CONFIG)/inc/bit.h
DEPS_20 += src/http.h

$(CONFIG)/obj/basic.o: \
    src/basic.c $(DEPS_20)
	@echo '   [Compile] src/basic.c'
	$(CC) -c -o $(CONFIG)/obj/basic.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/basic.c

#
#   cache.o
#
DEPS_21 += $(CONFIG)/inc/bit.h
DEPS_21 += src/http.h

$(CONFIG)/obj/cache.o: \
    src/cache.c $(DEPS_21)
	@echo '   [Compile] src/cache.c'
	$(CC) -c -o $(CONFIG)/obj/cache.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/cache.c

#
#   chunkFilter.o
#
DEPS_22 += $(CONFIG)/inc/bit.h
DEPS_22 += src/http.h

$(CONFIG)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_22)
	@echo '   [Compile] src/chunkFilter.c'
	$(CC) -c -o $(CONFIG)/obj/chunkFilter.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/chunkFilter.c

#
#   client.o
#
DEPS_23 += $(CONFIG)/inc/bit.h
DEPS_23 += src/http.h

$(CONFIG)/obj/client.o: \
    src/client.c $(DEPS_23)
	@echo '   [Compile] src/client.c'
	$(CC) -c -o $(CONFIG)/obj/client.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/client.c

#
#   conn.o
#
DEPS_24 += $(CONFIG)/inc/bit.h
DEPS_24 += src/http.h

$(CONFIG)/obj/conn.o: \
    src/conn.c $(DEPS_24)
	@echo '   [Compile] src/conn.c'
	$(CC) -c -o $(CONFIG)/obj/conn.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/conn.c

#
#   digest.o
#
DEPS_25 += $(CONFIG)/inc/bit.h
DEPS_25 += src/http.h

$(CONFIG)/obj/digest.o: \
    src/digest.c $(DEPS_25)
	@echo '   [Compile] src/digest.c'
	$(CC) -c -o $(CONFIG)/obj/digest.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/digest.c

#
#   endpoint.o
#
DEPS_26 += $(CONFIG)/inc/bit.h
DEPS_26 += src/http.h

$(CONFIG)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_26)
	@echo '   [Compile] src/endpoint.c'
	$(CC) -c -o $(CONFIG)/obj/endpoint.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/endpoint.c

#
#   error.o
#
DEPS_27 += $(CONFIG)/inc/bit.h
DEPS_27 += src/http.h

$(CONFIG)/obj/error.o: \
    src/error.c $(DEPS_27)
	@echo '   [Compile] src/error.c'
	$(CC) -c -o $(CONFIG)/obj/error.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/error.c

#
#   host.o
#
DEPS_28 += $(CONFIG)/inc/bit.h
DEPS_28 += src/http.h

$(CONFIG)/obj/host.o: \
    src/host.c $(DEPS_28)
	@echo '   [Compile] src/host.c'
	$(CC) -c -o $(CONFIG)/obj/host.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/host.c

#
#   http.o
#
DEPS_29 += $(CONFIG)/inc/bit.h
DEPS_29 += src/http.h

$(CONFIG)/obj/http.o: \
    src/http.c $(DEPS_29)
	@echo '   [Compile] src/http.c'
	$(CC) -c -o $(CONFIG)/obj/http.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/http.c

#
#   httpService.o
#
DEPS_30 += $(CONFIG)/inc/bit.h
DEPS_30 += src/http.h

$(CONFIG)/obj/httpService.o: \
    src/httpService.c $(DEPS_30)
	@echo '   [Compile] src/httpService.c'
	$(CC) -c -o $(CONFIG)/obj/httpService.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/httpService.c

#
#   log.o
#
DEPS_31 += $(CONFIG)/inc/bit.h
DEPS_31 += src/http.h

$(CONFIG)/obj/log.o: \
    src/log.c $(DEPS_31)
	@echo '   [Compile] src/log.c'
	$(CC) -c -o $(CONFIG)/obj/log.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/log.c

#
#   netConnector.o
#
DEPS_32 += $(CONFIG)/inc/bit.h
DEPS_32 += src/http.h

$(CONFIG)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_32)
	@echo '   [Compile] src/netConnector.c'
	$(CC) -c -o $(CONFIG)/obj/netConnector.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/netConnector.c

#
#   packet.o
#
DEPS_33 += $(CONFIG)/inc/bit.h
DEPS_33 += src/http.h

$(CONFIG)/obj/packet.o: \
    src/packet.c $(DEPS_33)
	@echo '   [Compile] src/packet.c'
	$(CC) -c -o $(CONFIG)/obj/packet.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/packet.c

#
#   pam.o
#
DEPS_34 += $(CONFIG)/inc/bit.h
DEPS_34 += src/http.h

$(CONFIG)/obj/pam.o: \
    src/pam.c $(DEPS_34)
	@echo '   [Compile] src/pam.c'
	$(CC) -c -o $(CONFIG)/obj/pam.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/pam.c

#
#   passHandler.o
#
DEPS_35 += $(CONFIG)/inc/bit.h
DEPS_35 += src/http.h

$(CONFIG)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_35)
	@echo '   [Compile] src/passHandler.c'
	$(CC) -c -o $(CONFIG)/obj/passHandler.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/passHandler.c

#
#   pipeline.o
#
DEPS_36 += $(CONFIG)/inc/bit.h
DEPS_36 += src/http.h

$(CONFIG)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_36)
	@echo '   [Compile] src/pipeline.c'
	$(CC) -c -o $(CONFIG)/obj/pipeline.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/pipeline.c

#
#   queue.o
#
DEPS_37 += $(CONFIG)/inc/bit.h
DEPS_37 += src/http.h

$(CONFIG)/obj/queue.o: \
    src/queue.c $(DEPS_37)
	@echo '   [Compile] src/queue.c'
	$(CC) -c -o $(CONFIG)/obj/queue.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/queue.c

#
#   rangeFilter.o
#
DEPS_38 += $(CONFIG)/inc/bit.h
DEPS_38 += src/http.h

$(CONFIG)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_38)
	@echo '   [Compile] src/rangeFilter.c'
	$(CC) -c -o $(CONFIG)/obj/rangeFilter.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/rangeFilter.c

#
#   route.o
#
DEPS_39 += $(CONFIG)/inc/bit.h
DEPS_39 += src/http.h

$(CONFIG)/obj/route.o: \
    src/route.c $(DEPS_39)
	@echo '   [Compile] src/route.c'
	$(CC) -c -o $(CONFIG)/obj/route.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/route.c

#
#   rx.o
#
DEPS_40 += $(CONFIG)/inc/bit.h
DEPS_40 += src/http.h

$(CONFIG)/obj/rx.o: \
    src/rx.c $(DEPS_40)
	@echo '   [Compile] src/rx.c'
	$(CC) -c -o $(CONFIG)/obj/rx.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/rx.c

#
#   sendConnector.o
#
DEPS_41 += $(CONFIG)/inc/bit.h
DEPS_41 += src/http.h

$(CONFIG)/obj/sendConnector.o: \
    src/sendConnector.c $(DEPS_41)
	@echo '   [Compile] src/sendConnector.c'
	$(CC) -c -o $(CONFIG)/obj/sendConnector.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/sendConnector.c

#
#   session.o
#
DEPS_42 += $(CONFIG)/inc/bit.h
DEPS_42 += src/http.h

$(CONFIG)/obj/session.o: \
    src/session.c $(DEPS_42)
	@echo '   [Compile] src/session.c'
	$(CC) -c -o $(CONFIG)/obj/session.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/session.c

#
#   stage.o
#
DEPS_43 += $(CONFIG)/inc/bit.h
DEPS_43 += src/http.h

$(CONFIG)/obj/stage.o: \
    src/stage.c $(DEPS_43)
	@echo '   [Compile] src/stage.c'
	$(CC) -c -o $(CONFIG)/obj/stage.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/stage.c

#
#   trace.o
#
DEPS_44 += $(CONFIG)/inc/bit.h
DEPS_44 += src/http.h

$(CONFIG)/obj/trace.o: \
    src/trace.c $(DEPS_44)
	@echo '   [Compile] src/trace.c'
	$(CC) -c -o $(CONFIG)/obj/trace.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/trace.c

#
#   tx.o
#
DEPS_45 += $(CONFIG)/inc/bit.h
DEPS_45 += src/http.h

$(CONFIG)/obj/tx.o: \
    src/tx.c $(DEPS_45)
	@echo '   [Compile] src/tx.c'
	$(CC) -c -o $(CONFIG)/obj/tx.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/tx.c

#
#   uploadFilter.o
#
DEPS_46 += $(CONFIG)/inc/bit.h
DEPS_46 += src/http.h

$(CONFIG)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_46)
	@echo '   [Compile] src/uploadFilter.c'
	$(CC) -c -o $(CONFIG)/obj/uploadFilter.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/uploadFilter.c

#
#   uri.o
#
DEPS_47 += $(CONFIG)/inc/bit.h
DEPS_47 += src/http.h

$(CONFIG)/obj/uri.o: \
    src/uri.c $(DEPS_47)
	@echo '   [Compile] src/uri.c'
	$(CC) -c -o $(CONFIG)/obj/uri.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/uri.c

#
#   var.o
#
DEPS_48 += $(CONFIG)/inc/bit.h
DEPS_48 += src/http.h

$(CONFIG)/obj/var.o: \
    src/var.c $(DEPS_48)
	@echo '   [Compile] src/var.c'
	$(CC) -c -o $(CONFIG)/obj/var.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/var.c

#
#   webSock.o
#
DEPS_49 += $(CONFIG)/inc/bit.h
DEPS_49 += src/http.h

$(CONFIG)/obj/webSock.o: \
    src/webSock.c $(DEPS_49)
	@echo '   [Compile] src/webSock.c'
	$(CC) -c -o $(CONFIG)/obj/webSock.o -fPIC $(LDFLAGS) $(DFLAGS) $(IFLAGS) src/webSock.c

#
#   libhttp
#
DEPS_50 += $(CONFIG)/bin/libmpr.so
DEPS_50 += $(CONFIG)/inc/bitos.h
DEPS_50 += $(CONFIG)/inc/http.h
DEPS_50 += $(CONFIG)/obj/actionHandler.o
DEPS_50 += $(CONFIG)/obj/auth.o
DEPS_50 += $(CONFIG)/obj/basic.o
DEPS_50 += $(CONFIG)/obj/cache.o
DEPS_50 += $(CONFIG)/obj/chunkFilter.o
DEPS_50 += $(CONFIG)/obj/client.o
DEPS_50 += $(CONFIG)/obj/conn.o
DEPS_50 += $(CONFIG)/obj/digest.o
DEPS_50 += $(CONFIG)/obj/endpoint.o
DEPS_50 += $(CONFIG)/obj/error.o
DEPS_50 += $(CONFIG)/obj/host.o
DEPS_50 += $(CONFIG)/obj/http.o
DEPS_50 += $(CONFIG)/obj/httpService.o
DEPS_50 += $(CONFIG)/obj/log.o
DEPS_50 += $(CONFIG)/obj/netConnector.o
DEPS_50 += $(CONFIG)/obj/packet.o
DEPS_50 += $(CONFIG)/obj/pam.o
DEPS_50 += $(CONFIG)/obj/passHandler.o
DEPS_50 += $(CONFIG)/obj/pipeline.o
DEPS_50 += $(CONFIG)/obj/queue.o
DEPS_50 += $(CONFIG)/obj/rangeFilter.o
DEPS_50 += $(CONFIG)/obj/route.o
DEPS_50 += $(CONFIG)/obj/rx.o
DEPS_50 += $(CONFIG)/obj/sendConnector.o
DEPS_50 += $(CONFIG)/obj/session.o
DEPS_50 += $(CONFIG)/obj/stage.o
DEPS_50 += $(CONFIG)/obj/trace.o
DEPS_50 += $(CONFIG)/obj/tx.o
DEPS_50 += $(CONFIG)/obj/uploadFilter.o
DEPS_50 += $(CONFIG)/obj/uri.o
DEPS_50 += $(CONFIG)/obj/var.o
DEPS_50 += $(CONFIG)/obj/webSock.o

LIBS_50 += -lmpr

$(CONFIG)/bin/libhttp.so: $(DEPS_50)
	@echo '      [Link] libhttp'
	$(CC) -shared -o $(CONFIG)/bin/libhttp.so $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/actionHandler.o $(CONFIG)/obj/auth.o $(CONFIG)/obj/basic.o $(CONFIG)/obj/cache.o $(CONFIG)/obj/chunkFilter.o $(CONFIG)/obj/client.o $(CONFIG)/obj/conn.o $(CONFIG)/obj/digest.o $(CONFIG)/obj/endpoint.o $(CONFIG)/obj/error.o $(CONFIG)/obj/host.o $(CONFIG)/obj/http.o $(CONFIG)/obj/httpService.o $(CONFIG)/obj/log.o $(CONFIG)/obj/netConnector.o $(CONFIG)/obj/packet.o $(CONFIG)/obj/pam.o $(CONFIG)/obj/passHandler.o $(CONFIG)/obj/pipeline.o $(CONFIG)/obj/queue.o $(CONFIG)/obj/rangeFilter.o $(CONFIG)/obj/route.o $(CONFIG)/obj/rx.o $(CONFIG)/obj/sendConnector.o $(CONFIG)/obj/session.o $(CONFIG)/obj/stage.o $(CONFIG)/obj/trace.o $(CONFIG)/obj/tx.o $(CONFIG)/obj/uploadFilter.o $(CONFIG)/obj/uri.o $(CONFIG)/obj/var.o $(CONFIG)/obj/webSock.o $(LIBS_50) $(LIBS_50) $(LIBS)

#
#   http
#
DEPS_51 += $(CONFIG)/bin/libhttp.so
DEPS_51 += $(CONFIG)/obj/http.o

LIBS_51 += -lhttp
LIBS_51 += -lmpr

$(CONFIG)/bin/http: $(DEPS_51)
	@echo '      [Link] http'
	$(CC) -o $(CONFIG)/bin/http $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/http.o $(LIBS_51) $(LIBS_51) $(LIBS) -lmpr $(LDFLAGS)

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
	

