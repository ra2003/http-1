#
#   http-linux-static.mk -- Makefile to build Embedthis Http for linux
#

NAME                  := http
VERSION               := 5.0.0
PROFILE               ?= static
ARCH                  ?= $(shell uname -m | sed 's/i.86/x86/;s/x86_64/x64/;s/arm.*/arm/;s/mips.*/mips/')
CC_ARCH               ?= $(shell echo $(ARCH) | sed 's/x86/i686/;s/x64/x86_64/')
OS                    ?= linux
CC                    ?= gcc
CONFIG                ?= $(OS)-$(ARCH)-$(PROFILE)
LBIN                  ?= $(CONFIG)/bin
PATH                  := $(LBIN):$(PATH)

ME_COM_EST            ?= 0
ME_COM_MATRIXSSL      ?= 0
ME_COM_NANOSSL        ?= 0
ME_COM_OPENSSL        ?= 1
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

ME_COM_COMPILER_PATH  ?= gcc
ME_COM_LIB_PATH       ?= ar
ME_COM_MATRIXSSL_PATH ?= /usr/src/matrixssl
ME_COM_NANOSSL_PATH   ?= /usr/src/nanossl
ME_COM_OPENSSL_PATH   ?= /usr/src/openssl

CFLAGS                += -g -w
DFLAGS                +=  $(patsubst %,-D%,$(filter ME_%,$(MAKEFLAGS))) -DME_COM_EST=$(ME_COM_EST) -DME_COM_MATRIXSSL=$(ME_COM_MATRIXSSL) -DME_COM_NANOSSL=$(ME_COM_NANOSSL) -DME_COM_OPENSSL=$(ME_COM_OPENSSL) -DME_COM_PCRE=$(ME_COM_PCRE) -DME_COM_SSL=$(ME_COM_SSL) -DME_COM_VXWORKS=$(ME_COM_VXWORKS) -DME_COM_WINSDK=$(ME_COM_WINSDK) 
IFLAGS                += "-I$(CONFIG)/inc"
LDFLAGS               += 
LIBPATHS              += -L$(CONFIG)/bin
LIBS                  += -lrt -ldl -lpthread -lm

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


TARGETS               += $(CONFIG)/bin/ca.crt
TARGETS               += $(CONFIG)/bin/http
TARGETS               += $(CONFIG)/bin/libmprssl.a
TARGETS               += $(CONFIG)/bin/testHttp

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
	@[ ! -x $(CONFIG)/bin ] && mkdir -p $(CONFIG)/bin; true
	@[ ! -x $(CONFIG)/inc ] && mkdir -p $(CONFIG)/inc; true
	@[ ! -x $(CONFIG)/obj ] && mkdir -p $(CONFIG)/obj; true
	@[ ! -f $(CONFIG)/inc/osdep.h ] && cp src/paks/osdep/osdep.h $(CONFIG)/inc/osdep.h ; true
	@if ! diff $(CONFIG)/inc/osdep.h src/paks/osdep/osdep.h >/dev/null ; then\
		cp src/paks/osdep/osdep.h $(CONFIG)/inc/osdep.h  ; \
	fi; true
	@[ ! -f $(CONFIG)/inc/me.h ] && cp projects/http-linux-static-me.h $(CONFIG)/inc/me.h ; true
	@if ! diff $(CONFIG)/inc/me.h projects/http-linux-static-me.h >/dev/null ; then\
		cp projects/http-linux-static-me.h $(CONFIG)/inc/me.h  ; \
	fi; true
	@if [ -f "$(CONFIG)/.makeflags" ] ; then \
		if [ "$(MAKEFLAGS)" != " ` cat $(CONFIG)/.makeflags`" ] ; then \
			echo "   [Warning] Make flags have changed since the last build: "`cat $(CONFIG)/.makeflags`"" ; \
		fi ; \
	fi
	@echo $(MAKEFLAGS) >$(CONFIG)/.makeflags

clean:
	rm -f "$(CONFIG)/obj/actionHandler.o"
	rm -f "$(CONFIG)/obj/auth.o"
	rm -f "$(CONFIG)/obj/basic.o"
	rm -f "$(CONFIG)/obj/cache.o"
	rm -f "$(CONFIG)/obj/chunkFilter.o"
	rm -f "$(CONFIG)/obj/client.o"
	rm -f "$(CONFIG)/obj/config.o"
	rm -f "$(CONFIG)/obj/conn.o"
	rm -f "$(CONFIG)/obj/digest.o"
	rm -f "$(CONFIG)/obj/endpoint.o"
	rm -f "$(CONFIG)/obj/error.o"
	rm -f "$(CONFIG)/obj/host.o"
	rm -f "$(CONFIG)/obj/http.o"
	rm -f "$(CONFIG)/obj/log.o"
	rm -f "$(CONFIG)/obj/makerom.o"
	rm -f "$(CONFIG)/obj/monitor.o"
	rm -f "$(CONFIG)/obj/mprLib.o"
	rm -f "$(CONFIG)/obj/mprSsl.o"
	rm -f "$(CONFIG)/obj/netConnector.o"
	rm -f "$(CONFIG)/obj/packet.o"
	rm -f "$(CONFIG)/obj/pam.o"
	rm -f "$(CONFIG)/obj/passHandler.o"
	rm -f "$(CONFIG)/obj/pcre.o"
	rm -f "$(CONFIG)/obj/pipeline.o"
	rm -f "$(CONFIG)/obj/queue.o"
	rm -f "$(CONFIG)/obj/rangeFilter.o"
	rm -f "$(CONFIG)/obj/route.o"
	rm -f "$(CONFIG)/obj/rx.o"
	rm -f "$(CONFIG)/obj/sendConnector.o"
	rm -f "$(CONFIG)/obj/service.o"
	rm -f "$(CONFIG)/obj/session.o"
	rm -f "$(CONFIG)/obj/stage.o"
	rm -f "$(CONFIG)/obj/testHttp.o"
	rm -f "$(CONFIG)/obj/testHttpGen.o"
	rm -f "$(CONFIG)/obj/testHttpUri.o"
	rm -f "$(CONFIG)/obj/trace.o"
	rm -f "$(CONFIG)/obj/tx.o"
	rm -f "$(CONFIG)/obj/uploadFilter.o"
	rm -f "$(CONFIG)/obj/uri.o"
	rm -f "$(CONFIG)/obj/var.o"
	rm -f "$(CONFIG)/obj/webSockFilter.o"
	rm -f "$(CONFIG)/bin/ca.crt"
	rm -f "$(CONFIG)/bin/http"
	rm -f "$(CONFIG)/bin/libhttp.a"
	rm -f "$(CONFIG)/bin/libmpr.a"
	rm -f "$(CONFIG)/bin/libmprssl.a"
	rm -f "$(CONFIG)/bin/libpcre.a"
	rm -f "$(CONFIG)/bin/makerom"
	rm -f "$(CONFIG)/bin/testHttp"

clobber: clean
	rm -fr ./$(CONFIG)



#
#   http-ca-crt
#
DEPS_1 += src/paks/est/ca.crt

$(CONFIG)/bin/ca.crt: $(DEPS_1)
	@echo '      [Copy] $(CONFIG)/bin/ca.crt'
	mkdir -p "$(CONFIG)/bin"
	cp src/paks/est/ca.crt $(CONFIG)/bin/ca.crt

#
#   mpr.h
#
$(CONFIG)/inc/mpr.h: $(DEPS_2)
	@echo '      [Copy] $(CONFIG)/inc/mpr.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/paks/mpr/mpr.h $(CONFIG)/inc/mpr.h

#
#   me.h
#
$(CONFIG)/inc/me.h: $(DEPS_3)
	@echo '      [Copy] $(CONFIG)/inc/me.h'

#
#   osdep.h
#
$(CONFIG)/inc/osdep.h: $(DEPS_4)
	@echo '      [Copy] $(CONFIG)/inc/osdep.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/paks/osdep/osdep.h $(CONFIG)/inc/osdep.h

#
#   mprLib.o
#
DEPS_5 += $(CONFIG)/inc/me.h
DEPS_5 += $(CONFIG)/inc/mpr.h
DEPS_5 += $(CONFIG)/inc/osdep.h

$(CONFIG)/obj/mprLib.o: \
    src/paks/mpr/mprLib.c $(DEPS_5)
	@echo '   [Compile] $(CONFIG)/obj/mprLib.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/mprLib.o $(LDFLAGS) $(IFLAGS) src/paks/mpr/mprLib.c

#
#   libmpr
#
DEPS_6 += $(CONFIG)/inc/mpr.h
DEPS_6 += $(CONFIG)/inc/me.h
DEPS_6 += $(CONFIG)/inc/osdep.h
DEPS_6 += $(CONFIG)/obj/mprLib.o

$(CONFIG)/bin/libmpr.a: $(DEPS_6)
	@echo '      [Link] $(CONFIG)/bin/libmpr.a'
	ar -cr $(CONFIG)/bin/libmpr.a "$(CONFIG)/obj/mprLib.o"

#
#   pcre.h
#
$(CONFIG)/inc/pcre.h: $(DEPS_7)
	@echo '      [Copy] $(CONFIG)/inc/pcre.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/paks/pcre/pcre.h $(CONFIG)/inc/pcre.h

#
#   pcre.o
#
DEPS_8 += $(CONFIG)/inc/me.h
DEPS_8 += $(CONFIG)/inc/pcre.h

$(CONFIG)/obj/pcre.o: \
    src/paks/pcre/pcre.c $(DEPS_8)
	@echo '   [Compile] $(CONFIG)/obj/pcre.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/pcre.o $(LDFLAGS) $(IFLAGS) src/paks/pcre/pcre.c

ifeq ($(ME_COM_PCRE),1)
#
#   libpcre
#
DEPS_9 += $(CONFIG)/inc/pcre.h
DEPS_9 += $(CONFIG)/inc/me.h
DEPS_9 += $(CONFIG)/obj/pcre.o

$(CONFIG)/bin/libpcre.a: $(DEPS_9)
	@echo '      [Link] $(CONFIG)/bin/libpcre.a'
	ar -cr $(CONFIG)/bin/libpcre.a "$(CONFIG)/obj/pcre.o"
endif

#
#   http.h
#
$(CONFIG)/inc/http.h: $(DEPS_10)
	@echo '      [Copy] $(CONFIG)/inc/http.h'
	mkdir -p "$(CONFIG)/inc"
	cp src/http.h $(CONFIG)/inc/http.h

#
#   actionHandler.o
#
DEPS_11 += $(CONFIG)/inc/me.h
DEPS_11 += $(CONFIG)/inc/http.h
DEPS_11 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/actionHandler.o: \
    src/actionHandler.c $(DEPS_11)
	@echo '   [Compile] $(CONFIG)/obj/actionHandler.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/actionHandler.o $(LDFLAGS) $(IFLAGS) src/actionHandler.c

#
#   auth.o
#
DEPS_12 += $(CONFIG)/inc/me.h
DEPS_12 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/auth.o: \
    src/auth.c $(DEPS_12)
	@echo '   [Compile] $(CONFIG)/obj/auth.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/auth.o $(LDFLAGS) $(IFLAGS) src/auth.c

#
#   basic.o
#
DEPS_13 += $(CONFIG)/inc/me.h
DEPS_13 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/basic.o: \
    src/basic.c $(DEPS_13)
	@echo '   [Compile] $(CONFIG)/obj/basic.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/basic.o $(LDFLAGS) $(IFLAGS) src/basic.c

#
#   cache.o
#
DEPS_14 += $(CONFIG)/inc/me.h
DEPS_14 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/cache.o: \
    src/cache.c $(DEPS_14)
	@echo '   [Compile] $(CONFIG)/obj/cache.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/cache.o $(LDFLAGS) $(IFLAGS) src/cache.c

#
#   chunkFilter.o
#
DEPS_15 += $(CONFIG)/inc/me.h
DEPS_15 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_15)
	@echo '   [Compile] $(CONFIG)/obj/chunkFilter.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/chunkFilter.o $(LDFLAGS) $(IFLAGS) src/chunkFilter.c

#
#   client.o
#
DEPS_16 += $(CONFIG)/inc/me.h
DEPS_16 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/client.o: \
    src/client.c $(DEPS_16)
	@echo '   [Compile] $(CONFIG)/obj/client.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/client.o $(LDFLAGS) $(IFLAGS) src/client.c

#
#   config.o
#
DEPS_17 += $(CONFIG)/inc/me.h
DEPS_17 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/config.o: \
    src/config.c $(DEPS_17)
	@echo '   [Compile] $(CONFIG)/obj/config.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/config.o $(LDFLAGS) $(IFLAGS) src/config.c

#
#   conn.o
#
DEPS_18 += $(CONFIG)/inc/me.h
DEPS_18 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/conn.o: \
    src/conn.c $(DEPS_18)
	@echo '   [Compile] $(CONFIG)/obj/conn.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/conn.o $(LDFLAGS) $(IFLAGS) src/conn.c

#
#   digest.o
#
DEPS_19 += $(CONFIG)/inc/me.h
DEPS_19 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/digest.o: \
    src/digest.c $(DEPS_19)
	@echo '   [Compile] $(CONFIG)/obj/digest.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/digest.o $(LDFLAGS) $(IFLAGS) src/digest.c

#
#   endpoint.o
#
DEPS_20 += $(CONFIG)/inc/me.h
DEPS_20 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_20)
	@echo '   [Compile] $(CONFIG)/obj/endpoint.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/endpoint.o $(LDFLAGS) $(IFLAGS) src/endpoint.c

#
#   error.o
#
DEPS_21 += $(CONFIG)/inc/me.h
DEPS_21 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/error.o: \
    src/error.c $(DEPS_21)
	@echo '   [Compile] $(CONFIG)/obj/error.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/error.o $(LDFLAGS) $(IFLAGS) src/error.c

#
#   host.o
#
DEPS_22 += $(CONFIG)/inc/me.h
DEPS_22 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/host.o: \
    src/host.c $(DEPS_22)
	@echo '   [Compile] $(CONFIG)/obj/host.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/host.o $(LDFLAGS) $(IFLAGS) src/host.c

#
#   log.o
#
DEPS_23 += $(CONFIG)/inc/me.h
DEPS_23 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/log.o: \
    src/log.c $(DEPS_23)
	@echo '   [Compile] $(CONFIG)/obj/log.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/log.o $(LDFLAGS) $(IFLAGS) src/log.c

#
#   monitor.o
#
DEPS_24 += $(CONFIG)/inc/me.h
DEPS_24 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/monitor.o: \
    src/monitor.c $(DEPS_24)
	@echo '   [Compile] $(CONFIG)/obj/monitor.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/monitor.o $(LDFLAGS) $(IFLAGS) src/monitor.c

#
#   netConnector.o
#
DEPS_25 += $(CONFIG)/inc/me.h
DEPS_25 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_25)
	@echo '   [Compile] $(CONFIG)/obj/netConnector.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/netConnector.o $(LDFLAGS) $(IFLAGS) src/netConnector.c

#
#   packet.o
#
DEPS_26 += $(CONFIG)/inc/me.h
DEPS_26 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/packet.o: \
    src/packet.c $(DEPS_26)
	@echo '   [Compile] $(CONFIG)/obj/packet.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/packet.o $(LDFLAGS) $(IFLAGS) src/packet.c

#
#   pam.o
#
DEPS_27 += $(CONFIG)/inc/me.h
DEPS_27 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/pam.o: \
    src/pam.c $(DEPS_27)
	@echo '   [Compile] $(CONFIG)/obj/pam.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/pam.o $(LDFLAGS) $(IFLAGS) src/pam.c

#
#   passHandler.o
#
DEPS_28 += $(CONFIG)/inc/me.h
DEPS_28 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_28)
	@echo '   [Compile] $(CONFIG)/obj/passHandler.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/passHandler.o $(LDFLAGS) $(IFLAGS) src/passHandler.c

#
#   pipeline.o
#
DEPS_29 += $(CONFIG)/inc/me.h
DEPS_29 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_29)
	@echo '   [Compile] $(CONFIG)/obj/pipeline.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/pipeline.o $(LDFLAGS) $(IFLAGS) src/pipeline.c

#
#   queue.o
#
DEPS_30 += $(CONFIG)/inc/me.h
DEPS_30 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/queue.o: \
    src/queue.c $(DEPS_30)
	@echo '   [Compile] $(CONFIG)/obj/queue.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/queue.o $(LDFLAGS) $(IFLAGS) src/queue.c

#
#   rangeFilter.o
#
DEPS_31 += $(CONFIG)/inc/me.h
DEPS_31 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_31)
	@echo '   [Compile] $(CONFIG)/obj/rangeFilter.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/rangeFilter.o $(LDFLAGS) $(IFLAGS) src/rangeFilter.c

#
#   route.o
#
DEPS_32 += $(CONFIG)/inc/me.h
DEPS_32 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/route.o: \
    src/route.c $(DEPS_32)
	@echo '   [Compile] $(CONFIG)/obj/route.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/route.o $(LDFLAGS) $(IFLAGS) src/route.c

#
#   rx.o
#
DEPS_33 += $(CONFIG)/inc/me.h
DEPS_33 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/rx.o: \
    src/rx.c $(DEPS_33)
	@echo '   [Compile] $(CONFIG)/obj/rx.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/rx.o $(LDFLAGS) $(IFLAGS) src/rx.c

#
#   sendConnector.o
#
DEPS_34 += $(CONFIG)/inc/me.h
DEPS_34 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/sendConnector.o: \
    src/sendConnector.c $(DEPS_34)
	@echo '   [Compile] $(CONFIG)/obj/sendConnector.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/sendConnector.o $(LDFLAGS) $(IFLAGS) src/sendConnector.c

#
#   service.o
#
DEPS_35 += $(CONFIG)/inc/me.h
DEPS_35 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/service.o: \
    src/service.c $(DEPS_35)
	@echo '   [Compile] $(CONFIG)/obj/service.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/service.o $(LDFLAGS) $(IFLAGS) src/service.c

#
#   session.o
#
DEPS_36 += $(CONFIG)/inc/me.h
DEPS_36 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/session.o: \
    src/session.c $(DEPS_36)
	@echo '   [Compile] $(CONFIG)/obj/session.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/session.o $(LDFLAGS) $(IFLAGS) src/session.c

#
#   stage.o
#
DEPS_37 += $(CONFIG)/inc/me.h
DEPS_37 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/stage.o: \
    src/stage.c $(DEPS_37)
	@echo '   [Compile] $(CONFIG)/obj/stage.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/stage.o $(LDFLAGS) $(IFLAGS) src/stage.c

#
#   trace.o
#
DEPS_38 += $(CONFIG)/inc/me.h
DEPS_38 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/trace.o: \
    src/trace.c $(DEPS_38)
	@echo '   [Compile] $(CONFIG)/obj/trace.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/trace.o $(LDFLAGS) $(IFLAGS) src/trace.c

#
#   tx.o
#
DEPS_39 += $(CONFIG)/inc/me.h
DEPS_39 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/tx.o: \
    src/tx.c $(DEPS_39)
	@echo '   [Compile] $(CONFIG)/obj/tx.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/tx.o $(LDFLAGS) $(IFLAGS) src/tx.c

#
#   uploadFilter.o
#
DEPS_40 += $(CONFIG)/inc/me.h
DEPS_40 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_40)
	@echo '   [Compile] $(CONFIG)/obj/uploadFilter.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/uploadFilter.o $(LDFLAGS) $(IFLAGS) src/uploadFilter.c

#
#   uri.o
#
DEPS_41 += $(CONFIG)/inc/me.h
DEPS_41 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/uri.o: \
    src/uri.c $(DEPS_41)
	@echo '   [Compile] $(CONFIG)/obj/uri.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/uri.o $(LDFLAGS) $(IFLAGS) src/uri.c

#
#   var.o
#
DEPS_42 += $(CONFIG)/inc/me.h
DEPS_42 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/var.o: \
    src/var.c $(DEPS_42)
	@echo '   [Compile] $(CONFIG)/obj/var.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/var.o $(LDFLAGS) $(IFLAGS) src/var.c

#
#   webSockFilter.o
#
DEPS_43 += $(CONFIG)/inc/me.h
DEPS_43 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/webSockFilter.o: \
    src/webSockFilter.c $(DEPS_43)
	@echo '   [Compile] $(CONFIG)/obj/webSockFilter.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/webSockFilter.o $(LDFLAGS) $(IFLAGS) src/webSockFilter.c

#
#   libhttp
#
DEPS_44 += $(CONFIG)/inc/mpr.h
DEPS_44 += $(CONFIG)/inc/me.h
DEPS_44 += $(CONFIG)/inc/osdep.h
DEPS_44 += $(CONFIG)/obj/mprLib.o
DEPS_44 += $(CONFIG)/bin/libmpr.a
DEPS_44 += $(CONFIG)/inc/pcre.h
DEPS_44 += $(CONFIG)/obj/pcre.o
ifeq ($(ME_COM_PCRE),1)
    DEPS_44 += $(CONFIG)/bin/libpcre.a
endif
DEPS_44 += $(CONFIG)/inc/http.h
DEPS_44 += $(CONFIG)/obj/actionHandler.o
DEPS_44 += $(CONFIG)/obj/auth.o
DEPS_44 += $(CONFIG)/obj/basic.o
DEPS_44 += $(CONFIG)/obj/cache.o
DEPS_44 += $(CONFIG)/obj/chunkFilter.o
DEPS_44 += $(CONFIG)/obj/client.o
DEPS_44 += $(CONFIG)/obj/config.o
DEPS_44 += $(CONFIG)/obj/conn.o
DEPS_44 += $(CONFIG)/obj/digest.o
DEPS_44 += $(CONFIG)/obj/endpoint.o
DEPS_44 += $(CONFIG)/obj/error.o
DEPS_44 += $(CONFIG)/obj/host.o
DEPS_44 += $(CONFIG)/obj/log.o
DEPS_44 += $(CONFIG)/obj/monitor.o
DEPS_44 += $(CONFIG)/obj/netConnector.o
DEPS_44 += $(CONFIG)/obj/packet.o
DEPS_44 += $(CONFIG)/obj/pam.o
DEPS_44 += $(CONFIG)/obj/passHandler.o
DEPS_44 += $(CONFIG)/obj/pipeline.o
DEPS_44 += $(CONFIG)/obj/queue.o
DEPS_44 += $(CONFIG)/obj/rangeFilter.o
DEPS_44 += $(CONFIG)/obj/route.o
DEPS_44 += $(CONFIG)/obj/rx.o
DEPS_44 += $(CONFIG)/obj/sendConnector.o
DEPS_44 += $(CONFIG)/obj/service.o
DEPS_44 += $(CONFIG)/obj/session.o
DEPS_44 += $(CONFIG)/obj/stage.o
DEPS_44 += $(CONFIG)/obj/trace.o
DEPS_44 += $(CONFIG)/obj/tx.o
DEPS_44 += $(CONFIG)/obj/uploadFilter.o
DEPS_44 += $(CONFIG)/obj/uri.o
DEPS_44 += $(CONFIG)/obj/var.o
DEPS_44 += $(CONFIG)/obj/webSockFilter.o

$(CONFIG)/bin/libhttp.a: $(DEPS_44)
	@echo '      [Link] $(CONFIG)/bin/libhttp.a'
	ar -cr $(CONFIG)/bin/libhttp.a "$(CONFIG)/obj/actionHandler.o" "$(CONFIG)/obj/auth.o" "$(CONFIG)/obj/basic.o" "$(CONFIG)/obj/cache.o" "$(CONFIG)/obj/chunkFilter.o" "$(CONFIG)/obj/client.o" "$(CONFIG)/obj/config.o" "$(CONFIG)/obj/conn.o" "$(CONFIG)/obj/digest.o" "$(CONFIG)/obj/endpoint.o" "$(CONFIG)/obj/error.o" "$(CONFIG)/obj/host.o" "$(CONFIG)/obj/log.o" "$(CONFIG)/obj/monitor.o" "$(CONFIG)/obj/netConnector.o" "$(CONFIG)/obj/packet.o" "$(CONFIG)/obj/pam.o" "$(CONFIG)/obj/passHandler.o" "$(CONFIG)/obj/pipeline.o" "$(CONFIG)/obj/queue.o" "$(CONFIG)/obj/rangeFilter.o" "$(CONFIG)/obj/route.o" "$(CONFIG)/obj/rx.o" "$(CONFIG)/obj/sendConnector.o" "$(CONFIG)/obj/service.o" "$(CONFIG)/obj/session.o" "$(CONFIG)/obj/stage.o" "$(CONFIG)/obj/trace.o" "$(CONFIG)/obj/tx.o" "$(CONFIG)/obj/uploadFilter.o" "$(CONFIG)/obj/uri.o" "$(CONFIG)/obj/var.o" "$(CONFIG)/obj/webSockFilter.o"

#
#   http.o
#
DEPS_45 += $(CONFIG)/inc/me.h
DEPS_45 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/http.o: \
    src/http.c $(DEPS_45)
	@echo '   [Compile] $(CONFIG)/obj/http.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/http.o $(LDFLAGS) $(IFLAGS) src/http.c

#
#   httpcmd
#
DEPS_46 += $(CONFIG)/inc/mpr.h
DEPS_46 += $(CONFIG)/inc/me.h
DEPS_46 += $(CONFIG)/inc/osdep.h
DEPS_46 += $(CONFIG)/obj/mprLib.o
DEPS_46 += $(CONFIG)/bin/libmpr.a
DEPS_46 += $(CONFIG)/inc/pcre.h
DEPS_46 += $(CONFIG)/obj/pcre.o
ifeq ($(ME_COM_PCRE),1)
    DEPS_46 += $(CONFIG)/bin/libpcre.a
endif
DEPS_46 += $(CONFIG)/inc/http.h
DEPS_46 += $(CONFIG)/obj/actionHandler.o
DEPS_46 += $(CONFIG)/obj/auth.o
DEPS_46 += $(CONFIG)/obj/basic.o
DEPS_46 += $(CONFIG)/obj/cache.o
DEPS_46 += $(CONFIG)/obj/chunkFilter.o
DEPS_46 += $(CONFIG)/obj/client.o
DEPS_46 += $(CONFIG)/obj/config.o
DEPS_46 += $(CONFIG)/obj/conn.o
DEPS_46 += $(CONFIG)/obj/digest.o
DEPS_46 += $(CONFIG)/obj/endpoint.o
DEPS_46 += $(CONFIG)/obj/error.o
DEPS_46 += $(CONFIG)/obj/host.o
DEPS_46 += $(CONFIG)/obj/log.o
DEPS_46 += $(CONFIG)/obj/monitor.o
DEPS_46 += $(CONFIG)/obj/netConnector.o
DEPS_46 += $(CONFIG)/obj/packet.o
DEPS_46 += $(CONFIG)/obj/pam.o
DEPS_46 += $(CONFIG)/obj/passHandler.o
DEPS_46 += $(CONFIG)/obj/pipeline.o
DEPS_46 += $(CONFIG)/obj/queue.o
DEPS_46 += $(CONFIG)/obj/rangeFilter.o
DEPS_46 += $(CONFIG)/obj/route.o
DEPS_46 += $(CONFIG)/obj/rx.o
DEPS_46 += $(CONFIG)/obj/sendConnector.o
DEPS_46 += $(CONFIG)/obj/service.o
DEPS_46 += $(CONFIG)/obj/session.o
DEPS_46 += $(CONFIG)/obj/stage.o
DEPS_46 += $(CONFIG)/obj/trace.o
DEPS_46 += $(CONFIG)/obj/tx.o
DEPS_46 += $(CONFIG)/obj/uploadFilter.o
DEPS_46 += $(CONFIG)/obj/uri.o
DEPS_46 += $(CONFIG)/obj/var.o
DEPS_46 += $(CONFIG)/obj/webSockFilter.o
DEPS_46 += $(CONFIG)/bin/libhttp.a
DEPS_46 += $(CONFIG)/obj/http.o

LIBS_46 += -lhttp
LIBS_46 += -lmpr
ifeq ($(ME_COM_PCRE),1)
    LIBS_46 += -lpcre
endif

$(CONFIG)/bin/http: $(DEPS_46)
	@echo '      [Link] $(CONFIG)/bin/http'
	$(CC) -o $(CONFIG)/bin/http $(LDFLAGS) $(LIBPATHS) "$(CONFIG)/obj/http.o" $(LIBPATHS_46) $(LIBS_46) $(LIBS_46) $(LIBS) $(LIBS) 

#
#   est.h
#
$(CONFIG)/inc/est.h: $(DEPS_47)
	@echo '      [Copy] $(CONFIG)/inc/est.h'

#
#   mprSsl.o
#
DEPS_48 += $(CONFIG)/inc/me.h
DEPS_48 += $(CONFIG)/inc/mpr.h
DEPS_48 += $(CONFIG)/inc/est.h

$(CONFIG)/obj/mprSsl.o: \
    src/paks/mpr/mprSsl.c $(DEPS_48)
	@echo '   [Compile] $(CONFIG)/obj/mprSsl.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/mprSsl.o $(LDFLAGS) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" "-I$(ME_COM_MATRIXSSL_PATH)" "-I$(ME_COM_MATRIXSSL_PATH)/matrixssl" "-I$(ME_COM_NANOSSL_PATH)/src" src/paks/mpr/mprSsl.c

#
#   libmprssl
#
DEPS_49 += $(CONFIG)/inc/mpr.h
DEPS_49 += $(CONFIG)/inc/me.h
DEPS_49 += $(CONFIG)/inc/osdep.h
DEPS_49 += $(CONFIG)/obj/mprLib.o
DEPS_49 += $(CONFIG)/bin/libmpr.a
DEPS_49 += $(CONFIG)/inc/est.h
DEPS_49 += $(CONFIG)/obj/mprSsl.o

$(CONFIG)/bin/libmprssl.a: $(DEPS_49)
	@echo '      [Link] $(CONFIG)/bin/libmprssl.a'
	ar -cr $(CONFIG)/bin/libmprssl.a "$(CONFIG)/obj/mprSsl.o"

#
#   testHttp.o
#
DEPS_50 += $(CONFIG)/inc/me.h
DEPS_50 += $(CONFIG)/inc/mpr.h

$(CONFIG)/obj/testHttp.o: \
    test/src/testHttp.c $(DEPS_50)
	@echo '   [Compile] $(CONFIG)/obj/testHttp.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/testHttp.o $(LDFLAGS) $(IFLAGS) test/src/testHttp.c

#
#   testHttpGen.o
#
DEPS_51 += $(CONFIG)/inc/me.h
DEPS_51 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/testHttpGen.o: \
    test/src/testHttpGen.c $(DEPS_51)
	@echo '   [Compile] $(CONFIG)/obj/testHttpGen.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/testHttpGen.o $(LDFLAGS) $(IFLAGS) test/src/testHttpGen.c

#
#   testHttpUri.o
#
DEPS_52 += $(CONFIG)/inc/me.h
DEPS_52 += $(CONFIG)/inc/http.h

$(CONFIG)/obj/testHttpUri.o: \
    test/src/testHttpUri.c $(DEPS_52)
	@echo '   [Compile] $(CONFIG)/obj/testHttpUri.o'
	$(CC) -c $(DFLAGS) -o $(CONFIG)/obj/testHttpUri.o $(LDFLAGS) $(IFLAGS) test/src/testHttpUri.c

#
#   testHttp
#
DEPS_53 += $(CONFIG)/inc/mpr.h
DEPS_53 += $(CONFIG)/inc/me.h
DEPS_53 += $(CONFIG)/inc/osdep.h
DEPS_53 += $(CONFIG)/obj/mprLib.o
DEPS_53 += $(CONFIG)/bin/libmpr.a
DEPS_53 += $(CONFIG)/inc/pcre.h
DEPS_53 += $(CONFIG)/obj/pcre.o
ifeq ($(ME_COM_PCRE),1)
    DEPS_53 += $(CONFIG)/bin/libpcre.a
endif
DEPS_53 += $(CONFIG)/inc/http.h
DEPS_53 += $(CONFIG)/obj/actionHandler.o
DEPS_53 += $(CONFIG)/obj/auth.o
DEPS_53 += $(CONFIG)/obj/basic.o
DEPS_53 += $(CONFIG)/obj/cache.o
DEPS_53 += $(CONFIG)/obj/chunkFilter.o
DEPS_53 += $(CONFIG)/obj/client.o
DEPS_53 += $(CONFIG)/obj/config.o
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
DEPS_53 += $(CONFIG)/bin/libhttp.a
DEPS_53 += $(CONFIG)/obj/testHttp.o
DEPS_53 += $(CONFIG)/obj/testHttpGen.o
DEPS_53 += $(CONFIG)/obj/testHttpUri.o

LIBS_53 += -lhttp
LIBS_53 += -lmpr
ifeq ($(ME_COM_PCRE),1)
    LIBS_53 += -lpcre
endif

$(CONFIG)/bin/testHttp: $(DEPS_53)
	@echo '      [Link] $(CONFIG)/bin/testHttp'
	$(CC) -o $(CONFIG)/bin/testHttp $(LDFLAGS) $(LIBPATHS) "$(CONFIG)/obj/testHttp.o" "$(CONFIG)/obj/testHttpGen.o" "$(CONFIG)/obj/testHttpUri.o" $(LIBPATHS_53) $(LIBS_53) $(LIBS_53) $(LIBS) $(LIBS) 

#
#   stop
#
stop: $(DEPS_54)

#
#   installBinary
#
installBinary: $(DEPS_55)

#
#   start
#
start: $(DEPS_56)

#
#   install
#
DEPS_57 += stop
DEPS_57 += installBinary
DEPS_57 += start

install: $(DEPS_57)

#
#   uninstall
#
DEPS_58 += stop

uninstall: $(DEPS_58)

#
#   version
#
version: $(DEPS_59)
	echo 5.0.0

