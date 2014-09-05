#
#   http-freebsd-default.mk -- Makefile to build Embedthis Http for freebsd
#

NAME                  := http
VERSION               := 5.2.0
PROFILE               ?= default
ARCH                  ?= $(shell uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/')
CC_ARCH               ?= $(shell echo $(ARCH) | sed 's/x86/i686/;s/x64/x86_64/')
OS                    ?= freebsd
CC                    ?= gcc
CONFIG                ?= $(OS)-$(ARCH)-$(PROFILE)
BUILD                 ?= build/$(CONFIG)
LBIN                  ?= $(BUILD)/bin
PATH                  := $(LBIN):$(PATH)

ME_COM_EST            ?= 1
ME_COM_OPENSSL        ?= 0
ME_COM_OSDEP          ?= 1
ME_COM_PCRE           ?= 1
ME_COM_SSL            ?= 1
ME_COM_VXWORKS        ?= 0
ME_COM_WINSDK         ?= 1

ifeq ($(ME_COM_EST),1)
    ME_COM_SSL := 1
endif
ifeq ($(ME_COM_OPENSSL),1)
    ME_COM_SSL := 1
endif

ME_COM_COMPILER_PATH  ?= gcc
ME_COM_LIB_PATH       ?= ar
ME_COM_OPENSSL_PATH   ?= /usr/src/openssl

CFLAGS                += -fPIC -w
DFLAGS                += -D_REENTRANT -DPIC $(patsubst %,-D%,$(filter ME_%,$(MAKEFLAGS))) -DME_COM_EST=$(ME_COM_EST) -DME_COM_OPENSSL=$(ME_COM_OPENSSL) -DME_COM_OSDEP=$(ME_COM_OSDEP) -DME_COM_PCRE=$(ME_COM_PCRE) -DME_COM_SSL=$(ME_COM_SSL) -DME_COM_VXWORKS=$(ME_COM_VXWORKS) -DME_COM_WINSDK=$(ME_COM_WINSDK) 
IFLAGS                += "-Ibuild/$(CONFIG)/inc"
LDFLAGS               += 
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
TARGETS               += build/$(CONFIG)/bin/http-server
TARGETS               += build/$(CONFIG)/bin/http
ifeq ($(ME_COM_EST),1)
    TARGETS           += build/$(CONFIG)/bin/libest.so
endif
TARGETS               += build/$(CONFIG)/bin/libmprssl.so

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
	@[ ! -f $(BUILD)/inc/me.h ] && cp projects/http-freebsd-default-me.h $(BUILD)/inc/me.h ; true
	@if ! diff $(BUILD)/inc/me.h projects/http-freebsd-default-me.h >/dev/null ; then\
		cp projects/http-freebsd-default-me.h $(BUILD)/inc/me.h  ; \
	fi; true
	@if [ -f "$(BUILD)/.makeflags" ] ; then \
		if [ "$(MAKEFLAGS)" != "`cat $(BUILD)/.makeflags`" ] ; then \
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
	rm -f "build/$(CONFIG)/obj/dirHandler.o"
	rm -f "build/$(CONFIG)/obj/endpoint.o"
	rm -f "build/$(CONFIG)/obj/error.o"
	rm -f "build/$(CONFIG)/obj/estLib.o"
	rm -f "build/$(CONFIG)/obj/fileHandler.o"
	rm -f "build/$(CONFIG)/obj/host.o"
	rm -f "build/$(CONFIG)/obj/http-server.o"
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
	rm -f "build/$(CONFIG)/bin/http-server"
	rm -f "build/$(CONFIG)/bin/http"
	rm -f "build/$(CONFIG)/bin/libest.so"
	rm -f "build/$(CONFIG)/bin/libhttp.so"
	rm -f "build/$(CONFIG)/bin/libmpr.so"
	rm -f "build/$(CONFIG)/bin/libmprssl.so"
	rm -f "build/$(CONFIG)/bin/libpcre.so"
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
DEPS_2 += src/paks/mpr/mpr.h

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
DEPS_4 += src/paks/osdep/osdep.h

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
	$(CC) -c -o build/$(CONFIG)/obj/mprLib.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/paks/mpr/mprLib.c

#
#   libmpr
#
DEPS_6 += build/$(CONFIG)/inc/mpr.h
DEPS_6 += build/$(CONFIG)/inc/me.h
DEPS_6 += build/$(CONFIG)/inc/osdep.h
DEPS_6 += build/$(CONFIG)/obj/mprLib.o

build/$(CONFIG)/bin/libmpr.so: $(DEPS_6)
	@echo '      [Link] build/$(CONFIG)/bin/libmpr.so'
	$(CC) -shared -o build/$(CONFIG)/bin/libmpr.so $(LDFLAGS) $(LIBPATHS) "build/$(CONFIG)/obj/mprLib.o" $(LIBS) 

#
#   pcre.h
#
DEPS_7 += src/paks/pcre/pcre.h

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
	$(CC) -c -o build/$(CONFIG)/obj/pcre.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/paks/pcre/pcre.c

ifeq ($(ME_COM_PCRE),1)
#
#   libpcre
#
DEPS_9 += build/$(CONFIG)/inc/pcre.h
DEPS_9 += build/$(CONFIG)/inc/me.h
DEPS_9 += build/$(CONFIG)/obj/pcre.o

build/$(CONFIG)/bin/libpcre.so: $(DEPS_9)
	@echo '      [Link] build/$(CONFIG)/bin/libpcre.so'
	$(CC) -shared -o build/$(CONFIG)/bin/libpcre.so $(LDFLAGS) $(LIBPATHS) "build/$(CONFIG)/obj/pcre.o" $(LIBS) 
endif

#
#   http.h
#
DEPS_10 += src/http.h

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
	$(CC) -c -o build/$(CONFIG)/obj/actionHandler.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/actionHandler.c

#
#   auth.o
#
DEPS_12 += build/$(CONFIG)/inc/me.h
DEPS_12 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/auth.o: \
    src/auth.c $(DEPS_12)
	@echo '   [Compile] build/$(CONFIG)/obj/auth.o'
	$(CC) -c -o build/$(CONFIG)/obj/auth.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/auth.c

#
#   basic.o
#
DEPS_13 += build/$(CONFIG)/inc/me.h
DEPS_13 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/basic.o: \
    src/basic.c $(DEPS_13)
	@echo '   [Compile] build/$(CONFIG)/obj/basic.o'
	$(CC) -c -o build/$(CONFIG)/obj/basic.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/basic.c

#
#   cache.o
#
DEPS_14 += build/$(CONFIG)/inc/me.h
DEPS_14 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/cache.o: \
    src/cache.c $(DEPS_14)
	@echo '   [Compile] build/$(CONFIG)/obj/cache.o'
	$(CC) -c -o build/$(CONFIG)/obj/cache.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/cache.c

#
#   chunkFilter.o
#
DEPS_15 += build/$(CONFIG)/inc/me.h
DEPS_15 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_15)
	@echo '   [Compile] build/$(CONFIG)/obj/chunkFilter.o'
	$(CC) -c -o build/$(CONFIG)/obj/chunkFilter.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/chunkFilter.c

#
#   client.o
#
DEPS_16 += build/$(CONFIG)/inc/me.h
DEPS_16 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/client.o: \
    src/client.c $(DEPS_16)
	@echo '   [Compile] build/$(CONFIG)/obj/client.o'
	$(CC) -c -o build/$(CONFIG)/obj/client.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/client.c

#
#   config.o
#
DEPS_17 += build/$(CONFIG)/inc/me.h
DEPS_17 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/config.o: \
    src/config.c $(DEPS_17)
	@echo '   [Compile] build/$(CONFIG)/obj/config.o'
	$(CC) -c -o build/$(CONFIG)/obj/config.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/config.c

#
#   conn.o
#
DEPS_18 += build/$(CONFIG)/inc/me.h
DEPS_18 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/conn.o: \
    src/conn.c $(DEPS_18)
	@echo '   [Compile] build/$(CONFIG)/obj/conn.o'
	$(CC) -c -o build/$(CONFIG)/obj/conn.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/conn.c

#
#   digest.o
#
DEPS_19 += build/$(CONFIG)/inc/me.h
DEPS_19 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/digest.o: \
    src/digest.c $(DEPS_19)
	@echo '   [Compile] build/$(CONFIG)/obj/digest.o'
	$(CC) -c -o build/$(CONFIG)/obj/digest.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/digest.c

#
#   dirHandler.o
#
DEPS_20 += build/$(CONFIG)/inc/me.h
DEPS_20 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/dirHandler.o: \
    src/dirHandler.c $(DEPS_20)
	@echo '   [Compile] build/$(CONFIG)/obj/dirHandler.o'
	$(CC) -c -o build/$(CONFIG)/obj/dirHandler.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/dirHandler.c

#
#   endpoint.o
#
DEPS_21 += build/$(CONFIG)/inc/me.h
DEPS_21 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_21)
	@echo '   [Compile] build/$(CONFIG)/obj/endpoint.o'
	$(CC) -c -o build/$(CONFIG)/obj/endpoint.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/endpoint.c

#
#   error.o
#
DEPS_22 += build/$(CONFIG)/inc/me.h
DEPS_22 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/error.o: \
    src/error.c $(DEPS_22)
	@echo '   [Compile] build/$(CONFIG)/obj/error.o'
	$(CC) -c -o build/$(CONFIG)/obj/error.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/error.c

#
#   fileHandler.o
#
DEPS_23 += build/$(CONFIG)/inc/me.h
DEPS_23 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/fileHandler.o: \
    src/fileHandler.c $(DEPS_23)
	@echo '   [Compile] build/$(CONFIG)/obj/fileHandler.o'
	$(CC) -c -o build/$(CONFIG)/obj/fileHandler.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/fileHandler.c

#
#   host.o
#
DEPS_24 += build/$(CONFIG)/inc/me.h
DEPS_24 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/host.o: \
    src/host.c $(DEPS_24)
	@echo '   [Compile] build/$(CONFIG)/obj/host.o'
	$(CC) -c -o build/$(CONFIG)/obj/host.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/host.c

#
#   monitor.o
#
DEPS_25 += build/$(CONFIG)/inc/me.h
DEPS_25 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/monitor.o: \
    src/monitor.c $(DEPS_25)
	@echo '   [Compile] build/$(CONFIG)/obj/monitor.o'
	$(CC) -c -o build/$(CONFIG)/obj/monitor.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/monitor.c

#
#   netConnector.o
#
DEPS_26 += build/$(CONFIG)/inc/me.h
DEPS_26 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_26)
	@echo '   [Compile] build/$(CONFIG)/obj/netConnector.o'
	$(CC) -c -o build/$(CONFIG)/obj/netConnector.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/netConnector.c

#
#   packet.o
#
DEPS_27 += build/$(CONFIG)/inc/me.h
DEPS_27 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/packet.o: \
    src/packet.c $(DEPS_27)
	@echo '   [Compile] build/$(CONFIG)/obj/packet.o'
	$(CC) -c -o build/$(CONFIG)/obj/packet.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/packet.c

#
#   pam.o
#
DEPS_28 += build/$(CONFIG)/inc/me.h
DEPS_28 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/pam.o: \
    src/pam.c $(DEPS_28)
	@echo '   [Compile] build/$(CONFIG)/obj/pam.o'
	$(CC) -c -o build/$(CONFIG)/obj/pam.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/pam.c

#
#   passHandler.o
#
DEPS_29 += build/$(CONFIG)/inc/me.h
DEPS_29 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_29)
	@echo '   [Compile] build/$(CONFIG)/obj/passHandler.o'
	$(CC) -c -o build/$(CONFIG)/obj/passHandler.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/passHandler.c

#
#   pipeline.o
#
DEPS_30 += build/$(CONFIG)/inc/me.h
DEPS_30 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_30)
	@echo '   [Compile] build/$(CONFIG)/obj/pipeline.o'
	$(CC) -c -o build/$(CONFIG)/obj/pipeline.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/pipeline.c

#
#   queue.o
#
DEPS_31 += build/$(CONFIG)/inc/me.h
DEPS_31 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/queue.o: \
    src/queue.c $(DEPS_31)
	@echo '   [Compile] build/$(CONFIG)/obj/queue.o'
	$(CC) -c -o build/$(CONFIG)/obj/queue.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/queue.c

#
#   rangeFilter.o
#
DEPS_32 += build/$(CONFIG)/inc/me.h
DEPS_32 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_32)
	@echo '   [Compile] build/$(CONFIG)/obj/rangeFilter.o'
	$(CC) -c -o build/$(CONFIG)/obj/rangeFilter.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/rangeFilter.c

#
#   route.o
#
DEPS_33 += build/$(CONFIG)/inc/me.h
DEPS_33 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/route.o: \
    src/route.c $(DEPS_33)
	@echo '   [Compile] build/$(CONFIG)/obj/route.o'
	$(CC) -c -o build/$(CONFIG)/obj/route.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/route.c

#
#   rx.o
#
DEPS_34 += build/$(CONFIG)/inc/me.h
DEPS_34 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/rx.o: \
    src/rx.c $(DEPS_34)
	@echo '   [Compile] build/$(CONFIG)/obj/rx.o'
	$(CC) -c -o build/$(CONFIG)/obj/rx.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/rx.c

#
#   sendConnector.o
#
DEPS_35 += build/$(CONFIG)/inc/me.h
DEPS_35 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/sendConnector.o: \
    src/sendConnector.c $(DEPS_35)
	@echo '   [Compile] build/$(CONFIG)/obj/sendConnector.o'
	$(CC) -c -o build/$(CONFIG)/obj/sendConnector.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/sendConnector.c

#
#   service.o
#
DEPS_36 += build/$(CONFIG)/inc/me.h
DEPS_36 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/service.o: \
    src/service.c $(DEPS_36)
	@echo '   [Compile] build/$(CONFIG)/obj/service.o'
	$(CC) -c -o build/$(CONFIG)/obj/service.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/service.c

#
#   session.o
#
DEPS_37 += build/$(CONFIG)/inc/me.h
DEPS_37 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/session.o: \
    src/session.c $(DEPS_37)
	@echo '   [Compile] build/$(CONFIG)/obj/session.o'
	$(CC) -c -o build/$(CONFIG)/obj/session.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/session.c

#
#   stage.o
#
DEPS_38 += build/$(CONFIG)/inc/me.h
DEPS_38 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/stage.o: \
    src/stage.c $(DEPS_38)
	@echo '   [Compile] build/$(CONFIG)/obj/stage.o'
	$(CC) -c -o build/$(CONFIG)/obj/stage.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/stage.c

#
#   trace.o
#
DEPS_39 += build/$(CONFIG)/inc/me.h
DEPS_39 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/trace.o: \
    src/trace.c $(DEPS_39)
	@echo '   [Compile] build/$(CONFIG)/obj/trace.o'
	$(CC) -c -o build/$(CONFIG)/obj/trace.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/trace.c

#
#   tx.o
#
DEPS_40 += build/$(CONFIG)/inc/me.h
DEPS_40 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/tx.o: \
    src/tx.c $(DEPS_40)
	@echo '   [Compile] build/$(CONFIG)/obj/tx.o'
	$(CC) -c -o build/$(CONFIG)/obj/tx.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/tx.c

#
#   uploadFilter.o
#
DEPS_41 += build/$(CONFIG)/inc/me.h
DEPS_41 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_41)
	@echo '   [Compile] build/$(CONFIG)/obj/uploadFilter.o'
	$(CC) -c -o build/$(CONFIG)/obj/uploadFilter.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/uploadFilter.c

#
#   uri.o
#
DEPS_42 += build/$(CONFIG)/inc/me.h
DEPS_42 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/uri.o: \
    src/uri.c $(DEPS_42)
	@echo '   [Compile] build/$(CONFIG)/obj/uri.o'
	$(CC) -c -o build/$(CONFIG)/obj/uri.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/uri.c

#
#   user.o
#
DEPS_43 += build/$(CONFIG)/inc/me.h
DEPS_43 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/user.o: \
    src/user.c $(DEPS_43)
	@echo '   [Compile] build/$(CONFIG)/obj/user.o'
	$(CC) -c -o build/$(CONFIG)/obj/user.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/user.c

#
#   var.o
#
DEPS_44 += build/$(CONFIG)/inc/me.h
DEPS_44 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/var.o: \
    src/var.c $(DEPS_44)
	@echo '   [Compile] build/$(CONFIG)/obj/var.o'
	$(CC) -c -o build/$(CONFIG)/obj/var.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/var.c

#
#   webSockFilter.o
#
DEPS_45 += build/$(CONFIG)/inc/me.h
DEPS_45 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/webSockFilter.o: \
    src/webSockFilter.c $(DEPS_45)
	@echo '   [Compile] build/$(CONFIG)/obj/webSockFilter.o'
	$(CC) -c -o build/$(CONFIG)/obj/webSockFilter.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/webSockFilter.c

#
#   libhttp
#
DEPS_46 += build/$(CONFIG)/inc/mpr.h
DEPS_46 += build/$(CONFIG)/inc/me.h
DEPS_46 += build/$(CONFIG)/inc/osdep.h
DEPS_46 += build/$(CONFIG)/obj/mprLib.o
DEPS_46 += build/$(CONFIG)/bin/libmpr.so
DEPS_46 += build/$(CONFIG)/inc/pcre.h
DEPS_46 += build/$(CONFIG)/obj/pcre.o
ifeq ($(ME_COM_PCRE),1)
    DEPS_46 += build/$(CONFIG)/bin/libpcre.so
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
DEPS_46 += build/$(CONFIG)/obj/dirHandler.o
DEPS_46 += build/$(CONFIG)/obj/endpoint.o
DEPS_46 += build/$(CONFIG)/obj/error.o
DEPS_46 += build/$(CONFIG)/obj/fileHandler.o
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

LIBS_46 += -lmpr
ifeq ($(ME_COM_PCRE),1)
    LIBS_46 += -lpcre
endif

build/$(CONFIG)/bin/libhttp.so: $(DEPS_46)
	@echo '      [Link] build/$(CONFIG)/bin/libhttp.so'
	$(CC) -shared -o build/$(CONFIG)/bin/libhttp.so $(LDFLAGS) $(LIBPATHS) "build/$(CONFIG)/obj/actionHandler.o" "build/$(CONFIG)/obj/auth.o" "build/$(CONFIG)/obj/basic.o" "build/$(CONFIG)/obj/cache.o" "build/$(CONFIG)/obj/chunkFilter.o" "build/$(CONFIG)/obj/client.o" "build/$(CONFIG)/obj/config.o" "build/$(CONFIG)/obj/conn.o" "build/$(CONFIG)/obj/digest.o" "build/$(CONFIG)/obj/dirHandler.o" "build/$(CONFIG)/obj/endpoint.o" "build/$(CONFIG)/obj/error.o" "build/$(CONFIG)/obj/fileHandler.o" "build/$(CONFIG)/obj/host.o" "build/$(CONFIG)/obj/monitor.o" "build/$(CONFIG)/obj/netConnector.o" "build/$(CONFIG)/obj/packet.o" "build/$(CONFIG)/obj/pam.o" "build/$(CONFIG)/obj/passHandler.o" "build/$(CONFIG)/obj/pipeline.o" "build/$(CONFIG)/obj/queue.o" "build/$(CONFIG)/obj/rangeFilter.o" "build/$(CONFIG)/obj/route.o" "build/$(CONFIG)/obj/rx.o" "build/$(CONFIG)/obj/sendConnector.o" "build/$(CONFIG)/obj/service.o" "build/$(CONFIG)/obj/session.o" "build/$(CONFIG)/obj/stage.o" "build/$(CONFIG)/obj/trace.o" "build/$(CONFIG)/obj/tx.o" "build/$(CONFIG)/obj/uploadFilter.o" "build/$(CONFIG)/obj/uri.o" "build/$(CONFIG)/obj/user.o" "build/$(CONFIG)/obj/var.o" "build/$(CONFIG)/obj/webSockFilter.o" $(LIBPATHS_46) $(LIBS_46) $(LIBS_46) $(LIBS) 

#
#   http-server.o
#
DEPS_47 += build/$(CONFIG)/inc/me.h
DEPS_47 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/http-server.o: \
    test/http-server.c $(DEPS_47)
	@echo '   [Compile] build/$(CONFIG)/obj/http-server.o'
	$(CC) -c -o build/$(CONFIG)/obj/http-server.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) test/http-server.c

#
#   http-server
#
DEPS_48 += build/$(CONFIG)/inc/mpr.h
DEPS_48 += build/$(CONFIG)/inc/me.h
DEPS_48 += build/$(CONFIG)/inc/osdep.h
DEPS_48 += build/$(CONFIG)/obj/mprLib.o
DEPS_48 += build/$(CONFIG)/bin/libmpr.so
DEPS_48 += build/$(CONFIG)/inc/pcre.h
DEPS_48 += build/$(CONFIG)/obj/pcre.o
ifeq ($(ME_COM_PCRE),1)
    DEPS_48 += build/$(CONFIG)/bin/libpcre.so
endif
DEPS_48 += build/$(CONFIG)/inc/http.h
DEPS_48 += build/$(CONFIG)/obj/actionHandler.o
DEPS_48 += build/$(CONFIG)/obj/auth.o
DEPS_48 += build/$(CONFIG)/obj/basic.o
DEPS_48 += build/$(CONFIG)/obj/cache.o
DEPS_48 += build/$(CONFIG)/obj/chunkFilter.o
DEPS_48 += build/$(CONFIG)/obj/client.o
DEPS_48 += build/$(CONFIG)/obj/config.o
DEPS_48 += build/$(CONFIG)/obj/conn.o
DEPS_48 += build/$(CONFIG)/obj/digest.o
DEPS_48 += build/$(CONFIG)/obj/dirHandler.o
DEPS_48 += build/$(CONFIG)/obj/endpoint.o
DEPS_48 += build/$(CONFIG)/obj/error.o
DEPS_48 += build/$(CONFIG)/obj/fileHandler.o
DEPS_48 += build/$(CONFIG)/obj/host.o
DEPS_48 += build/$(CONFIG)/obj/monitor.o
DEPS_48 += build/$(CONFIG)/obj/netConnector.o
DEPS_48 += build/$(CONFIG)/obj/packet.o
DEPS_48 += build/$(CONFIG)/obj/pam.o
DEPS_48 += build/$(CONFIG)/obj/passHandler.o
DEPS_48 += build/$(CONFIG)/obj/pipeline.o
DEPS_48 += build/$(CONFIG)/obj/queue.o
DEPS_48 += build/$(CONFIG)/obj/rangeFilter.o
DEPS_48 += build/$(CONFIG)/obj/route.o
DEPS_48 += build/$(CONFIG)/obj/rx.o
DEPS_48 += build/$(CONFIG)/obj/sendConnector.o
DEPS_48 += build/$(CONFIG)/obj/service.o
DEPS_48 += build/$(CONFIG)/obj/session.o
DEPS_48 += build/$(CONFIG)/obj/stage.o
DEPS_48 += build/$(CONFIG)/obj/trace.o
DEPS_48 += build/$(CONFIG)/obj/tx.o
DEPS_48 += build/$(CONFIG)/obj/uploadFilter.o
DEPS_48 += build/$(CONFIG)/obj/uri.o
DEPS_48 += build/$(CONFIG)/obj/user.o
DEPS_48 += build/$(CONFIG)/obj/var.o
DEPS_48 += build/$(CONFIG)/obj/webSockFilter.o
DEPS_48 += build/$(CONFIG)/bin/libhttp.so
DEPS_48 += build/$(CONFIG)/obj/http-server.o

LIBS_48 += -lhttp
LIBS_48 += -lmpr
ifeq ($(ME_COM_PCRE),1)
    LIBS_48 += -lpcre
endif

build/$(CONFIG)/bin/http-server: $(DEPS_48)
	@echo '      [Link] build/$(CONFIG)/bin/http-server'
	$(CC) -o build/$(CONFIG)/bin/http-server $(LDFLAGS) $(LIBPATHS) "build/$(CONFIG)/obj/http-server.o" $(LIBPATHS_48) $(LIBS_48) $(LIBS_48) $(LIBS) $(LIBS) 

#
#   http.o
#
DEPS_49 += build/$(CONFIG)/inc/me.h
DEPS_49 += build/$(CONFIG)/inc/http.h

build/$(CONFIG)/obj/http.o: \
    src/http.c $(DEPS_49)
	@echo '   [Compile] build/$(CONFIG)/obj/http.o'
	$(CC) -c -o build/$(CONFIG)/obj/http.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/http.c

#
#   httpcmd
#
DEPS_50 += build/$(CONFIG)/inc/mpr.h
DEPS_50 += build/$(CONFIG)/inc/me.h
DEPS_50 += build/$(CONFIG)/inc/osdep.h
DEPS_50 += build/$(CONFIG)/obj/mprLib.o
DEPS_50 += build/$(CONFIG)/bin/libmpr.so
DEPS_50 += build/$(CONFIG)/inc/pcre.h
DEPS_50 += build/$(CONFIG)/obj/pcre.o
ifeq ($(ME_COM_PCRE),1)
    DEPS_50 += build/$(CONFIG)/bin/libpcre.so
endif
DEPS_50 += build/$(CONFIG)/inc/http.h
DEPS_50 += build/$(CONFIG)/obj/actionHandler.o
DEPS_50 += build/$(CONFIG)/obj/auth.o
DEPS_50 += build/$(CONFIG)/obj/basic.o
DEPS_50 += build/$(CONFIG)/obj/cache.o
DEPS_50 += build/$(CONFIG)/obj/chunkFilter.o
DEPS_50 += build/$(CONFIG)/obj/client.o
DEPS_50 += build/$(CONFIG)/obj/config.o
DEPS_50 += build/$(CONFIG)/obj/conn.o
DEPS_50 += build/$(CONFIG)/obj/digest.o
DEPS_50 += build/$(CONFIG)/obj/dirHandler.o
DEPS_50 += build/$(CONFIG)/obj/endpoint.o
DEPS_50 += build/$(CONFIG)/obj/error.o
DEPS_50 += build/$(CONFIG)/obj/fileHandler.o
DEPS_50 += build/$(CONFIG)/obj/host.o
DEPS_50 += build/$(CONFIG)/obj/monitor.o
DEPS_50 += build/$(CONFIG)/obj/netConnector.o
DEPS_50 += build/$(CONFIG)/obj/packet.o
DEPS_50 += build/$(CONFIG)/obj/pam.o
DEPS_50 += build/$(CONFIG)/obj/passHandler.o
DEPS_50 += build/$(CONFIG)/obj/pipeline.o
DEPS_50 += build/$(CONFIG)/obj/queue.o
DEPS_50 += build/$(CONFIG)/obj/rangeFilter.o
DEPS_50 += build/$(CONFIG)/obj/route.o
DEPS_50 += build/$(CONFIG)/obj/rx.o
DEPS_50 += build/$(CONFIG)/obj/sendConnector.o
DEPS_50 += build/$(CONFIG)/obj/service.o
DEPS_50 += build/$(CONFIG)/obj/session.o
DEPS_50 += build/$(CONFIG)/obj/stage.o
DEPS_50 += build/$(CONFIG)/obj/trace.o
DEPS_50 += build/$(CONFIG)/obj/tx.o
DEPS_50 += build/$(CONFIG)/obj/uploadFilter.o
DEPS_50 += build/$(CONFIG)/obj/uri.o
DEPS_50 += build/$(CONFIG)/obj/user.o
DEPS_50 += build/$(CONFIG)/obj/var.o
DEPS_50 += build/$(CONFIG)/obj/webSockFilter.o
DEPS_50 += build/$(CONFIG)/bin/libhttp.so
DEPS_50 += build/$(CONFIG)/obj/http.o

LIBS_50 += -lhttp
LIBS_50 += -lmpr
ifeq ($(ME_COM_PCRE),1)
    LIBS_50 += -lpcre
endif

build/$(CONFIG)/bin/http: $(DEPS_50)
	@echo '      [Link] build/$(CONFIG)/bin/http'
	$(CC) -o build/$(CONFIG)/bin/http $(LDFLAGS) $(LIBPATHS) "build/$(CONFIG)/obj/http.o" $(LIBPATHS_50) $(LIBS_50) $(LIBS_50) $(LIBS) $(LIBS) 

#
#   est.h
#
DEPS_51 += src/paks/est/est.h

build/$(CONFIG)/inc/est.h: $(DEPS_51)
	@echo '      [Copy] build/$(CONFIG)/inc/est.h'
	mkdir -p "build/$(CONFIG)/inc"
	cp src/paks/est/est.h build/$(CONFIG)/inc/est.h

#
#   estLib.o
#
DEPS_52 += build/$(CONFIG)/inc/me.h
DEPS_52 += build/$(CONFIG)/inc/est.h
DEPS_52 += build/$(CONFIG)/inc/osdep.h

build/$(CONFIG)/obj/estLib.o: \
    src/paks/est/estLib.c $(DEPS_52)
	@echo '   [Compile] build/$(CONFIG)/obj/estLib.o'
	$(CC) -c -o build/$(CONFIG)/obj/estLib.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) src/paks/est/estLib.c

ifeq ($(ME_COM_EST),1)
#
#   libest
#
DEPS_53 += build/$(CONFIG)/inc/est.h
DEPS_53 += build/$(CONFIG)/inc/me.h
DEPS_53 += build/$(CONFIG)/inc/osdep.h
DEPS_53 += build/$(CONFIG)/obj/estLib.o

build/$(CONFIG)/bin/libest.so: $(DEPS_53)
	@echo '      [Link] build/$(CONFIG)/bin/libest.so'
	$(CC) -shared -o build/$(CONFIG)/bin/libest.so $(LDFLAGS) $(LIBPATHS) "build/$(CONFIG)/obj/estLib.o" $(LIBS) 
endif

#
#   mprSsl.o
#
DEPS_54 += build/$(CONFIG)/inc/me.h
DEPS_54 += build/$(CONFIG)/inc/mpr.h

build/$(CONFIG)/obj/mprSsl.o: \
    src/paks/mpr/mprSsl.c $(DEPS_54)
	@echo '   [Compile] build/$(CONFIG)/obj/mprSsl.o'
	$(CC) -c -o build/$(CONFIG)/obj/mprSsl.o $(LDFLAGS) $(CFLAGS) $(DFLAGS) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/paks/mpr/mprSsl.c

#
#   libmprssl
#
DEPS_55 += build/$(CONFIG)/inc/mpr.h
DEPS_55 += build/$(CONFIG)/inc/me.h
DEPS_55 += build/$(CONFIG)/inc/osdep.h
DEPS_55 += build/$(CONFIG)/obj/mprLib.o
DEPS_55 += build/$(CONFIG)/bin/libmpr.so
DEPS_55 += build/$(CONFIG)/inc/est.h
DEPS_55 += build/$(CONFIG)/obj/estLib.o
ifeq ($(ME_COM_EST),1)
    DEPS_55 += build/$(CONFIG)/bin/libest.so
endif
DEPS_55 += build/$(CONFIG)/obj/mprSsl.o

LIBS_55 += -lmpr
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_55 += -lssl
    LIBPATHS_55 += -L$(ME_COM_OPENSSL_PATH)
endif
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_55 += -lcrypto
    LIBPATHS_55 += -L$(ME_COM_OPENSSL_PATH)
endif
ifeq ($(ME_COM_EST),1)
    LIBS_55 += -lest
endif

build/$(CONFIG)/bin/libmprssl.so: $(DEPS_55)
	@echo '      [Link] build/$(CONFIG)/bin/libmprssl.so'
	$(CC) -shared -o build/$(CONFIG)/bin/libmprssl.so $(LDFLAGS) $(LIBPATHS)  "build/$(CONFIG)/obj/mprSsl.o" $(LIBPATHS_55) $(LIBS_55) $(LIBS_55) $(LIBS) 

#
#   stop
#
stop: $(DEPS_56)

#
#   installBinary
#
installBinary: $(DEPS_57)

#
#   start
#
start: $(DEPS_58)

#
#   install
#
DEPS_59 += stop
DEPS_59 += installBinary
DEPS_59 += start

install: $(DEPS_59)

#
#   uninstall
#
DEPS_60 += stop

uninstall: $(DEPS_60)

#
#   version
#
version: $(DEPS_61)
	echo 5.2.0

