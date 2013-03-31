#
#   http-linux-static.mk -- Makefile to build Http Library for linux
#

PRODUCT            := http
VERSION            := 1.3.0
BUILD_NUMBER       := 0
PROFILE            := static
ARCH               := $(shell uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/')
OS                 := linux
CC                 := gcc
LD                 := link
CONFIG             := $(OS)-$(ARCH)-$(PROFILE)
LBIN               := $(CONFIG)/bin

BIT_PACK_EST       := 1
BIT_PACK_MATRIXSSL := 0
BIT_PACK_OPENSSL   := 0
BIT_PACK_PCRE      := 1
BIT_PACK_SSL       := 1

ifeq ($(BIT_PACK_EST),1)
    BIT_PACK_SSL := 1
endif
ifeq ($(BIT_PACK_LIB),1)
    BIT_PACK_COMPILER := 1
endif
ifeq ($(BIT_PACK_MATRIXSSL),1)
    BIT_PACK_SSL := 1
endif
ifeq ($(BIT_PACK_NANOSSL),1)
    BIT_PACK_SSL := 1
endif
ifeq ($(BIT_PACK_OPENSSL),1)
    BIT_PACK_SSL := 1
endif

BIT_PACK_COMPILER_PATH    := gcc
BIT_PACK_DOXYGEN_PATH     := doxygen
BIT_PACK_DSI_PATH         := dsi
BIT_PACK_EST_PATH         := est
BIT_PACK_LIB_PATH         := ar
BIT_PACK_LINK_PATH        := link
BIT_PACK_MAN_PATH         := man
BIT_PACK_MAN2HTML_PATH    := man2html
BIT_PACK_MATRIXSSL_PATH   := /usr/src/matrixssl
BIT_PACK_NANOSSL_PATH     := /usr/src/nanossl
BIT_PACK_OPENSSL_PATH     := /usr/src/openssl
BIT_PACK_PCRE_PATH        := pcre
BIT_PACK_SSL_PATH         := ssl
BIT_PACK_UTEST_PATH       := utest

CFLAGS             += -fPIC   -w
DFLAGS             += -D_REENTRANT -DPIC  $(patsubst %,-D%,$(filter BIT_%,$(MAKEFLAGS))) -DBIT_PACK_EST=$(BIT_PACK_EST) -DBIT_PACK_MATRIXSSL=$(BIT_PACK_MATRIXSSL) -DBIT_PACK_OPENSSL=$(BIT_PACK_OPENSSL) -DBIT_PACK_PCRE=$(BIT_PACK_PCRE) -DBIT_PACK_SSL=$(BIT_PACK_SSL) 
IFLAGS             += -I$(CONFIG)/inc -Isrc
LDFLAGS            += '-Wl,--enable-new-dtags' '-Wl,-rpath,$$ORIGIN/' '-rdynamic'
LIBPATHS           += -L$(CONFIG)/bin
LIBS               += -lpthread -lm -lrt -ldl

DEBUG              := debug
CFLAGS-debug       := -g
DFLAGS-debug       := -DBIT_DEBUG
LDFLAGS-debug      := -g
DFLAGS-release     := 
CFLAGS-release     := -O2
LDFLAGS-release    := 
CFLAGS             += $(CFLAGS-$(DEBUG))
DFLAGS             += $(DFLAGS-$(DEBUG))
LDFLAGS            += $(LDFLAGS-$(DEBUG))

BIT_ROOT_PREFIX    := 
BIT_BASE_PREFIX    := $(BIT_ROOT_PREFIX)/usr/local
BIT_DATA_PREFIX    := $(BIT_ROOT_PREFIX)/
BIT_STATE_PREFIX   := $(BIT_ROOT_PREFIX)/var
BIT_APP_PREFIX     := $(BIT_BASE_PREFIX)/lib/$(PRODUCT)
BIT_VAPP_PREFIX    := $(BIT_APP_PREFIX)/$(VERSION)
BIT_BIN_PREFIX     := $(BIT_ROOT_PREFIX)/usr/local/bin
BIT_INC_PREFIX     := $(BIT_ROOT_PREFIX)/usr/local/include
BIT_LIB_PREFIX     := $(BIT_ROOT_PREFIX)/usr/local/lib
BIT_MAN_PREFIX     := $(BIT_ROOT_PREFIX)/usr/local/share/man
BIT_SBIN_PREFIX    := $(BIT_ROOT_PREFIX)/usr/local/sbin
BIT_ETC_PREFIX     := $(BIT_ROOT_PREFIX)/etc/$(PRODUCT)
BIT_WEB_PREFIX     := $(BIT_ROOT_PREFIX)/var/www/$(PRODUCT)-default
BIT_LOG_PREFIX     := $(BIT_ROOT_PREFIX)/var/log/$(PRODUCT)
BIT_SPOOL_PREFIX   := $(BIT_ROOT_PREFIX)/var/spool/$(PRODUCT)
BIT_CACHE_PREFIX   := $(BIT_ROOT_PREFIX)/var/spool/$(PRODUCT)/cache
BIT_SRC_PREFIX     := $(BIT_ROOT_PREFIX)$(PRODUCT)-$(VERSION)


ifeq ($(BIT_PACK_EST),1)
TARGETS            += $(CONFIG)/bin/libest.a
endif
TARGETS            += $(CONFIG)/bin/ca.crt
ifeq ($(BIT_PACK_PCRE),1)
TARGETS            += $(CONFIG)/bin/libpcre.a
endif
TARGETS            += $(CONFIG)/bin/libmpr.a
TARGETS            += $(CONFIG)/bin/libmprssl.a
TARGETS            += $(CONFIG)/bin/makerom
ifeq ($(BIT_PACK_PCRE),1)
TARGETS            += $(CONFIG)/bin/libhttp.a
endif
TARGETS            += $(CONFIG)/bin/http

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
	@[ ! -f $(CONFIG)/inc/bit.h ] && cp projects/http-linux-static-bit.h $(CONFIG)/inc/bit.h ; true
	@[ ! -f $(CONFIG)/inc/bitos.h ] && cp src/bitos.h $(CONFIG)/inc/bitos.h ; true
	@if ! diff $(CONFIG)/inc/bitos.h src/bitos.h >/dev/null ; then\
		cp src/bitos.h $(CONFIG)/inc/bitos.h  ; \
	fi; true
	@if ! diff $(CONFIG)/inc/bit.h projects/http-linux-static-bit.h >/dev/null ; then\
		cp projects/http-linux-static-bit.h $(CONFIG)/inc/bit.h  ; \
	fi; true
	@if [ -f "$(CONFIG)/.makeflags" ] ; then \
		if [ "$(MAKEFLAGS)" != " ` cat $(CONFIG)/.makeflags`" ] ; then \
			echo "   [Warning] Make flags have changed since the last build: "`cat $(CONFIG)/.makeflags`"" ; \
		fi ; \
	fi
	@echo $(MAKEFLAGS) >$(CONFIG)/.makeflags
clean:
	rm -f "$(CONFIG)/bin/libest.a"
	rm -f "$(CONFIG)/bin/ca.crt"
	rm -f "$(CONFIG)/bin/libpcre.a"
	rm -f "$(CONFIG)/bin/libmpr.a"
	rm -f "$(CONFIG)/bin/libmprssl.a"
	rm -f "$(CONFIG)/bin/makerom"
	rm -f "$(CONFIG)/bin/libhttp.a"
	rm -f "$(CONFIG)/bin/http"
	rm -f "$(CONFIG)/obj/estLib.o"
	rm -f "$(CONFIG)/obj/pcre.o"
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
	rm -f "$(CONFIG)/obj/http.o"

clobber: clean
	rm -fr ./$(CONFIG)



#
#   version
#
version: $(DEPS_1)
	@echo 1.3.0-0

#
#   est.h
#
$(CONFIG)/inc/est.h: $(DEPS_2)
	@echo '      [Copy] $(CONFIG)/inc/est.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/deps/est/est.h $(CONFIG)/inc/est.h

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
	@echo '   [Compile] $(CONFIG)/obj/estLib.o'
	$(CC) -c -o $(CONFIG)/obj/estLib.o -fPIC $(DFLAGS) $(IFLAGS) src/deps/est/estLib.c

ifeq ($(BIT_PACK_EST),1)
#
#   libest
#
DEPS_6 += $(CONFIG)/inc/est.h
DEPS_6 += $(CONFIG)/obj/estLib.o

$(CONFIG)/bin/libest.a: $(DEPS_6)
	@echo '      [Link] $(CONFIG)/bin/libest.a'
	ar -cr $(CONFIG)/bin/libest.a $(CONFIG)/obj/estLib.o
endif

#
#   ca-crt
#
DEPS_7 += src/deps/est/ca.crt

$(CONFIG)/bin/ca.crt: $(DEPS_7)
	@echo '      [Copy] $(CONFIG)/bin/ca.crt'
	mkdir -p "$(CONFIG)/bin"
	cp src/deps/est/ca.crt $(CONFIG)/bin/ca.crt

#
#   pcre.h
#
$(CONFIG)/inc/pcre.h: $(DEPS_8)
	@echo '      [Copy] $(CONFIG)/inc/pcre.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/deps/pcre/pcre.h $(CONFIG)/inc/pcre.h

#
#   pcre.o
#
DEPS_9 += $(CONFIG)/inc/bit.h
DEPS_9 += $(CONFIG)/inc/pcre.h

$(CONFIG)/obj/pcre.o: \
    src/deps/pcre/pcre.c $(DEPS_9)
	@echo '   [Compile] $(CONFIG)/obj/pcre.o'
	$(CC) -c -o $(CONFIG)/obj/pcre.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/deps/pcre/pcre.c

ifeq ($(BIT_PACK_PCRE),1)
#
#   libpcre
#
DEPS_10 += $(CONFIG)/inc/pcre.h
DEPS_10 += $(CONFIG)/obj/pcre.o

$(CONFIG)/bin/libpcre.a: $(DEPS_10)
	@echo '      [Link] $(CONFIG)/bin/libpcre.a'
	ar -cr $(CONFIG)/bin/libpcre.a $(CONFIG)/obj/pcre.o
endif

#
#   mpr.h
#
$(CONFIG)/inc/mpr.h: $(DEPS_11)
	@echo '      [Copy] $(CONFIG)/inc/mpr.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/deps/mpr/mpr.h $(CONFIG)/inc/mpr.h

#
#   mprLib.o
#
DEPS_12 += $(CONFIG)/inc/bit.h
DEPS_12 += $(CONFIG)/inc/mpr.h
DEPS_12 += src/bitos.h

$(CONFIG)/obj/mprLib.o: \
    src/deps/mpr/mprLib.c $(DEPS_12)
	@echo '   [Compile] $(CONFIG)/obj/mprLib.o'
	$(CC) -c -o $(CONFIG)/obj/mprLib.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/mprLib.c

#
#   libmpr
#
DEPS_13 += $(CONFIG)/inc/mpr.h
DEPS_13 += $(CONFIG)/obj/mprLib.o

$(CONFIG)/bin/libmpr.a: $(DEPS_13)
	@echo '      [Link] $(CONFIG)/bin/libmpr.a'
	ar -cr $(CONFIG)/bin/libmpr.a $(CONFIG)/obj/mprLib.o

ifeq ($(BIT_PACK_SSL),1)
#
#   est
#
ifeq ($(BIT_PACK_EST),1)
    DEPS_14 += $(CONFIG)/bin/libest.a
endif

est: $(DEPS_14)
endif

#
#   ssl
#
ifeq ($(BIT_PACK_SSL),1)
    DEPS_15 += est
endif

ssl: $(DEPS_15)

#
#   mprSsl.o
#
DEPS_16 += $(CONFIG)/inc/bit.h
DEPS_16 += $(CONFIG)/inc/mpr.h
DEPS_16 += $(CONFIG)/inc/est.h

$(CONFIG)/obj/mprSsl.o: \
    src/deps/mpr/mprSsl.c $(DEPS_16)
	@echo '   [Compile] $(CONFIG)/obj/mprSsl.o'
	$(CC) -c -o $(CONFIG)/obj/mprSsl.o $(CFLAGS) $(DFLAGS) $(IFLAGS) -I$(BIT_PACK_MATRIXSSL_PATH) -I$(BIT_PACK_MATRIXSSL_PATH)/matrixssl -I$(BIT_PACK_NANOSSL_PATH)/src -I$(BIT_PACK_OPENSSL_PATH)/include src/deps/mpr/mprSsl.c

#
#   libmprssl
#
DEPS_17 += $(CONFIG)/bin/libmpr.a
DEPS_17 += ssl
DEPS_17 += $(CONFIG)/obj/mprSsl.o

$(CONFIG)/bin/libmprssl.a: $(DEPS_17)
	@echo '      [Link] $(CONFIG)/bin/libmprssl.a'
	ar -cr $(CONFIG)/bin/libmprssl.a $(CONFIG)/obj/mprSsl.o

#
#   makerom.o
#
DEPS_18 += $(CONFIG)/inc/bit.h
DEPS_18 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/makerom.o: \
    src/deps/mpr/makerom.c $(DEPS_18)
	@echo '   [Compile] $(CONFIG)/obj/makerom.o'
	$(CC) -c -o $(CONFIG)/obj/makerom.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/deps/mpr/makerom.c

#
#   makerom
#
DEPS_19 += $(CONFIG)/bin/libmpr.a
DEPS_19 += $(CONFIG)/obj/makerom.o

LIBS_19 += -lmpr

$(CONFIG)/bin/makerom: $(DEPS_19)
	@echo '      [Link] $(CONFIG)/bin/makerom'
	$(CC) -o $(CONFIG)/bin/makerom $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/makerom.o $(LIBPATHS_19) $(LIBS_19) $(LIBS_19) $(LIBS) -lpthread -lm -lrt -ldl $(LDFLAGS) 

#
#   bitos.h
#
$(CONFIG)/inc/bitos.h: $(DEPS_20)
	@echo '      [Copy] $(CONFIG)/inc/bitos.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/bitos.h $(CONFIG)/inc/bitos.h

#
#   http.h
#
$(CONFIG)/inc/http.h: $(DEPS_21)
	@echo '      [Copy] $(CONFIG)/inc/http.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/http.h $(CONFIG)/inc/http.h

#
#   http.h
#
src/http.h: $(DEPS_22)
	@echo '      [Copy] src/http.h'

#
#   actionHandler.o
#
DEPS_23 += $(CONFIG)/inc/bit.h
DEPS_23 += src/http.h
DEPS_23 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/actionHandler.o: \
    src/actionHandler.c $(DEPS_23)
	@echo '   [Compile] $(CONFIG)/obj/actionHandler.o'
	$(CC) -c -o $(CONFIG)/obj/actionHandler.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/actionHandler.c

#
#   auth.o
#
DEPS_24 += $(CONFIG)/inc/bit.h
DEPS_24 += src/http.h

$(CONFIG)/obj/auth.o: \
    src/auth.c $(DEPS_24)
	@echo '   [Compile] $(CONFIG)/obj/auth.o'
	$(CC) -c -o $(CONFIG)/obj/auth.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/auth.c

#
#   basic.o
#
DEPS_25 += $(CONFIG)/inc/bit.h
DEPS_25 += src/http.h

$(CONFIG)/obj/basic.o: \
    src/basic.c $(DEPS_25)
	@echo '   [Compile] $(CONFIG)/obj/basic.o'
	$(CC) -c -o $(CONFIG)/obj/basic.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/basic.c

#
#   cache.o
#
DEPS_26 += $(CONFIG)/inc/bit.h
DEPS_26 += src/http.h

$(CONFIG)/obj/cache.o: \
    src/cache.c $(DEPS_26)
	@echo '   [Compile] $(CONFIG)/obj/cache.o'
	$(CC) -c -o $(CONFIG)/obj/cache.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/cache.c

#
#   chunkFilter.o
#
DEPS_27 += $(CONFIG)/inc/bit.h
DEPS_27 += src/http.h

$(CONFIG)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_27)
	@echo '   [Compile] $(CONFIG)/obj/chunkFilter.o'
	$(CC) -c -o $(CONFIG)/obj/chunkFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/chunkFilter.c

#
#   client.o
#
DEPS_28 += $(CONFIG)/inc/bit.h
DEPS_28 += src/http.h

$(CONFIG)/obj/client.o: \
    src/client.c $(DEPS_28)
	@echo '   [Compile] $(CONFIG)/obj/client.o'
	$(CC) -c -o $(CONFIG)/obj/client.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/client.c

#
#   conn.o
#
DEPS_29 += $(CONFIG)/inc/bit.h
DEPS_29 += src/http.h

$(CONFIG)/obj/conn.o: \
    src/conn.c $(DEPS_29)
	@echo '   [Compile] $(CONFIG)/obj/conn.o'
	$(CC) -c -o $(CONFIG)/obj/conn.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/conn.c

#
#   digest.o
#
DEPS_30 += $(CONFIG)/inc/bit.h
DEPS_30 += src/http.h

$(CONFIG)/obj/digest.o: \
    src/digest.c $(DEPS_30)
	@echo '   [Compile] $(CONFIG)/obj/digest.o'
	$(CC) -c -o $(CONFIG)/obj/digest.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/digest.c

#
#   endpoint.o
#
DEPS_31 += $(CONFIG)/inc/bit.h
DEPS_31 += src/http.h

$(CONFIG)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_31)
	@echo '   [Compile] $(CONFIG)/obj/endpoint.o'
	$(CC) -c -o $(CONFIG)/obj/endpoint.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/endpoint.c

#
#   error.o
#
DEPS_32 += $(CONFIG)/inc/bit.h
DEPS_32 += src/http.h

$(CONFIG)/obj/error.o: \
    src/error.c $(DEPS_32)
	@echo '   [Compile] $(CONFIG)/obj/error.o'
	$(CC) -c -o $(CONFIG)/obj/error.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/error.c

#
#   host.o
#
DEPS_33 += $(CONFIG)/inc/bit.h
DEPS_33 += src/http.h

$(CONFIG)/obj/host.o: \
    src/host.c $(DEPS_33)
	@echo '   [Compile] $(CONFIG)/obj/host.o'
	$(CC) -c -o $(CONFIG)/obj/host.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/host.c

#
#   httpService.o
#
DEPS_34 += $(CONFIG)/inc/bit.h
DEPS_34 += src/http.h

$(CONFIG)/obj/httpService.o: \
    src/httpService.c $(DEPS_34)
	@echo '   [Compile] $(CONFIG)/obj/httpService.o'
	$(CC) -c -o $(CONFIG)/obj/httpService.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/httpService.c

#
#   log.o
#
DEPS_35 += $(CONFIG)/inc/bit.h
DEPS_35 += src/http.h

$(CONFIG)/obj/log.o: \
    src/log.c $(DEPS_35)
	@echo '   [Compile] $(CONFIG)/obj/log.o'
	$(CC) -c -o $(CONFIG)/obj/log.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/log.c

#
#   netConnector.o
#
DEPS_36 += $(CONFIG)/inc/bit.h
DEPS_36 += src/http.h

$(CONFIG)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_36)
	@echo '   [Compile] $(CONFIG)/obj/netConnector.o'
	$(CC) -c -o $(CONFIG)/obj/netConnector.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/netConnector.c

#
#   packet.o
#
DEPS_37 += $(CONFIG)/inc/bit.h
DEPS_37 += src/http.h

$(CONFIG)/obj/packet.o: \
    src/packet.c $(DEPS_37)
	@echo '   [Compile] $(CONFIG)/obj/packet.o'
	$(CC) -c -o $(CONFIG)/obj/packet.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/packet.c

#
#   pam.o
#
DEPS_38 += $(CONFIG)/inc/bit.h
DEPS_38 += src/http.h

$(CONFIG)/obj/pam.o: \
    src/pam.c $(DEPS_38)
	@echo '   [Compile] $(CONFIG)/obj/pam.o'
	$(CC) -c -o $(CONFIG)/obj/pam.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/pam.c

#
#   passHandler.o
#
DEPS_39 += $(CONFIG)/inc/bit.h
DEPS_39 += src/http.h

$(CONFIG)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_39)
	@echo '   [Compile] $(CONFIG)/obj/passHandler.o'
	$(CC) -c -o $(CONFIG)/obj/passHandler.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/passHandler.c

#
#   pipeline.o
#
DEPS_40 += $(CONFIG)/inc/bit.h
DEPS_40 += src/http.h

$(CONFIG)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_40)
	@echo '   [Compile] $(CONFIG)/obj/pipeline.o'
	$(CC) -c -o $(CONFIG)/obj/pipeline.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/pipeline.c

#
#   queue.o
#
DEPS_41 += $(CONFIG)/inc/bit.h
DEPS_41 += src/http.h

$(CONFIG)/obj/queue.o: \
    src/queue.c $(DEPS_41)
	@echo '   [Compile] $(CONFIG)/obj/queue.o'
	$(CC) -c -o $(CONFIG)/obj/queue.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/queue.c

#
#   rangeFilter.o
#
DEPS_42 += $(CONFIG)/inc/bit.h
DEPS_42 += src/http.h

$(CONFIG)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_42)
	@echo '   [Compile] $(CONFIG)/obj/rangeFilter.o'
	$(CC) -c -o $(CONFIG)/obj/rangeFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/rangeFilter.c

#
#   route.o
#
DEPS_43 += $(CONFIG)/inc/bit.h
DEPS_43 += src/http.h

$(CONFIG)/obj/route.o: \
    src/route.c $(DEPS_43)
	@echo '   [Compile] $(CONFIG)/obj/route.o'
	$(CC) -c -o $(CONFIG)/obj/route.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/route.c

#
#   rx.o
#
DEPS_44 += $(CONFIG)/inc/bit.h
DEPS_44 += src/http.h

$(CONFIG)/obj/rx.o: \
    src/rx.c $(DEPS_44)
	@echo '   [Compile] $(CONFIG)/obj/rx.o'
	$(CC) -c -o $(CONFIG)/obj/rx.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/rx.c

#
#   sendConnector.o
#
DEPS_45 += $(CONFIG)/inc/bit.h
DEPS_45 += src/http.h

$(CONFIG)/obj/sendConnector.o: \
    src/sendConnector.c $(DEPS_45)
	@echo '   [Compile] $(CONFIG)/obj/sendConnector.o'
	$(CC) -c -o $(CONFIG)/obj/sendConnector.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/sendConnector.c

#
#   session.o
#
DEPS_46 += $(CONFIG)/inc/bit.h
DEPS_46 += src/http.h

$(CONFIG)/obj/session.o: \
    src/session.c $(DEPS_46)
	@echo '   [Compile] $(CONFIG)/obj/session.o'
	$(CC) -c -o $(CONFIG)/obj/session.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/session.c

#
#   stage.o
#
DEPS_47 += $(CONFIG)/inc/bit.h
DEPS_47 += src/http.h

$(CONFIG)/obj/stage.o: \
    src/stage.c $(DEPS_47)
	@echo '   [Compile] $(CONFIG)/obj/stage.o'
	$(CC) -c -o $(CONFIG)/obj/stage.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/stage.c

#
#   trace.o
#
DEPS_48 += $(CONFIG)/inc/bit.h
DEPS_48 += src/http.h

$(CONFIG)/obj/trace.o: \
    src/trace.c $(DEPS_48)
	@echo '   [Compile] $(CONFIG)/obj/trace.o'
	$(CC) -c -o $(CONFIG)/obj/trace.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/trace.c

#
#   tx.o
#
DEPS_49 += $(CONFIG)/inc/bit.h
DEPS_49 += src/http.h

$(CONFIG)/obj/tx.o: \
    src/tx.c $(DEPS_49)
	@echo '   [Compile] $(CONFIG)/obj/tx.o'
	$(CC) -c -o $(CONFIG)/obj/tx.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/tx.c

#
#   uploadFilter.o
#
DEPS_50 += $(CONFIG)/inc/bit.h
DEPS_50 += src/http.h

$(CONFIG)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_50)
	@echo '   [Compile] $(CONFIG)/obj/uploadFilter.o'
	$(CC) -c -o $(CONFIG)/obj/uploadFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/uploadFilter.c

#
#   uri.o
#
DEPS_51 += $(CONFIG)/inc/bit.h
DEPS_51 += src/http.h

$(CONFIG)/obj/uri.o: \
    src/uri.c $(DEPS_51)
	@echo '   [Compile] $(CONFIG)/obj/uri.o'
	$(CC) -c -o $(CONFIG)/obj/uri.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/uri.c

#
#   var.o
#
DEPS_52 += $(CONFIG)/inc/bit.h
DEPS_52 += src/http.h

$(CONFIG)/obj/var.o: \
    src/var.c $(DEPS_52)
	@echo '   [Compile] $(CONFIG)/obj/var.o'
	$(CC) -c -o $(CONFIG)/obj/var.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/var.c

#
#   webSock.o
#
DEPS_53 += $(CONFIG)/inc/bit.h
DEPS_53 += src/http.h

$(CONFIG)/obj/webSock.o: \
    src/webSock.c $(DEPS_53)
	@echo '   [Compile] $(CONFIG)/obj/webSock.o'
	$(CC) -c -o $(CONFIG)/obj/webSock.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/webSock.c

ifeq ($(BIT_PACK_PCRE),1)
#
#   libhttp
#
DEPS_54 += $(CONFIG)/bin/libmpr.a
DEPS_54 += $(CONFIG)/bin/libpcre.a
DEPS_54 += $(CONFIG)/inc/bitos.h
DEPS_54 += $(CONFIG)/inc/http.h
DEPS_54 += $(CONFIG)/obj/actionHandler.o
DEPS_54 += $(CONFIG)/obj/auth.o
DEPS_54 += $(CONFIG)/obj/basic.o
DEPS_54 += $(CONFIG)/obj/cache.o
DEPS_54 += $(CONFIG)/obj/chunkFilter.o
DEPS_54 += $(CONFIG)/obj/client.o
DEPS_54 += $(CONFIG)/obj/conn.o
DEPS_54 += $(CONFIG)/obj/digest.o
DEPS_54 += $(CONFIG)/obj/endpoint.o
DEPS_54 += $(CONFIG)/obj/error.o
DEPS_54 += $(CONFIG)/obj/host.o
DEPS_54 += $(CONFIG)/obj/httpService.o
DEPS_54 += $(CONFIG)/obj/log.o
DEPS_54 += $(CONFIG)/obj/netConnector.o
DEPS_54 += $(CONFIG)/obj/packet.o
DEPS_54 += $(CONFIG)/obj/pam.o
DEPS_54 += $(CONFIG)/obj/passHandler.o
DEPS_54 += $(CONFIG)/obj/pipeline.o
DEPS_54 += $(CONFIG)/obj/queue.o
DEPS_54 += $(CONFIG)/obj/rangeFilter.o
DEPS_54 += $(CONFIG)/obj/route.o
DEPS_54 += $(CONFIG)/obj/rx.o
DEPS_54 += $(CONFIG)/obj/sendConnector.o
DEPS_54 += $(CONFIG)/obj/session.o
DEPS_54 += $(CONFIG)/obj/stage.o
DEPS_54 += $(CONFIG)/obj/trace.o
DEPS_54 += $(CONFIG)/obj/tx.o
DEPS_54 += $(CONFIG)/obj/uploadFilter.o
DEPS_54 += $(CONFIG)/obj/uri.o
DEPS_54 += $(CONFIG)/obj/var.o
DEPS_54 += $(CONFIG)/obj/webSock.o

$(CONFIG)/bin/libhttp.a: $(DEPS_54)
	@echo '      [Link] $(CONFIG)/bin/libhttp.a'
	ar -cr $(CONFIG)/bin/libhttp.a $(CONFIG)/obj/actionHandler.o $(CONFIG)/obj/auth.o $(CONFIG)/obj/basic.o $(CONFIG)/obj/cache.o $(CONFIG)/obj/chunkFilter.o $(CONFIG)/obj/client.o $(CONFIG)/obj/conn.o $(CONFIG)/obj/digest.o $(CONFIG)/obj/endpoint.o $(CONFIG)/obj/error.o $(CONFIG)/obj/host.o $(CONFIG)/obj/httpService.o $(CONFIG)/obj/log.o $(CONFIG)/obj/netConnector.o $(CONFIG)/obj/packet.o $(CONFIG)/obj/pam.o $(CONFIG)/obj/passHandler.o $(CONFIG)/obj/pipeline.o $(CONFIG)/obj/queue.o $(CONFIG)/obj/rangeFilter.o $(CONFIG)/obj/route.o $(CONFIG)/obj/rx.o $(CONFIG)/obj/sendConnector.o $(CONFIG)/obj/session.o $(CONFIG)/obj/stage.o $(CONFIG)/obj/trace.o $(CONFIG)/obj/tx.o $(CONFIG)/obj/uploadFilter.o $(CONFIG)/obj/uri.o $(CONFIG)/obj/var.o $(CONFIG)/obj/webSock.o
endif

#
#   http.o
#
DEPS_55 += $(CONFIG)/inc/bit.h
DEPS_55 += src/http.h

$(CONFIG)/obj/http.o: \
    src/http.c $(DEPS_55)
	@echo '   [Compile] $(CONFIG)/obj/http.o'
	$(CC) -c -o $(CONFIG)/obj/http.o $(CFLAGS) $(DFLAGS) $(IFLAGS) src/http.c

#
#   httpcmd
#
ifeq ($(BIT_PACK_PCRE),1)
    DEPS_56 += $(CONFIG)/bin/libhttp.a
endif
DEPS_56 += $(CONFIG)/obj/http.o

LIBS_56 += -lmpr
ifeq ($(BIT_PACK_PCRE),1)
    LIBS_56 += -lpcre
endif
ifeq ($(BIT_PACK_PCRE),1)
    LIBS_56 += -lhttp
endif

$(CONFIG)/bin/http: $(DEPS_56)
	@echo '      [Link] $(CONFIG)/bin/http'
	$(CC) -o $(CONFIG)/bin/http $(LDFLAGS) $(LIBPATHS) $(CONFIG)/obj/http.o $(LIBPATHS_56) $(LIBS_56) $(LIBS_56) $(LIBS) -lpthread -lm -lrt -ldl $(LDFLAGS) 

#
#   stop
#
stop: $(DEPS_57)

#
#   installBinary
#
installBinary: $(DEPS_58)

#
#   start
#
start: $(DEPS_59)

#
#   install
#
DEPS_60 += stop
DEPS_60 += installBinary
DEPS_60 += start

install: $(DEPS_60)
	

#
#   uninstall
#
DEPS_61 += stop

uninstall: $(DEPS_61)

