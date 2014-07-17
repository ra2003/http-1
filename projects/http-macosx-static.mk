#
#   http-macosx-static.mk -- Makefile to build Embedthis Http for macosx
#

NAME                  := http
VERSION               := 5.0.0
PROFILE               ?= static
ARCH                  ?= $(shell uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/')
CC_ARCH               ?= $(shell echo $(ARCH) | sed 's/x86/i686/;s/x64/x86_64/')
OS                    ?= macosx
CC                    ?= clang
CONFIG                ?= $(OS)-$(ARCH)-$(PROFILE)
BUILD                 ?= build/$(CONFIG)
LBIN                  ?= $(BUILD)/bin
PATH                  := $(LBIN):$(PATH)

ME_COM_EST            ?= 1
ME_COM_MATRIXSSL      ?= 0
ME_COM_NANOSSL        ?= 0
ME_COM_OPENSSL        ?= 0
ME_COM_PCRE           ?= 1
ME_COM_SSL            ?= 1
ME_COM_VXWORKS        ?= 0
ME_COM_WINSDK         ?= 1

ifeq ($(ME_COM_EST),1)
    ME_COM_SSL := 1
endif
ifeq ($(ME_COM_MATRIXSSL),1)
    ME_COM_SSL := 1
endif
ifeq ($(ME_COM_NANOSSL),1)
    ME_COM_SSL := 1
endif
ifeq ($(ME_COM_OPENSSL),1)
    ME_COM_SSL := 1
endif

ME_COM_COMPILER_PATH  ?= clang
ME_COM_LIB_PATH       ?= ar
ME_COM_MATRIXSSL_PATH ?= /usr/src/matrixssl
ME_COM_NANOSSL_PATH   ?= /usr/src/nanossl
ME_COM_OPENSSL_PATH   ?= /usr/src/openssl

CFLAGS                += -g -w
DFLAGS                +=  $(patsubst %,-D%,$(filter ME_%,$(MAKEFLAGS))) -DME_COM_EST=$(ME_COM_EST) -DME_COM_MATRIXSSL=$(ME_COM_MATRIXSSL) -DME_COM_NANOSSL=$(ME_COM_NANOSSL) -DME_COM_OPENSSL=$(ME_COM_OPENSSL) -DME_COM_PCRE=$(ME_COM_PCRE) -DME_COM_SSL=$(ME_COM_SSL) -DME_COM_VXWORKS=$(ME_COM_VXWORKS) -DME_COM_WINSDK=$(ME_COM_WINSDK) 
IFLAGS                += "-Ibuild/$(CONFIG)/inc"
LDFLAGS               += '-Wl,-rpath,@executable_path/' '-Wl,-rpath,@loader_path/'
LIBPATHS              += -Lbuild/$(CONFIG)/bin
LIBS                  += -ldl -lpthread -lm

DEBUG                 ?= debug
CFLAGS-debug          ?= -g
DFLAGS-debug          ?= -DME_DEBUG
LDFLAGS-debug         ?= -g
DFLAGS-release        ?= 
CFLAGS-release        ?= -O2
LDFLAGS-release       ?= 
CFLAGS                += $(CFLAGS-$(DEBUG))
DFLAGS                += $(DFLAGS-$(DEBUG))
LDFLAGS               += $(LDFLAGS-$(DEBUG))

ME_ROOT_PREFIX        ?= 
ME_BASE_PREFIX        ?= $(ME_ROOT_PREFIX)/usr/local
ME_DATA_PREFIX        ?= $(ME_ROOT_PREFIX)/
ME_STATE_PREFIX       ?= $(ME_ROOT_PREFIX)/var
ME_APP_PREFIX         ?= $(ME_BASE_PREFIX)/lib/$(NAME)
ME_VAPP_PREFIX        ?= $(ME_APP_PREFIX)/$(VERSION)
ME_BIN_PREFIX         ?= $(ME_ROOT_PREFIX)/usr/local/bin
ME_INC_PREFIX         ?= $(ME_ROOT_PREFIX)/usr/local/include
ME_LIB_PREFIX         ?= $(ME_ROOT_PREFIX)/usr/local/lib
ME_MAN_PREFIX         ?= $(ME_ROOT_PREFIX)/usr/local/share/man
ME_SBIN_PREFIX        ?= $(ME_ROOT_PREFIX)/usr/local/sbin
ME_ETC_PREFIX         ?= $(ME_ROOT_PREFIX)/etc/$(NAME)
ME_WEB_PREFIX         ?= $(ME_ROOT_PREFIX)/var/www/$(NAME)-default
ME_LOG_PREFIX         ?= $(ME_ROOT_PREFIX)/var/log/$(NAME)
ME_SPOOL_PREFIX       ?= $(ME_ROOT_PREFIX)/var/spool/$(NAME)
ME_CACHE_PREFIX       ?= $(ME_ROOT_PREFIX)/var/spool/$(NAME)/cache
ME_SRC_PREFIX         ?= $(ME_ROOT_PREFIX)$(NAME)-$(VERSION)


TARGETS               += build/$(CONFIG)/bin/ca.crt
TARGETS               += build/$(CONFIG)/bin/http
ifeq ($(ME_COM_EST),1)
    TARGETS           += build/$(CONFIG)/bin/libest.a
endif
TARGETS               += build/$(CONFIG)/bin/libmprssl.a

unexport CDPATH

ifndef SHOW
.SILENT:
endif

all build compile: prep $(TARGETS)

.PHONY: prep

prep:
	@echo "      [Info] Use "make SHOW=1" to trace executed commands."
	@if [ "$(CONFIG)" = "" ] ; then echo WARNING: CONFIG not set ; exit 255 ; fi
	@if [ "$(ME_APP_PREFIX)" = "" ] ; then echo WARNING: ME_APP_PREFIX not set ; exit 255 ; fi
	@[ ! -x $(BUILD)/bin ] && mkdir -p $(BUILD)/bin; true
	@[ ! -x $(BUILD)/inc ] && mkdir -p $(BUILD)/inc; true
	@[ ! -x $(BUILD)/obj ] && mkdir -p $(BUILD)/obj; true
	@[ ! -f $(BUILD)/inc/osdep.h ] && cp src/paks/osdep/osdep.h $(BUILD)/inc/osdep.h ; true
	@if ! diff $(BUILD)/inc/osdep.h src/paks/osdep/osdep.h >/dev/null ; then\
		cp src/paks/osdep/osdep.h $(BUILD)/inc/osdep.h  ; \
	fi; true
	@[ ! -f $(BUILD)/inc/me.h ] && cp projects/http-macosx-static-me.h $(BUILD)/inc/me.h ; true
	@if ! diff $(BUILD)/inc/me.h projects/http-macosx-static-me.h >/dev/null ; then\
		cp projects/http-macosx-static-me.h $(BUILD)/inc/me.h  ; \
	fi; true
	@if [ -f "$(BUILD)/.makeflags" ] ; then \
		if [ "$(MAKEFLAGS)" != " ` cat $(BUILD)/.makeflags`" ] ; then \
			echo "   [Warning] Make flags have changed since the last build: "`cat $(BUILD)/.makeflags`"" ; \
		fi ; \
	fi
	@echo $(MAKEFLAGS) >$(BUILD)/.makeflags

clean:
	rm -f "build/$(CONFIG)/obj/actionHandler.o"
	rm -f "build/$(CONFIG)/obj/auth.o"
	rm -f "build/$(CONFIG)/obj/basic.o"
	rm -f "build/$(CONFIG)/obj/cache.o"
	rm -f "build/$(CONFIG)/obj/chunkFilter.o"
	rm -f "build/$(CONFIG)/obj/client.o"
	rm -f "build/$(CONFIG)/obj/config.o"
	rm -f "build/$(CONFIG)/obj/conn.o"
	rm -f "build/$(CONFIG)/obj/digest.o"
	rm -f "build/$(CONFIG)/obj/endpoint.o"
	rm -f "build/$(CONFIG)/obj/error.o"
	rm -f "build/$(CONFIG)/obj/estLib.o"
	rm -f "build/$(CONFIG)/obj/host.o"
	rm -f "build/$(CONFIG)/obj/http.o"
	rm -f "build/$(CONFIG)/obj/makerom.o"
	rm -f "build/$(CONFIG)/obj/monitor.o"
	rm -f "build/$(CONFIG)/obj/mprLib.o"
	rm -f "build/$(CONFIG)/obj/mprSsl.o"
	rm -f "build/$(CONFIG)/obj/netConnector.o"
	rm -f "build/$(CONFIG)/obj/packet.o"
	rm -f "build/$(CONFIG)/obj/pam.o"
	rm -f "build/$(CONFIG)/obj/passHandler.o"
	rm -f "build/$(CONFIG)/obj/pcre.o"
	rm -f "build/$(CONFIG)/obj/pipeline.o"
	rm -f "build/$(CONFIG)/obj/queue.o"
	rm -f "build/$(CONFIG)/obj/rangeFilter.o"
	rm -f "build/$(CONFIG)/obj/route.o"
	rm -f "build/$(CONFIG)/obj/rx.o"
	rm -f "build/$(CONFIG)/obj/sendConnector.o"
	rm -f "build/$(CONFIG)/obj/service.o"
	rm -f "build/$(CONFIG)/obj/session.o"
	rm -f "build/$(CONFIG)/obj/stage.o"
	rm -f "build/$(CONFIG)/obj/trace.o"
	rm -f "build/$(CONFIG)/obj/tx.o"
	rm -f "build/$(CONFIG)/obj/uploadFilter.o"
	rm -f "build/$(CONFIG)/obj/uri.o"
	rm -f "build/$(CONFIG)/obj/user.o"
	rm -f "build/$(CONFIG)/obj/var.o"
	rm -f "build/$(CONFIG)/obj/webSockFilter.o"
	rm -f "build/$(CONFIG)/bin/ca.crt"
	rm -f "build/$(CONFIG)/bin/http"
	rm -f "build/$(CONFIG)/bin/libest.a"
	rm -f "build/$(CONFIG)/bin/libhttp.a"
	rm -f "build/$(CONFIG)/bin/libmpr.a"
	rm -f "build/$(CONFIG)/bin/libmprssl.a"
	rm -f "build/$(CONFIG)/bin/libpcre.a"
	rm -f "build/$(CONFIG)/bin/makerom"

clobber: clean
	rm -fr ./$(BUILD)



#
#   http-ca-crt
#
DEPS_1 += src/paks/est/ca.crt

build/$(CONFIG)/bin/ca.crt: $(DEPS_1)
	@echo '      [Copy] build/$(CONFIG)/bin/ca.crt'
	mkdir -p "build/$(CONFIG)/bin"
	cp src/paks/est/ca.crt build/$(CONFIG)/bin/ca.crt

#
#   mpr.h
#
build/$(CONFIG)/inc/mpr.h: $(DEPS_2)
	@echo '      [Copy] build/$(CONFIG)/inc/mpr.h'
	mkdir -p "build/$(CONFIG)/inc"
	cp src/paks/mpr/mpr.h build/$(CONFIG)/inc/mpr.h

#
#   me.h
#
build/$(CONFIG)/inc/me.h: $(DEPS_3)
	@echo '      [Copy] build/$(CONFIG)/inc/me.h'

#
#   osdep.h
#
build/$(CONFIG)/inc/osdep.h: $(DEPS_4)
	@echo '      [Copy] build/$(CONFIG)/inc/osdep.h'
	mkdir -p "build/$(CONFIG)/inc"
	cp src/paks/osdep/osdep.h build/$(CONFIG)/inc/osdep.h

#
#   mprLib.o
#
DEPS_5 += build/$(CONFIG)/inc/me.h
DEPS_5 += build/$(CONFIG)/inc/mpr.h
DEPS_5 += build/$(CONFIG)/inc/osdep.h

build/$(CONFIG)/obj/mprLib.o: \
    src/paks/mpr/mprLib.c $(DEPS_5)
	@echo '   [Compile] build/$(CONFIG)/obj/mprLib.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/mprLib.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/paks/mpr/mprLib.c

#
#   libmpr
#
DEPS_6 += build/$(CONFIG)/inc/mpr.h
DEPS_6 += build/$(CONFIG)/inc/me.h
DEPS_6 += build/$(CONFIG)/inc/osdep.h
DEPS_6 += build/$(CONFIG)/obj/mprLib.o

build/$(CONFIG)/bin/libmpr.a: $(DEPS_6)
	@echo '      [Link] build/$(CONFIG)/bin/libmpr.a'
	ar -cr build/$(CONFIG)/bin/libmpr.a "build/$(CONFIG)/obj/mprLib.o"

#
#   pcre.h
#
build/$(CONFIG)/inc/pcre.h: $(DEPS_7)
	@echo '      [Copy] build/$(CONFIG)/inc/pcre.h'
	mkdir -p "build/$(CONFIG)/inc"
	cp src/paks/pcre/pcre.h build/$(CONFIG)/inc/pcre.h

#
#   pcre.o
#
DEPS_8 += build/$(CONFIG)/inc/me.h
DEPS_8 += build/$(CONFIG)/inc/pcre.h

build/$(CONFIG)/obj/pcre.o: \
    src/paks/pcre/pcre.c $(DEPS_8)
	@echo '   [Compile] build/$(CONFIG)/obj/pcre.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/pcre.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/paks/pcre/pcre.c

ifeq ($(ME_COM_PCRE),1)
#
#   libpcre
#
DEPS_9 += build/$(CONFIG)/inc/pcre.h
DEPS_9 += build/$(CONFIG)/inc/me.h
DEPS_9 += build/$(CONFIG)/obj/pcre.o

build/$(CONFIG)/bin/libpcre.a: $(DEPS_9)
	@echo '      [Link] build/$(CONFIG)/bin/libpcre.a'
	ar -cr build/$(CONFIG)/bin/libpcre.a "build/$(CONFIG)/obj/pcre.o"
endif

#
#   http.h
#
build/$(CONFIG)/inc/http.h: $(DEPS_10)
	@echo '      [Copy] build/$(CONFIG)/inc/http.h'
	mkdir -p "build/$(CONFIG)/inc"
	cp src/http.h build/$(CONFIG)/inc/http.h

#
#   actionHandler.o
#
DEPS_11 += build/$(CONFIG)/inc/me.h
DEPS_11 += build/$(CONFIG)/inc/http.h
DEPS_11 += build/$(CONFIG)/inc/mpr.h

build/$(CONFIG)/obj/actionHandler.o: \
    src/actionHandler.c $(DEPS_11)
	@echo '   [Compile] build/$(CONFIG)/obj/actionHandler.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/actionHandler.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/actionHandler.c

#
#   auth.o
#
DEPS_12 += build/$(CONFIG)/inc/me.h
DEPS_12 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/auth.o: \
    src/auth.c $(DEPS_12)
	@echo '   [Compile] build/$(CONFIG)/obj/auth.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/auth.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/auth.c

#
#   basic.o
#
DEPS_13 += build/$(CONFIG)/inc/me.h
DEPS_13 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/basic.o: \
    src/basic.c $(DEPS_13)
	@echo '   [Compile] build/$(CONFIG)/obj/basic.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/basic.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/basic.c

#
#   cache.o
#
DEPS_14 += build/$(CONFIG)/inc/me.h
DEPS_14 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/cache.o: \
    src/cache.c $(DEPS_14)
	@echo '   [Compile] build/$(CONFIG)/obj/cache.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/cache.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/cache.c

#
#   chunkFilter.o
#
DEPS_15 += build/$(CONFIG)/inc/me.h
DEPS_15 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_15)
	@echo '   [Compile] build/$(CONFIG)/obj/chunkFilter.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/chunkFilter.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/chunkFilter.c

#
#   client.o
#
DEPS_16 += build/$(CONFIG)/inc/me.h
DEPS_16 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/client.o: \
    src/client.c $(DEPS_16)
	@echo '   [Compile] build/$(CONFIG)/obj/client.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/client.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/client.c

#
#   config.o
#
DEPS_17 += build/$(CONFIG)/inc/me.h
DEPS_17 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/config.o: \
    src/config.c $(DEPS_17)
	@echo '   [Compile] build/$(CONFIG)/obj/config.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/config.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/config.c

#
#   conn.o
#
DEPS_18 += build/$(CONFIG)/inc/me.h
DEPS_18 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/conn.o: \
    src/conn.c $(DEPS_18)
	@echo '   [Compile] build/$(CONFIG)/obj/conn.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/conn.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/conn.c

#
#   digest.o
#
DEPS_19 += build/$(CONFIG)/inc/me.h
DEPS_19 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/digest.o: \
    src/digest.c $(DEPS_19)
	@echo '   [Compile] build/$(CONFIG)/obj/digest.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/digest.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/digest.c

#
#   endpoint.o
#
DEPS_20 += build/$(CONFIG)/inc/me.h
DEPS_20 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_20)
	@echo '   [Compile] build/$(CONFIG)/obj/endpoint.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/endpoint.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/endpoint.c

#
#   error.o
#
DEPS_21 += build/$(CONFIG)/inc/me.h
DEPS_21 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/error.o: \
    src/error.c $(DEPS_21)
	@echo '   [Compile] build/$(CONFIG)/obj/error.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/error.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/error.c

#
#   host.o
#
DEPS_22 += build/$(CONFIG)/inc/me.h
DEPS_22 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/host.o: \
    src/host.c $(DEPS_22)
	@echo '   [Compile] build/$(CONFIG)/obj/host.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/host.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/host.c

#
#   monitor.o
#
DEPS_23 += build/$(CONFIG)/inc/me.h
DEPS_23 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/monitor.o: \
    src/monitor.c $(DEPS_23)
	@echo '   [Compile] build/$(CONFIG)/obj/monitor.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/monitor.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/monitor.c

#
#   netConnector.o
#
DEPS_24 += build/$(CONFIG)/inc/me.h
DEPS_24 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_24)
	@echo '   [Compile] build/$(CONFIG)/obj/netConnector.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/netConnector.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/netConnector.c

#
#   packet.o
#
DEPS_25 += build/$(CONFIG)/inc/me.h
DEPS_25 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/packet.o: \
    src/packet.c $(DEPS_25)
	@echo '   [Compile] build/$(CONFIG)/obj/packet.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/packet.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/packet.c

#
#   pam.o
#
DEPS_26 += build/$(CONFIG)/inc/me.h
DEPS_26 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/pam.o: \
    src/pam.c $(DEPS_26)
	@echo '   [Compile] build/$(CONFIG)/obj/pam.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/pam.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/pam.c

#
#   passHandler.o
#
DEPS_27 += build/$(CONFIG)/inc/me.h
DEPS_27 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_27)
	@echo '   [Compile] build/$(CONFIG)/obj/passHandler.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/passHandler.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/passHandler.c

#
#   pipeline.o
#
DEPS_28 += build/$(CONFIG)/inc/me.h
DEPS_28 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_28)
	@echo '   [Compile] build/$(CONFIG)/obj/pipeline.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/pipeline.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/pipeline.c

#
#   queue.o
#
DEPS_29 += build/$(CONFIG)/inc/me.h
DEPS_29 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/queue.o: \
    src/queue.c $(DEPS_29)
	@echo '   [Compile] build/$(CONFIG)/obj/queue.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/queue.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/queue.c

#
#   rangeFilter.o
#
DEPS_30 += build/$(CONFIG)/inc/me.h
DEPS_30 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_30)
	@echo '   [Compile] build/$(CONFIG)/obj/rangeFilter.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/rangeFilter.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/rangeFilter.c

#
#   route.o
#
DEPS_31 += build/$(CONFIG)/inc/me.h
DEPS_31 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/route.o: \
    src/route.c $(DEPS_31)
	@echo '   [Compile] build/$(CONFIG)/obj/route.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/route.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/route.c

#
#   rx.o
#
DEPS_32 += build/$(CONFIG)/inc/me.h
DEPS_32 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/rx.o: \
    src/rx.c $(DEPS_32)
	@echo '   [Compile] build/$(CONFIG)/obj/rx.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/rx.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/rx.c

#
#   sendConnector.o
#
DEPS_33 += build/$(CONFIG)/inc/me.h
DEPS_33 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/sendConnector.o: \
    src/sendConnector.c $(DEPS_33)
	@echo '   [Compile] build/$(CONFIG)/obj/sendConnector.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/sendConnector.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/sendConnector.c

#
#   service.o
#
DEPS_34 += build/$(CONFIG)/inc/me.h
DEPS_34 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/service.o: \
    src/service.c $(DEPS_34)
	@echo '   [Compile] build/$(CONFIG)/obj/service.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/service.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/service.c

#
#   session.o
#
DEPS_35 += build/$(CONFIG)/inc/me.h
DEPS_35 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/session.o: \
    src/session.c $(DEPS_35)
	@echo '   [Compile] build/$(CONFIG)/obj/session.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/session.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/session.c

#
#   stage.o
#
DEPS_36 += build/$(CONFIG)/inc/me.h
DEPS_36 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/stage.o: \
    src/stage.c $(DEPS_36)
	@echo '   [Compile] build/$(CONFIG)/obj/stage.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/stage.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/stage.c

#
#   trace.o
#
DEPS_37 += build/$(CONFIG)/inc/me.h
DEPS_37 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/trace.o: \
    src/trace.c $(DEPS_37)
	@echo '   [Compile] build/$(CONFIG)/obj/trace.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/trace.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/trace.c

#
#   tx.o
#
DEPS_38 += build/$(CONFIG)/inc/me.h
DEPS_38 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/tx.o: \
    src/tx.c $(DEPS_38)
	@echo '   [Compile] build/$(CONFIG)/obj/tx.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/tx.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/tx.c

#
#   uploadFilter.o
#
DEPS_39 += build/$(CONFIG)/inc/me.h
DEPS_39 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_39)
	@echo '   [Compile] build/$(CONFIG)/obj/uploadFilter.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/uploadFilter.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/uploadFilter.c

#
#   uri.o
#
DEPS_40 += build/$(CONFIG)/inc/me.h
DEPS_40 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/uri.o: \
    src/uri.c $(DEPS_40)
	@echo '   [Compile] build/$(CONFIG)/obj/uri.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/uri.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/uri.c

#
#   user.o
#
DEPS_41 += build/$(CONFIG)/inc/me.h
DEPS_41 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/user.o: \
    src/user.c $(DEPS_41)
	@echo '   [Compile] build/$(CONFIG)/obj/user.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/user.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/user.c

#
#   var.o
#
DEPS_42 += build/$(CONFIG)/inc/me.h
DEPS_42 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/var.o: \
    src/var.c $(DEPS_42)
	@echo '   [Compile] build/$(CONFIG)/obj/var.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/var.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/var.c

#
#   webSockFilter.o
#
DEPS_43 += build/$(CONFIG)/inc/me.h
DEPS_43 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/webSockFilter.o: \
    src/webSockFilter.c $(DEPS_43)
	@echo '   [Compile] build/$(CONFIG)/obj/webSockFilter.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/webSockFilter.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/webSockFilter.c

#
#   libhttp
#
DEPS_44 += build/$(CONFIG)/inc/mpr.h
DEPS_44 += build/$(CONFIG)/inc/me.h
DEPS_44 += build/$(CONFIG)/inc/osdep.h
DEPS_44 += build/$(CONFIG)/obj/mprLib.o
DEPS_44 += build/$(CONFIG)/bin/libmpr.a
DEPS_44 += build/$(CONFIG)/inc/pcre.h
DEPS_44 += build/$(CONFIG)/obj/pcre.o
ifeq ($(ME_COM_PCRE),1)
    DEPS_44 += build/$(CONFIG)/bin/libpcre.a
endif
DEPS_44 += build/$(CONFIG)/inc/http.h
DEPS_44 += build/$(CONFIG)/obj/actionHandler.o
DEPS_44 += build/$(CONFIG)/obj/auth.o
DEPS_44 += build/$(CONFIG)/obj/basic.o
DEPS_44 += build/$(CONFIG)/obj/cache.o
DEPS_44 += build/$(CONFIG)/obj/chunkFilter.o
DEPS_44 += build/$(CONFIG)/obj/client.o
DEPS_44 += build/$(CONFIG)/obj/config.o
DEPS_44 += build/$(CONFIG)/obj/conn.o
DEPS_44 += build/$(CONFIG)/obj/digest.o
DEPS_44 += build/$(CONFIG)/obj/endpoint.o
DEPS_44 += build/$(CONFIG)/obj/error.o
DEPS_44 += build/$(CONFIG)/obj/host.o
DEPS_44 += build/$(CONFIG)/obj/monitor.o
DEPS_44 += build/$(CONFIG)/obj/netConnector.o
DEPS_44 += build/$(CONFIG)/obj/packet.o
DEPS_44 += build/$(CONFIG)/obj/pam.o
DEPS_44 += build/$(CONFIG)/obj/passHandler.o
DEPS_44 += build/$(CONFIG)/obj/pipeline.o
DEPS_44 += build/$(CONFIG)/obj/queue.o
DEPS_44 += build/$(CONFIG)/obj/rangeFilter.o
DEPS_44 += build/$(CONFIG)/obj/route.o
DEPS_44 += build/$(CONFIG)/obj/rx.o
DEPS_44 += build/$(CONFIG)/obj/sendConnector.o
DEPS_44 += build/$(CONFIG)/obj/service.o
DEPS_44 += build/$(CONFIG)/obj/session.o
DEPS_44 += build/$(CONFIG)/obj/stage.o
DEPS_44 += build/$(CONFIG)/obj/trace.o
DEPS_44 += build/$(CONFIG)/obj/tx.o
DEPS_44 += build/$(CONFIG)/obj/uploadFilter.o
DEPS_44 += build/$(CONFIG)/obj/uri.o
DEPS_44 += build/$(CONFIG)/obj/user.o
DEPS_44 += build/$(CONFIG)/obj/var.o
DEPS_44 += build/$(CONFIG)/obj/webSockFilter.o

build/$(CONFIG)/bin/libhttp.a: $(DEPS_44)
	@echo '      [Link] build/$(CONFIG)/bin/libhttp.a'
	ar -cr build/$(CONFIG)/bin/libhttp.a "build/$(CONFIG)/obj/actionHandler.o" "build/$(CONFIG)/obj/auth.o" "build/$(CONFIG)/obj/basic.o" "build/$(CONFIG)/obj/cache.o" "build/$(CONFIG)/obj/chunkFilter.o" "build/$(CONFIG)/obj/client.o" "build/$(CONFIG)/obj/config.o" "build/$(CONFIG)/obj/conn.o" "build/$(CONFIG)/obj/digest.o" "build/$(CONFIG)/obj/endpoint.o" "build/$(CONFIG)/obj/error.o" "build/$(CONFIG)/obj/host.o" "build/$(CONFIG)/obj/monitor.o" "build/$(CONFIG)/obj/netConnector.o" "build/$(CONFIG)/obj/packet.o" "build/$(CONFIG)/obj/pam.o" "build/$(CONFIG)/obj/passHandler.o" "build/$(CONFIG)/obj/pipeline.o" "build/$(CONFIG)/obj/queue.o" "build/$(CONFIG)/obj/rangeFilter.o" "build/$(CONFIG)/obj/route.o" "build/$(CONFIG)/obj/rx.o" "build/$(CONFIG)/obj/sendConnector.o" "build/$(CONFIG)/obj/service.o" "build/$(CONFIG)/obj/session.o" "build/$(CONFIG)/obj/stage.o" "build/$(CONFIG)/obj/trace.o" "build/$(CONFIG)/obj/tx.o" "build/$(CONFIG)/obj/uploadFilter.o" "build/$(CONFIG)/obj/uri.o" "build/$(CONFIG)/obj/user.o" "build/$(CONFIG)/obj/var.o" "build/$(CONFIG)/obj/webSockFilter.o"

#
#   http.o
#
DEPS_45 += build/$(CONFIG)/inc/me.h
DEPS_45 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/http.o: \
    src/http.c $(DEPS_45)
	@echo '   [Compile] build/$(CONFIG)/obj/http.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/http.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/http.c

#
#   httpcmd
#
DEPS_46 += build/$(CONFIG)/inc/mpr.h
DEPS_46 += build/$(CONFIG)/inc/me.h
DEPS_46 += build/$(CONFIG)/inc/osdep.h
DEPS_46 += build/$(CONFIG)/obj/mprLib.o
DEPS_46 += build/$(CONFIG)/bin/libmpr.a
DEPS_46 += build/$(CONFIG)/inc/pcre.h
DEPS_46 += build/$(CONFIG)/obj/pcre.o
ifeq ($(ME_COM_PCRE),1)
    DEPS_46 += build/$(CONFIG)/bin/libpcre.a
endif
DEPS_46 += build/$(CONFIG)/inc/http.h
DEPS_46 += build/$(CONFIG)/obj/actionHandler.o
DEPS_46 += build/$(CONFIG)/obj/auth.o
DEPS_46 += build/$(CONFIG)/obj/basic.o
DEPS_46 += build/$(CONFIG)/obj/cache.o
DEPS_46 += build/$(CONFIG)/obj/chunkFilter.o
DEPS_46 += build/$(CONFIG)/obj/client.o
DEPS_46 += build/$(CONFIG)/obj/config.o
DEPS_46 += build/$(CONFIG)/obj/conn.o
DEPS_46 += build/$(CONFIG)/obj/digest.o
DEPS_46 += build/$(CONFIG)/obj/endpoint.o
DEPS_46 += build/$(CONFIG)/obj/error.o
DEPS_46 += build/$(CONFIG)/obj/host.o
DEPS_46 += build/$(CONFIG)/obj/monitor.o
DEPS_46 += build/$(CONFIG)/obj/netConnector.o
DEPS_46 += build/$(CONFIG)/obj/packet.o
DEPS_46 += build/$(CONFIG)/obj/pam.o
DEPS_46 += build/$(CONFIG)/obj/passHandler.o
DEPS_46 += build/$(CONFIG)/obj/pipeline.o
DEPS_46 += build/$(CONFIG)/obj/queue.o
DEPS_46 += build/$(CONFIG)/obj/rangeFilter.o
DEPS_46 += build/$(CONFIG)/obj/route.o
DEPS_46 += build/$(CONFIG)/obj/rx.o
DEPS_46 += build/$(CONFIG)/obj/sendConnector.o
DEPS_46 += build/$(CONFIG)/obj/service.o
DEPS_46 += build/$(CONFIG)/obj/session.o
DEPS_46 += build/$(CONFIG)/obj/stage.o
DEPS_46 += build/$(CONFIG)/obj/trace.o
DEPS_46 += build/$(CONFIG)/obj/tx.o
DEPS_46 += build/$(CONFIG)/obj/uploadFilter.o
DEPS_46 += build/$(CONFIG)/obj/uri.o
DEPS_46 += build/$(CONFIG)/obj/user.o
DEPS_46 += build/$(CONFIG)/obj/var.o
DEPS_46 += build/$(CONFIG)/obj/webSockFilter.o
DEPS_46 += build/$(CONFIG)/bin/libhttp.a
DEPS_46 += build/$(CONFIG)/obj/http.o

LIBS_46 += -lhttp
LIBS_46 += -lmpr
ifeq ($(ME_COM_PCRE),1)
    LIBS_46 += -lpcre
endif

build/$(CONFIG)/bin/http: $(DEPS_46)
	@echo '      [Link] build/$(CONFIG)/bin/http'
	$(CC) -o build/$(CONFIG)/bin/http -arch $(CC_ARCH) $(LDFLAGS) $(LIBPATHS) "build/$(CONFIG)/obj/http.o" $(LIBPATHS_46) $(LIBS_46) $(LIBS_46) $(LIBS) 

#
#   est.h
#
build/$(CONFIG)/inc/est.h: $(DEPS_47)
	@echo '      [Copy] build/$(CONFIG)/inc/est.h'
	mkdir -p "build/$(CONFIG)/inc"
	cp src/paks/est/est.h build/$(CONFIG)/inc/est.h

#
#   estLib.o
#
DEPS_48 += build/$(CONFIG)/inc/me.h
DEPS_48 += build/$(CONFIG)/inc/est.h
DEPS_48 += build/$(CONFIG)/inc/osdep.h

build/$(CONFIG)/obj/estLib.o: \
    src/paks/est/estLib.c $(DEPS_48)
	@echo '   [Compile] build/$(CONFIG)/obj/estLib.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/estLib.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) src/paks/est/estLib.c

ifeq ($(ME_COM_EST),1)
#
#   libest
#
DEPS_49 += build/$(CONFIG)/inc/est.h
DEPS_49 += build/$(CONFIG)/inc/me.h
DEPS_49 += build/$(CONFIG)/inc/osdep.h
DEPS_49 += build/$(CONFIG)/obj/estLib.o

build/$(CONFIG)/bin/libest.a: $(DEPS_49)
	@echo '      [Link] build/$(CONFIG)/bin/libest.a'
	ar -cr build/$(CONFIG)/bin/libest.a "build/$(CONFIG)/obj/estLib.o"
endif

#
#   mprSsl.o
#
DEPS_50 += build/$(CONFIG)/inc/me.h
DEPS_50 += build/$(CONFIG)/inc/mpr.h
DEPS_50 += build/$(CONFIG)/inc/est.h

build/$(CONFIG)/obj/mprSsl.o: \
    src/paks/mpr/mprSsl.c $(DEPS_50)
	@echo '   [Compile] build/$(CONFIG)/obj/mprSsl.o'
	$(CC) -c $(DFLAGS) -o build/$(CONFIG)/obj/mprSsl.o -arch $(CC_ARCH) $(CFLAGS) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" "-I$(ME_COM_MATRIXSSL_PATH)" "-I$(ME_COM_MATRIXSSL_PATH)/matrixssl" "-I$(ME_COM_NANOSSL_PATH)/src" src/paks/mpr/mprSsl.c

#
#   libmprssl
#
DEPS_51 += build/$(CONFIG)/inc/mpr.h
DEPS_51 += build/$(CONFIG)/inc/me.h
DEPS_51 += build/$(CONFIG)/inc/osdep.h
DEPS_51 += build/$(CONFIG)/obj/mprLib.o
DEPS_51 += build/$(CONFIG)/bin/libmpr.a
DEPS_51 += build/$(CONFIG)/inc/est.h
DEPS_51 += build/$(CONFIG)/obj/estLib.o
ifeq ($(ME_COM_EST),1)
    DEPS_51 += build/$(CONFIG)/bin/libest.a
endif
DEPS_51 += build/$(CONFIG)/obj/mprSsl.o

build/$(CONFIG)/bin/libmprssl.a: $(DEPS_51)
	@echo '      [Link] build/$(CONFIG)/bin/libmprssl.a'
	ar -cr build/$(CONFIG)/bin/libmprssl.a "build/$(CONFIG)/obj/mprSsl.o"

#
#   stop
#
stop: $(DEPS_52)

#
#   installBinary
#
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

#
#   version
#
version: $(DEPS_57)
	echo 5.0.0

