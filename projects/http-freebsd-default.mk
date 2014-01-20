#
#   http-freebsd-default.mk -- Makefile to build Embedthis Http for freebsd
#

PRODUCT            := http
VERSION            := 4.5.0
BUILD_NUMBER       := 0
PROFILE            := default
ARCH               := $(shell uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/')
CC_ARCH            := $(shell echo $(ARCH) | sed 's/x86/i686/;s/x64/x86_64/')
OS                 := freebsd
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

CFLAGS             += -fPIC -w
DFLAGS             += -D_REENTRANT -DPIC $(patsubst %,-D%,$(filter BIT_%,$(MAKEFLAGS))) -DBIT_PACK_EST=$(BIT_PACK_EST) -DBIT_PACK_MATRIXSSL=$(BIT_PACK_MATRIXSSL) -DBIT_PACK_OPENSSL=$(BIT_PACK_OPENSSL) -DBIT_PACK_PCRE=$(BIT_PACK_PCRE) -DBIT_PACK_SSL=$(BIT_PACK_SSL) 
IFLAGS             += "-I$(CONFIG)/inc"
LDFLAGS            += 
LIBPATHS           += -L$(CONFIG)/bin
LIBS               += -ldl -lpthread -lm

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
TARGETS            += $(CONFIG)/bin/libest.so
endif
TARGETS            += $(CONFIG)/bin/ca.crt
ifeq ($(BIT_PACK_PCRE),1)
TARGETS            += $(CONFIG)/bin/libpcre.so
endif
TARGETS            += $(CONFIG)/bin/libmpr.so
TARGETS            += $(CONFIG)/bin/libmprssl.so
TARGETS            += $(CONFIG)/bin/makerom
TARGETS            += $(CONFIG)/bin/testHttp
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
	@[ ! -f $(CONFIG)/inc/bitos.h ] && cp src/bitos.h $(CONFIG)/inc/bitos.h ; true
	@if ! diff $(CONFIG)/inc/bitos.h src/bitos.h >/dev/null ; then\
		cp src/bitos.h $(CONFIG)/inc/bitos.h  ; \
	fi; true
	@[ ! -f $(CONFIG)/inc/bit.h ] && cp projects/http-freebsd-default-bit.h $(CONFIG)/inc/bit.h ; true
	@if ! diff $(CONFIG)/inc/bit.h projects/http-freebsd-default-bit.h >/dev/null ; then\
		cp projects/http-freebsd-default-bit.h $(CONFIG)/inc/bit.h  ; \
	fi; true
	@if [ -f "$(CONFIG)/.makeflags" ] ; then \
		if [ "$(MAKEFLAGS)" != " ` cat $(CONFIG)/.makeflags`" ] ; then \
			echo "   [Warning] Make flags have changed since the last build: "`cat $(CONFIG)/.makeflags`"" ; \
		fi ; \
	fi
	@echo $(MAKEFLAGS) >$(CONFIG)/.makeflags

clean:
	rm -f "$(CONFIG)/bin/libest.so"
	rm -f "$(CONFIG)/bin/ca.crt"
	rm -f "$(CONFIG)/bin/libpcre.so"
	rm -f "$(CONFIG)/bin/libmpr.so"
	rm -f "$(CONFIG)/bin/libmprssl.so"
	rm -f "$(CONFIG)/bin/makerom"
	rm -f "$(CONFIG)/bin/testHttp"
	rm -f "$(CONFIG)/bin/libhttp.so"
	rm -f "$(CONFIG)/bin/http"
	rm -f "$(CONFIG)/obj/estLib.o"
	rm -f "$(CONFIG)/obj/pcre.o"
	rm -f "$(CONFIG)/obj/mprLib.o"
	rm -f "$(CONFIG)/obj/mprSsl.o"
	rm -f "$(CONFIG)/obj/makerom.o"
	rm -f "$(CONFIG)/obj/testHttp.o"
	rm -f "$(CONFIG)/obj/testHttpGen.o"
	rm -f "$(CONFIG)/obj/testHttpUri.o"
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
	rm -f "$(CONFIG)/obj/log.o"
	rm -f "$(CONFIG)/obj/monitor.o"
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
	rm -f "$(CONFIG)/obj/service.o"
	rm -f "$(CONFIG)/obj/session.o"
	rm -f "$(CONFIG)/obj/stage.o"
	rm -f "$(CONFIG)/obj/trace.o"
	rm -f "$(CONFIG)/obj/tx.o"
	rm -f "$(CONFIG)/obj/uploadFilter.o"
	rm -f "$(CONFIG)/obj/uri.o"
	rm -f "$(CONFIG)/obj/var.o"
	rm -f "$(CONFIG)/obj/webSockFilter.o"
	rm -f "$(CONFIG)/obj/http.o"

clobber: clean
	rm -fr ./$(CONFIG)



#
#   version
#
version: $(DEPS_1)
	echo 4.5.0-0

#
#   est.h
#
$(CONFIG)/inc/est.h: $(DEPS_2)
	@echo '      [Copy] $(CONFIG)/inc/est.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/paks/est/est.h $(CONFIG)/inc/est.h

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
    src/paks/est/estLib.c $(DEPS_5)
	@echo '   [Compile] $(CONFIG)/obj/estLib.o'
	$(CC) -c -o $(CONFIG)/obj/estLib.o -fPIC $(DFLAGS) $(IFLAGS) "-Isrc" src/paks/est/estLib.c

ifeq ($(BIT_PACK_EST),1)
#
#   libest
#
DEPS_6 += $(CONFIG)/inc/est.h
DEPS_6 += $(CONFIG)/inc/bit.h
DEPS_6 += src/bitos.h
DEPS_6 += $(CONFIG)/obj/estLib.o

$(CONFIG)/bin/libest.so: $(DEPS_6)
	@echo '      [Link] $(CONFIG)/bin/libest.so'
	$(CC) -shared -o $(CONFIG)/bin/libest.so $(LIBPATHS) "$(CONFIG)/obj/estLib.o" $(LIBS) 
endif

#
#   ca-crt
#
DEPS_7 += src/paks/est/ca.crt

$(CONFIG)/bin/ca.crt: $(DEPS_7)
	@echo '      [Copy] $(CONFIG)/bin/ca.crt'
	mkdir -p "$(CONFIG)/bin"
	cp src/paks/est/ca.crt $(CONFIG)/bin/ca.crt

#
#   pcre.h
#
$(CONFIG)/inc/pcre.h: $(DEPS_8)
	@echo '      [Copy] $(CONFIG)/inc/pcre.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/paks/pcre/pcre.h $(CONFIG)/inc/pcre.h

#
#   pcre.o
#
DEPS_9 += $(CONFIG)/inc/bit.h
DEPS_9 += $(CONFIG)/inc/pcre.h

$(CONFIG)/obj/pcre.o: \
    src/paks/pcre/pcre.c $(DEPS_9)
	@echo '   [Compile] $(CONFIG)/obj/pcre.o'
	$(CC) -c -o $(CONFIG)/obj/pcre.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/paks/pcre/pcre.c

ifeq ($(BIT_PACK_PCRE),1)
#
#   libpcre
#
DEPS_10 += $(CONFIG)/inc/pcre.h
DEPS_10 += $(CONFIG)/inc/bit.h
DEPS_10 += $(CONFIG)/obj/pcre.o

$(CONFIG)/bin/libpcre.so: $(DEPS_10)
	@echo '      [Link] $(CONFIG)/bin/libpcre.so'
	$(CC) -shared -o $(CONFIG)/bin/libpcre.so $(LIBPATHS) "$(CONFIG)/obj/pcre.o" $(LIBS) 
endif

#
#   mpr.h
#
$(CONFIG)/inc/mpr.h: $(DEPS_11)
	@echo '      [Copy] $(CONFIG)/inc/mpr.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/paks/mpr/mpr.h $(CONFIG)/inc/mpr.h

#
#   mprLib.o
#
DEPS_12 += $(CONFIG)/inc/bit.h
DEPS_12 += $(CONFIG)/inc/mpr.h
DEPS_12 += src/bitos.h

$(CONFIG)/obj/mprLib.o: \
    src/paks/mpr/mprLib.c $(DEPS_12)
	@echo '   [Compile] $(CONFIG)/obj/mprLib.o'
	$(CC) -c -o $(CONFIG)/obj/mprLib.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/paks/mpr/mprLib.c

#
#   libmpr
#
DEPS_13 += $(CONFIG)/inc/mpr.h
DEPS_13 += $(CONFIG)/inc/bit.h
DEPS_13 += src/bitos.h
DEPS_13 += $(CONFIG)/obj/mprLib.o

$(CONFIG)/bin/libmpr.so: $(DEPS_13)
	@echo '      [Link] $(CONFIG)/bin/libmpr.so'
	$(CC) -shared -o $(CONFIG)/bin/libmpr.so $(LIBPATHS) "$(CONFIG)/obj/mprLib.o" $(LIBS) 

#
#   mprSsl.o
#
DEPS_14 += $(CONFIG)/inc/bit.h
DEPS_14 += $(CONFIG)/inc/mpr.h
DEPS_14 += $(CONFIG)/inc/est.h

$(CONFIG)/obj/mprSsl.o: \
    src/paks/mpr/mprSsl.c $(DEPS_14)
	@echo '   [Compile] $(CONFIG)/obj/mprSsl.o'
	$(CC) -c -o $(CONFIG)/obj/mprSsl.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" "-I$(BIT_PACK_MATRIXSSL_PATH)" "-I$(BIT_PACK_MATRIXSSL_PATH)/matrixssl" "-I$(BIT_PACK_NANOSSL_PATH)/src" "-I$(BIT_PACK_OPENSSL_PATH)/include" src/paks/mpr/mprSsl.c

#
#   libmprssl
#
DEPS_15 += $(CONFIG)/inc/mpr.h
DEPS_15 += $(CONFIG)/inc/bit.h
DEPS_15 += src/bitos.h
DEPS_15 += $(CONFIG)/obj/mprLib.o
DEPS_15 += $(CONFIG)/bin/libmpr.so
DEPS_15 += $(CONFIG)/inc/est.h
DEPS_15 += $(CONFIG)/obj/estLib.o
ifeq ($(BIT_PACK_EST),1)
    DEPS_15 += $(CONFIG)/bin/libest.so
endif
DEPS_15 += $(CONFIG)/obj/mprSsl.o

LIBS_15 += -lmpr
ifeq ($(BIT_PACK_EST),1)
    LIBS_15 += -lest
endif
ifeq ($(BIT_PACK_MATRIXSSL),1)
    LIBS_15 += -lmatrixssl
    LIBPATHS_15 += -L$(BIT_PACK_MATRIXSSL_PATH)
endif
ifeq ($(BIT_PACK_NANOSSL),1)
    LIBS_15 += -lssls
    LIBPATHS_15 += -L$(BIT_PACK_NANOSSL_PATH)/bin
endif
ifeq ($(BIT_PACK_OPENSSL),1)
    LIBS_15 += -lssl
    LIBPATHS_15 += -L$(BIT_PACK_OPENSSL_PATH)
endif
ifeq ($(BIT_PACK_OPENSSL),1)
    LIBS_15 += -lcrypto
    LIBPATHS_15 += -L$(BIT_PACK_OPENSSL_PATH)
endif

$(CONFIG)/bin/libmprssl.so: $(DEPS_15)
	@echo '      [Link] $(CONFIG)/bin/libmprssl.so'
	$(CC) -shared -o $(CONFIG)/bin/libmprssl.so $(LIBPATHS)    "$(CONFIG)/obj/mprSsl.o" $(LIBPATHS_15) $(LIBS_15) $(LIBS_15) $(LIBS) 

#
#   makerom.o
#
DEPS_16 += $(CONFIG)/inc/bit.h
DEPS_16 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/makerom.o: \
    src/paks/mpr/makerom.c $(DEPS_16)
	@echo '   [Compile] $(CONFIG)/obj/makerom.o'
	$(CC) -c -o $(CONFIG)/obj/makerom.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/paks/mpr/makerom.c

#
#   makerom
#
DEPS_17 += $(CONFIG)/inc/mpr.h
DEPS_17 += $(CONFIG)/inc/bit.h
DEPS_17 += src/bitos.h
DEPS_17 += $(CONFIG)/obj/mprLib.o
DEPS_17 += $(CONFIG)/bin/libmpr.so
DEPS_17 += $(CONFIG)/obj/makerom.o

LIBS_17 += -lmpr

$(CONFIG)/bin/makerom: $(DEPS_17)
	@echo '      [Link] $(CONFIG)/bin/makerom'
	$(CC) -o $(CONFIG)/bin/makerom $(LIBPATHS) "$(CONFIG)/obj/makerom.o" $(LIBPATHS_17) $(LIBS_17) $(LIBS_17) $(LIBS) $(LIBS) 

#
#   bitos.h
#
$(CONFIG)/inc/bitos.h: $(DEPS_18)
	@echo '      [Copy] $(CONFIG)/inc/bitos.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/bitos.h $(CONFIG)/inc/bitos.h

#
#   http.h
#
$(CONFIG)/inc/http.h: $(DEPS_19)
	@echo '      [Copy] $(CONFIG)/inc/http.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/http.h $(CONFIG)/inc/http.h

#
#   http.h
#
src/http.h: $(DEPS_20)
	@echo '      [Copy] src/http.h'

#
#   actionHandler.o
#
DEPS_21 += $(CONFIG)/inc/bit.h
DEPS_21 += src/http.h

$(CONFIG)/obj/actionHandler.o: \
    src/actionHandler.c $(DEPS_21)
	@echo '   [Compile] $(CONFIG)/obj/actionHandler.o'
	$(CC) -c -o $(CONFIG)/obj/actionHandler.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/actionHandler.c

#
#   auth.o
#
DEPS_22 += $(CONFIG)/inc/bit.h
DEPS_22 += src/http.h

$(CONFIG)/obj/auth.o: \
    src/auth.c $(DEPS_22)
	@echo '   [Compile] $(CONFIG)/obj/auth.o'
	$(CC) -c -o $(CONFIG)/obj/auth.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/auth.c

#
#   basic.o
#
DEPS_23 += $(CONFIG)/inc/bit.h
DEPS_23 += src/http.h

$(CONFIG)/obj/basic.o: \
    src/basic.c $(DEPS_23)
	@echo '   [Compile] $(CONFIG)/obj/basic.o'
	$(CC) -c -o $(CONFIG)/obj/basic.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/basic.c

#
#   cache.o
#
DEPS_24 += $(CONFIG)/inc/bit.h
DEPS_24 += src/http.h

$(CONFIG)/obj/cache.o: \
    src/cache.c $(DEPS_24)
	@echo '   [Compile] $(CONFIG)/obj/cache.o'
	$(CC) -c -o $(CONFIG)/obj/cache.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/cache.c

#
#   chunkFilter.o
#
DEPS_25 += $(CONFIG)/inc/bit.h
DEPS_25 += src/http.h

$(CONFIG)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_25)
	@echo '   [Compile] $(CONFIG)/obj/chunkFilter.o'
	$(CC) -c -o $(CONFIG)/obj/chunkFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/chunkFilter.c

#
#   client.o
#
DEPS_26 += $(CONFIG)/inc/bit.h
DEPS_26 += src/http.h

$(CONFIG)/obj/client.o: \
    src/client.c $(DEPS_26)
	@echo '   [Compile] $(CONFIG)/obj/client.o'
	$(CC) -c -o $(CONFIG)/obj/client.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/client.c

#
#   conn.o
#
DEPS_27 += $(CONFIG)/inc/bit.h
DEPS_27 += src/http.h

$(CONFIG)/obj/conn.o: \
    src/conn.c $(DEPS_27)
	@echo '   [Compile] $(CONFIG)/obj/conn.o'
	$(CC) -c -o $(CONFIG)/obj/conn.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/conn.c

#
#   digest.o
#
DEPS_28 += $(CONFIG)/inc/bit.h
DEPS_28 += src/http.h

$(CONFIG)/obj/digest.o: \
    src/digest.c $(DEPS_28)
	@echo '   [Compile] $(CONFIG)/obj/digest.o'
	$(CC) -c -o $(CONFIG)/obj/digest.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/digest.c

#
#   endpoint.o
#
DEPS_29 += $(CONFIG)/inc/bit.h
DEPS_29 += src/http.h

$(CONFIG)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_29)
	@echo '   [Compile] $(CONFIG)/obj/endpoint.o'
	$(CC) -c -o $(CONFIG)/obj/endpoint.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/endpoint.c

#
#   error.o
#
DEPS_30 += $(CONFIG)/inc/bit.h
DEPS_30 += src/http.h

$(CONFIG)/obj/error.o: \
    src/error.c $(DEPS_30)
	@echo '   [Compile] $(CONFIG)/obj/error.o'
	$(CC) -c -o $(CONFIG)/obj/error.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/error.c

#
#   host.o
#
DEPS_31 += $(CONFIG)/inc/bit.h
DEPS_31 += src/http.h

$(CONFIG)/obj/host.o: \
    src/host.c $(DEPS_31)
	@echo '   [Compile] $(CONFIG)/obj/host.o'
	$(CC) -c -o $(CONFIG)/obj/host.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/host.c

#
#   log.o
#
DEPS_32 += $(CONFIG)/inc/bit.h
DEPS_32 += src/http.h

$(CONFIG)/obj/log.o: \
    src/log.c $(DEPS_32)
	@echo '   [Compile] $(CONFIG)/obj/log.o'
	$(CC) -c -o $(CONFIG)/obj/log.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/log.c

#
#   monitor.o
#
DEPS_33 += $(CONFIG)/inc/bit.h
DEPS_33 += src/http.h

$(CONFIG)/obj/monitor.o: \
    src/monitor.c $(DEPS_33)
	@echo '   [Compile] $(CONFIG)/obj/monitor.o'
	$(CC) -c -o $(CONFIG)/obj/monitor.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/monitor.c

#
#   netConnector.o
#
DEPS_34 += $(CONFIG)/inc/bit.h
DEPS_34 += src/http.h

$(CONFIG)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_34)
	@echo '   [Compile] $(CONFIG)/obj/netConnector.o'
	$(CC) -c -o $(CONFIG)/obj/netConnector.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/netConnector.c

#
#   packet.o
#
DEPS_35 += $(CONFIG)/inc/bit.h
DEPS_35 += src/http.h

$(CONFIG)/obj/packet.o: \
    src/packet.c $(DEPS_35)
	@echo '   [Compile] $(CONFIG)/obj/packet.o'
	$(CC) -c -o $(CONFIG)/obj/packet.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/packet.c

#
#   pam.o
#
DEPS_36 += $(CONFIG)/inc/bit.h
DEPS_36 += src/http.h

$(CONFIG)/obj/pam.o: \
    src/pam.c $(DEPS_36)
	@echo '   [Compile] $(CONFIG)/obj/pam.o'
	$(CC) -c -o $(CONFIG)/obj/pam.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/pam.c

#
#   passHandler.o
#
DEPS_37 += $(CONFIG)/inc/bit.h
DEPS_37 += src/http.h

$(CONFIG)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_37)
	@echo '   [Compile] $(CONFIG)/obj/passHandler.o'
	$(CC) -c -o $(CONFIG)/obj/passHandler.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/passHandler.c

#
#   pipeline.o
#
DEPS_38 += $(CONFIG)/inc/bit.h
DEPS_38 += src/http.h

$(CONFIG)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_38)
	@echo '   [Compile] $(CONFIG)/obj/pipeline.o'
	$(CC) -c -o $(CONFIG)/obj/pipeline.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/pipeline.c

#
#   queue.o
#
DEPS_39 += $(CONFIG)/inc/bit.h
DEPS_39 += src/http.h

$(CONFIG)/obj/queue.o: \
    src/queue.c $(DEPS_39)
	@echo '   [Compile] $(CONFIG)/obj/queue.o'
	$(CC) -c -o $(CONFIG)/obj/queue.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/queue.c

#
#   rangeFilter.o
#
DEPS_40 += $(CONFIG)/inc/bit.h
DEPS_40 += src/http.h

$(CONFIG)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_40)
	@echo '   [Compile] $(CONFIG)/obj/rangeFilter.o'
	$(CC) -c -o $(CONFIG)/obj/rangeFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/rangeFilter.c

#
#   route.o
#
DEPS_41 += $(CONFIG)/inc/bit.h
DEPS_41 += src/http.h

$(CONFIG)/obj/route.o: \
    src/route.c $(DEPS_41)
	@echo '   [Compile] $(CONFIG)/obj/route.o'
	$(CC) -c -o $(CONFIG)/obj/route.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/route.c

#
#   rx.o
#
DEPS_42 += $(CONFIG)/inc/bit.h
DEPS_42 += src/http.h

$(CONFIG)/obj/rx.o: \
    src/rx.c $(DEPS_42)
	@echo '   [Compile] $(CONFIG)/obj/rx.o'
	$(CC) -c -o $(CONFIG)/obj/rx.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/rx.c

#
#   sendConnector.o
#
DEPS_43 += $(CONFIG)/inc/bit.h
DEPS_43 += src/http.h

$(CONFIG)/obj/sendConnector.o: \
    src/sendConnector.c $(DEPS_43)
	@echo '   [Compile] $(CONFIG)/obj/sendConnector.o'
	$(CC) -c -o $(CONFIG)/obj/sendConnector.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/sendConnector.c

#
#   service.o
#
DEPS_44 += $(CONFIG)/inc/bit.h
DEPS_44 += src/http.h

$(CONFIG)/obj/service.o: \
    src/service.c $(DEPS_44)
	@echo '   [Compile] $(CONFIG)/obj/service.o'
	$(CC) -c -o $(CONFIG)/obj/service.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/service.c

#
#   session.o
#
DEPS_45 += $(CONFIG)/inc/bit.h
DEPS_45 += src/http.h

$(CONFIG)/obj/session.o: \
    src/session.c $(DEPS_45)
	@echo '   [Compile] $(CONFIG)/obj/session.o'
	$(CC) -c -o $(CONFIG)/obj/session.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/session.c

#
#   stage.o
#
DEPS_46 += $(CONFIG)/inc/bit.h
DEPS_46 += src/http.h

$(CONFIG)/obj/stage.o: \
    src/stage.c $(DEPS_46)
	@echo '   [Compile] $(CONFIG)/obj/stage.o'
	$(CC) -c -o $(CONFIG)/obj/stage.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/stage.c

#
#   trace.o
#
DEPS_47 += $(CONFIG)/inc/bit.h
DEPS_47 += src/http.h

$(CONFIG)/obj/trace.o: \
    src/trace.c $(DEPS_47)
	@echo '   [Compile] $(CONFIG)/obj/trace.o'
	$(CC) -c -o $(CONFIG)/obj/trace.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/trace.c

#
#   tx.o
#
DEPS_48 += $(CONFIG)/inc/bit.h
DEPS_48 += src/http.h

$(CONFIG)/obj/tx.o: \
    src/tx.c $(DEPS_48)
	@echo '   [Compile] $(CONFIG)/obj/tx.o'
	$(CC) -c -o $(CONFIG)/obj/tx.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/tx.c

#
#   uploadFilter.o
#
DEPS_49 += $(CONFIG)/inc/bit.h
DEPS_49 += src/http.h

$(CONFIG)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_49)
	@echo '   [Compile] $(CONFIG)/obj/uploadFilter.o'
	$(CC) -c -o $(CONFIG)/obj/uploadFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/uploadFilter.c

#
#   uri.o
#
DEPS_50 += $(CONFIG)/inc/bit.h
DEPS_50 += src/http.h

$(CONFIG)/obj/uri.o: \
    src/uri.c $(DEPS_50)
	@echo '   [Compile] $(CONFIG)/obj/uri.o'
	$(CC) -c -o $(CONFIG)/obj/uri.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/uri.c

#
#   var.o
#
DEPS_51 += $(CONFIG)/inc/bit.h
DEPS_51 += src/http.h

$(CONFIG)/obj/var.o: \
    src/var.c $(DEPS_51)
	@echo '   [Compile] $(CONFIG)/obj/var.o'
	$(CC) -c -o $(CONFIG)/obj/var.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/var.c

#
#   webSockFilter.o
#
DEPS_52 += $(CONFIG)/inc/bit.h
DEPS_52 += src/http.h

$(CONFIG)/obj/webSockFilter.o: \
    src/webSockFilter.c $(DEPS_52)
	@echo '   [Compile] $(CONFIG)/obj/webSockFilter.o'
	$(CC) -c -o $(CONFIG)/obj/webSockFilter.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/webSockFilter.c

ifeq ($(BIT_PACK_PCRE),1)
#
#   libhttp
#
DEPS_53 += $(CONFIG)/inc/mpr.h
DEPS_53 += $(CONFIG)/inc/bit.h
DEPS_53 += src/bitos.h
DEPS_53 += $(CONFIG)/obj/mprLib.o
DEPS_53 += $(CONFIG)/bin/libmpr.so
DEPS_53 += $(CONFIG)/inc/pcre.h
DEPS_53 += $(CONFIG)/obj/pcre.o
DEPS_53 += $(CONFIG)/bin/libpcre.so
DEPS_53 += $(CONFIG)/inc/bitos.h
DEPS_53 += $(CONFIG)/inc/http.h
DEPS_53 += src/http.h
DEPS_53 += $(CONFIG)/obj/actionHandler.o
DEPS_53 += $(CONFIG)/obj/auth.o
DEPS_53 += $(CONFIG)/obj/basic.o
DEPS_53 += $(CONFIG)/obj/cache.o
DEPS_53 += $(CONFIG)/obj/chunkFilter.o
DEPS_53 += $(CONFIG)/obj/client.o
DEPS_53 += $(CONFIG)/obj/conn.o
DEPS_53 += $(CONFIG)/obj/digest.o
DEPS_53 += $(CONFIG)/obj/endpoint.o
DEPS_53 += $(CONFIG)/obj/error.o
DEPS_53 += $(CONFIG)/obj/host.o
DEPS_53 += $(CONFIG)/obj/log.o
DEPS_53 += $(CONFIG)/obj/monitor.o
DEPS_53 += $(CONFIG)/obj/netConnector.o
DEPS_53 += $(CONFIG)/obj/packet.o
DEPS_53 += $(CONFIG)/obj/pam.o
DEPS_53 += $(CONFIG)/obj/passHandler.o
DEPS_53 += $(CONFIG)/obj/pipeline.o
DEPS_53 += $(CONFIG)/obj/queue.o
DEPS_53 += $(CONFIG)/obj/rangeFilter.o
DEPS_53 += $(CONFIG)/obj/route.o
DEPS_53 += $(CONFIG)/obj/rx.o
DEPS_53 += $(CONFIG)/obj/sendConnector.o
DEPS_53 += $(CONFIG)/obj/service.o
DEPS_53 += $(CONFIG)/obj/session.o
DEPS_53 += $(CONFIG)/obj/stage.o
DEPS_53 += $(CONFIG)/obj/trace.o
DEPS_53 += $(CONFIG)/obj/tx.o
DEPS_53 += $(CONFIG)/obj/uploadFilter.o
DEPS_53 += $(CONFIG)/obj/uri.o
DEPS_53 += $(CONFIG)/obj/var.o
DEPS_53 += $(CONFIG)/obj/webSockFilter.o

LIBS_53 += -lmpr
LIBS_53 += -lpcre

$(CONFIG)/bin/libhttp.so: $(DEPS_53)
	@echo '      [Link] $(CONFIG)/bin/libhttp.so'
	$(CC) -shared -o $(CONFIG)/bin/libhttp.so $(LIBPATHS) "$(CONFIG)/obj/actionHandler.o" "$(CONFIG)/obj/auth.o" "$(CONFIG)/obj/basic.o" "$(CONFIG)/obj/cache.o" "$(CONFIG)/obj/chunkFilter.o" "$(CONFIG)/obj/client.o" "$(CONFIG)/obj/conn.o" "$(CONFIG)/obj/digest.o" "$(CONFIG)/obj/endpoint.o" "$(CONFIG)/obj/error.o" "$(CONFIG)/obj/host.o" "$(CONFIG)/obj/log.o" "$(CONFIG)/obj/monitor.o" "$(CONFIG)/obj/netConnector.o" "$(CONFIG)/obj/packet.o" "$(CONFIG)/obj/pam.o" "$(CONFIG)/obj/passHandler.o" "$(CONFIG)/obj/pipeline.o" "$(CONFIG)/obj/queue.o" "$(CONFIG)/obj/rangeFilter.o" "$(CONFIG)/obj/route.o" "$(CONFIG)/obj/rx.o" "$(CONFIG)/obj/sendConnector.o" "$(CONFIG)/obj/service.o" "$(CONFIG)/obj/session.o" "$(CONFIG)/obj/stage.o" "$(CONFIG)/obj/trace.o" "$(CONFIG)/obj/tx.o" "$(CONFIG)/obj/uploadFilter.o" "$(CONFIG)/obj/uri.o" "$(CONFIG)/obj/var.o" "$(CONFIG)/obj/webSockFilter.o" $(LIBPATHS_53) $(LIBS_53) $(LIBS_53) $(LIBS) 
endif

#
#   testHttp.o
#
DEPS_54 += $(CONFIG)/inc/bit.h
DEPS_54 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/testHttp.o: \
    test/src/testHttp.c $(DEPS_54)
	@echo '   [Compile] $(CONFIG)/obj/testHttp.o'
	$(CC) -c -o $(CONFIG)/obj/testHttp.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" test/src/testHttp.c

#
#   testHttpGen.o
#
DEPS_55 += $(CONFIG)/inc/bit.h
DEPS_55 += src/http.h
DEPS_55 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/testHttpGen.o: \
    test/src/testHttpGen.c $(DEPS_55)
	@echo '   [Compile] $(CONFIG)/obj/testHttpGen.o'
	$(CC) -c -o $(CONFIG)/obj/testHttpGen.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" test/src/testHttpGen.c

#
#   testHttpUri.o
#
DEPS_56 += $(CONFIG)/inc/bit.h
DEPS_56 += src/http.h

$(CONFIG)/obj/testHttpUri.o: \
    test/src/testHttpUri.c $(DEPS_56)
	@echo '   [Compile] $(CONFIG)/obj/testHttpUri.o'
	$(CC) -c -o $(CONFIG)/obj/testHttpUri.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" test/src/testHttpUri.c

#
#   testHttp
#
DEPS_57 += $(CONFIG)/inc/mpr.h
DEPS_57 += $(CONFIG)/inc/bit.h
DEPS_57 += src/bitos.h
DEPS_57 += $(CONFIG)/obj/mprLib.o
DEPS_57 += $(CONFIG)/bin/libmpr.so
DEPS_57 += $(CONFIG)/inc/pcre.h
DEPS_57 += $(CONFIG)/obj/pcre.o
ifeq ($(BIT_PACK_PCRE),1)
    DEPS_57 += $(CONFIG)/bin/libpcre.so
endif
DEPS_57 += $(CONFIG)/inc/bitos.h
DEPS_57 += $(CONFIG)/inc/http.h
DEPS_57 += src/http.h
DEPS_57 += $(CONFIG)/obj/actionHandler.o
DEPS_57 += $(CONFIG)/obj/auth.o
DEPS_57 += $(CONFIG)/obj/basic.o
DEPS_57 += $(CONFIG)/obj/cache.o
DEPS_57 += $(CONFIG)/obj/chunkFilter.o
DEPS_57 += $(CONFIG)/obj/client.o
DEPS_57 += $(CONFIG)/obj/conn.o
DEPS_57 += $(CONFIG)/obj/digest.o
DEPS_57 += $(CONFIG)/obj/endpoint.o
DEPS_57 += $(CONFIG)/obj/error.o
DEPS_57 += $(CONFIG)/obj/host.o
DEPS_57 += $(CONFIG)/obj/log.o
DEPS_57 += $(CONFIG)/obj/monitor.o
DEPS_57 += $(CONFIG)/obj/netConnector.o
DEPS_57 += $(CONFIG)/obj/packet.o
DEPS_57 += $(CONFIG)/obj/pam.o
DEPS_57 += $(CONFIG)/obj/passHandler.o
DEPS_57 += $(CONFIG)/obj/pipeline.o
DEPS_57 += $(CONFIG)/obj/queue.o
DEPS_57 += $(CONFIG)/obj/rangeFilter.o
DEPS_57 += $(CONFIG)/obj/route.o
DEPS_57 += $(CONFIG)/obj/rx.o
DEPS_57 += $(CONFIG)/obj/sendConnector.o
DEPS_57 += $(CONFIG)/obj/service.o
DEPS_57 += $(CONFIG)/obj/session.o
DEPS_57 += $(CONFIG)/obj/stage.o
DEPS_57 += $(CONFIG)/obj/trace.o
DEPS_57 += $(CONFIG)/obj/tx.o
DEPS_57 += $(CONFIG)/obj/uploadFilter.o
DEPS_57 += $(CONFIG)/obj/uri.o
DEPS_57 += $(CONFIG)/obj/var.o
DEPS_57 += $(CONFIG)/obj/webSockFilter.o
ifeq ($(BIT_PACK_PCRE),1)
    DEPS_57 += $(CONFIG)/bin/libhttp.so
endif
DEPS_57 += $(CONFIG)/obj/testHttp.o
DEPS_57 += $(CONFIG)/obj/testHttpGen.o
DEPS_57 += $(CONFIG)/obj/testHttpUri.o

ifeq ($(BIT_PACK_PCRE),1)
    LIBS_57 += -lhttp
endif
LIBS_57 += -lmpr
ifeq ($(BIT_PACK_PCRE),1)
    LIBS_57 += -lpcre
endif

$(CONFIG)/bin/testHttp: $(DEPS_57)
	@echo '      [Link] $(CONFIG)/bin/testHttp'
	$(CC) -o $(CONFIG)/bin/testHttp $(LIBPATHS) "$(CONFIG)/obj/testHttp.o" "$(CONFIG)/obj/testHttpGen.o" "$(CONFIG)/obj/testHttpUri.o" $(LIBPATHS_57) $(LIBS_57) $(LIBS_57) $(LIBS) $(LIBS) 

#
#   http.o
#
DEPS_58 += $(CONFIG)/inc/bit.h
DEPS_58 += src/http.h

$(CONFIG)/obj/http.o: \
    src/http.c $(DEPS_58)
	@echo '   [Compile] $(CONFIG)/obj/http.o'
	$(CC) -c -o $(CONFIG)/obj/http.o $(CFLAGS) $(DFLAGS) $(IFLAGS) "-Isrc" src/http.c

#
#   httpcmd
#
DEPS_59 += $(CONFIG)/inc/mpr.h
DEPS_59 += $(CONFIG)/inc/bit.h
DEPS_59 += src/bitos.h
DEPS_59 += $(CONFIG)/obj/mprLib.o
DEPS_59 += $(CONFIG)/bin/libmpr.so
DEPS_59 += $(CONFIG)/inc/pcre.h
DEPS_59 += $(CONFIG)/obj/pcre.o
ifeq ($(BIT_PACK_PCRE),1)
    DEPS_59 += $(CONFIG)/bin/libpcre.so
endif
DEPS_59 += $(CONFIG)/inc/bitos.h
DEPS_59 += $(CONFIG)/inc/http.h
DEPS_59 += src/http.h
DEPS_59 += $(CONFIG)/obj/actionHandler.o
DEPS_59 += $(CONFIG)/obj/auth.o
DEPS_59 += $(CONFIG)/obj/basic.o
DEPS_59 += $(CONFIG)/obj/cache.o
DEPS_59 += $(CONFIG)/obj/chunkFilter.o
DEPS_59 += $(CONFIG)/obj/client.o
DEPS_59 += $(CONFIG)/obj/conn.o
DEPS_59 += $(CONFIG)/obj/digest.o
DEPS_59 += $(CONFIG)/obj/endpoint.o
DEPS_59 += $(CONFIG)/obj/error.o
DEPS_59 += $(CONFIG)/obj/host.o
DEPS_59 += $(CONFIG)/obj/log.o
DEPS_59 += $(CONFIG)/obj/monitor.o
DEPS_59 += $(CONFIG)/obj/netConnector.o
DEPS_59 += $(CONFIG)/obj/packet.o
DEPS_59 += $(CONFIG)/obj/pam.o
DEPS_59 += $(CONFIG)/obj/passHandler.o
DEPS_59 += $(CONFIG)/obj/pipeline.o
DEPS_59 += $(CONFIG)/obj/queue.o
DEPS_59 += $(CONFIG)/obj/rangeFilter.o
DEPS_59 += $(CONFIG)/obj/route.o
DEPS_59 += $(CONFIG)/obj/rx.o
DEPS_59 += $(CONFIG)/obj/sendConnector.o
DEPS_59 += $(CONFIG)/obj/service.o
DEPS_59 += $(CONFIG)/obj/session.o
DEPS_59 += $(CONFIG)/obj/stage.o
DEPS_59 += $(CONFIG)/obj/trace.o
DEPS_59 += $(CONFIG)/obj/tx.o
DEPS_59 += $(CONFIG)/obj/uploadFilter.o
DEPS_59 += $(CONFIG)/obj/uri.o
DEPS_59 += $(CONFIG)/obj/var.o
DEPS_59 += $(CONFIG)/obj/webSockFilter.o
ifeq ($(BIT_PACK_PCRE),1)
    DEPS_59 += $(CONFIG)/bin/libhttp.so
endif
DEPS_59 += $(CONFIG)/obj/http.o

ifeq ($(BIT_PACK_PCRE),1)
    LIBS_59 += -lhttp
endif
LIBS_59 += -lmpr
ifeq ($(BIT_PACK_PCRE),1)
    LIBS_59 += -lpcre
endif

$(CONFIG)/bin/http: $(DEPS_59)
	@echo '      [Link] $(CONFIG)/bin/http'
	$(CC) -o $(CONFIG)/bin/http $(LIBPATHS) "$(CONFIG)/obj/http.o" $(LIBPATHS_59) $(LIBS_59) $(LIBS_59) $(LIBS) $(LIBS) 

#
#   stop
#
stop: $(DEPS_60)

#
#   installBinary
#
installBinary: $(DEPS_61)

#
#   start
#
start: $(DEPS_62)

#
#   install
#
DEPS_63 += stop
DEPS_63 += installBinary
DEPS_63 += start

install: $(DEPS_63)

#
#   uninstall
#
DEPS_64 += stop

uninstall: $(DEPS_64)

