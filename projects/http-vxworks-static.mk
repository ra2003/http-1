#
#   http-vxworks-static.mk -- Makefile to build Embedthis Http for vxworks
#

NAME                  := http
VERSION               := 8.0.3
PROFILE               ?= static
ARCH                  ?= $(shell echo $(WIND_HOST_TYPE) | sed 's/-.*$(ME_ROOT_PREFIX)/')
CPU                   ?= $(subst X86,PENTIUM,$(shell echo $(ARCH) | tr a-z A-Z))
OS                    ?= vxworks
CC                    ?= cc$(subst x86,pentium,$(ARCH))
LD                    ?= ldundefined
AR                    ?= arundefined
CONFIG                ?= $(OS)-$(ARCH)-$(PROFILE)
BUILD                 ?= build/$(CONFIG)
LBIN                  ?= $(BUILD)/bin
PATH                  := $(LBIN):$(PATH)

ME_COM_COMPILER       ?= 1
ME_COM_LIB            ?= 1
ME_COM_LINK           ?= 1
ME_COM_MATRIXSSL      ?= 0
ME_COM_MBEDTLS        ?= 1
ME_COM_MPR            ?= 1
ME_COM_NANOSSL        ?= 0
ME_COM_OPENSSL        ?= 0
ME_COM_OSDEP          ?= 1
ME_COM_PCRE           ?= 1
ME_COM_SSL            ?= 1
ME_COM_VXWORKS        ?= 0

ME_COM_OPENSSL_PATH   ?= "/path/to/openssl"

ifeq ($(ME_COM_LIB),1)
    ME_COM_COMPILER := 1
endif
ifeq ($(ME_COM_LINK),1)
    ME_COM_COMPILER := 1
endif
ifeq ($(ME_COM_MBEDTLS),1)
    ME_COM_SSL := 1
endif
ifeq ($(ME_COM_OPENSSL),1)
    ME_COM_SSL := 1
endif

export PATH           := $(WIND_GNU_PATH)/$(WIND_HOST_TYPE)/bin:$(PATH)
CFLAGS                += -fno-builtin -fno-defer-pop -fvolatile -w
DFLAGS                += -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h" $(patsubst %,-D%,$(filter ME_%,$(MAKEFLAGS))) -DME_COM_COMPILER=$(ME_COM_COMPILER) -DME_COM_LIB=$(ME_COM_LIB) -DME_COM_LINK=$(ME_COM_LINK) -DME_COM_MATRIXSSL=$(ME_COM_MATRIXSSL) -DME_COM_MBEDTLS=$(ME_COM_MBEDTLS) -DME_COM_MPR=$(ME_COM_MPR) -DME_COM_NANOSSL=$(ME_COM_NANOSSL) -DME_COM_OPENSSL=$(ME_COM_OPENSSL) -DME_COM_OSDEP=$(ME_COM_OSDEP) -DME_COM_PCRE=$(ME_COM_PCRE) -DME_COM_SSL=$(ME_COM_SSL) -DME_COM_VXWORKS=$(ME_COM_VXWORKS) 
IFLAGS                += "-I$(BUILD)/inc"
LDFLAGS               += '-Wl,-r'
LIBPATHS              += -L$(BUILD)/bin
LIBS                  += -lgcc

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

ME_ROOT_PREFIX        ?= deploy
ME_BASE_PREFIX        ?= $(ME_ROOT_PREFIX)
ME_DATA_PREFIX        ?= $(ME_VAPP_PREFIX)
ME_STATE_PREFIX       ?= $(ME_VAPP_PREFIX)
ME_BIN_PREFIX         ?= $(ME_VAPP_PREFIX)
ME_INC_PREFIX         ?= $(ME_VAPP_PREFIX)/inc
ME_LIB_PREFIX         ?= $(ME_VAPP_PREFIX)
ME_MAN_PREFIX         ?= $(ME_VAPP_PREFIX)
ME_SBIN_PREFIX        ?= $(ME_VAPP_PREFIX)
ME_ETC_PREFIX         ?= $(ME_VAPP_PREFIX)
ME_WEB_PREFIX         ?= $(ME_VAPP_PREFIX)/web
ME_LOG_PREFIX         ?= $(ME_VAPP_PREFIX)
ME_SPOOL_PREFIX       ?= $(ME_VAPP_PREFIX)
ME_CACHE_PREFIX       ?= $(ME_VAPP_PREFIX)
ME_APP_PREFIX         ?= $(ME_BASE_PREFIX)
ME_VAPP_PREFIX        ?= $(ME_APP_PREFIX)
ME_SRC_PREFIX         ?= $(ME_ROOT_PREFIX)/usr/src/$(NAME)-$(VERSION)


TARGETS               += $(BUILD)/bin/http.out
TARGETS               += $(BUILD)/.install-certs-modified
TARGETS               += $(BUILD)/bin/server.out

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
	@if [ "$(WIND_BASE)" = "" ] ; then echo WARNING: WIND_BASE not set. Run wrenv.sh. ; exit 255 ; fi
	@if [ "$(WIND_HOST_TYPE)" = "" ] ; then echo WARNING: WIND_HOST_TYPE not set. Run wrenv.sh. ; exit 255 ; fi
	@if [ "$(WIND_GNU_PATH)" = "" ] ; then echo WARNING: WIND_GNU_PATH not set. Run wrenv.sh. ; exit 255 ; fi
	@[ ! -x $(BUILD)/bin ] && mkdir -p $(BUILD)/bin; true
	@[ ! -x $(BUILD)/inc ] && mkdir -p $(BUILD)/inc; true
	@[ ! -x $(BUILD)/obj ] && mkdir -p $(BUILD)/obj; true
	@[ ! -f $(BUILD)/inc/me.h ] && cp projects/http-vxworks-static-me.h $(BUILD)/inc/me.h ; true
	@if ! diff $(BUILD)/inc/me.h projects/http-vxworks-static-me.h >/dev/null ; then\
		cp projects/http-vxworks-static-me.h $(BUILD)/inc/me.h  ; \
	fi; true
	@if [ -f "$(BUILD)/.makeflags" ] ; then \
		if [ "$(MAKEFLAGS)" != "`cat $(BUILD)/.makeflags`" ] ; then \
			echo "   [Warning] Make flags have changed since the last build" ; \
			echo "   [Warning] Previous build command: "`cat $(BUILD)/.makeflags`"" ; \
		fi ; \
	fi
	@echo "$(MAKEFLAGS)" >$(BUILD)/.makeflags

clean:
	rm -f "$(BUILD)/obj/actionHandler.o"
	rm -f "$(BUILD)/obj/auth.o"
	rm -f "$(BUILD)/obj/basic.o"
	rm -f "$(BUILD)/obj/cache.o"
	rm -f "$(BUILD)/obj/chunkFilter.o"
	rm -f "$(BUILD)/obj/client.o"
	rm -f "$(BUILD)/obj/config.o"
	rm -f "$(BUILD)/obj/digest.o"
	rm -f "$(BUILD)/obj/dirHandler.o"
	rm -f "$(BUILD)/obj/endpoint.o"
	rm -f "$(BUILD)/obj/error.o"
	rm -f "$(BUILD)/obj/fileHandler.o"
	rm -f "$(BUILD)/obj/host.o"
	rm -f "$(BUILD)/obj/hpack.o"
	rm -f "$(BUILD)/obj/http.o"
	rm -f "$(BUILD)/obj/http1Filter.o"
	rm -f "$(BUILD)/obj/http2Filter.o"
	rm -f "$(BUILD)/obj/huff.o"
	rm -f "$(BUILD)/obj/mbedtls.o"
	rm -f "$(BUILD)/obj/monitor.o"
	rm -f "$(BUILD)/obj/mpr-mbedtls.o"
	rm -f "$(BUILD)/obj/mpr-openssl.o"
	rm -f "$(BUILD)/obj/mprLib.o"
	rm -f "$(BUILD)/obj/net.o"
	rm -f "$(BUILD)/obj/netConnector.o"
	rm -f "$(BUILD)/obj/packet.o"
	rm -f "$(BUILD)/obj/pam.o"
	rm -f "$(BUILD)/obj/passHandler.o"
	rm -f "$(BUILD)/obj/pcre.o"
	rm -f "$(BUILD)/obj/pipeline.o"
	rm -f "$(BUILD)/obj/process.o"
	rm -f "$(BUILD)/obj/queue.o"
	rm -f "$(BUILD)/obj/rangeFilter.o"
	rm -f "$(BUILD)/obj/route.o"
	rm -f "$(BUILD)/obj/rx.o"
	rm -f "$(BUILD)/obj/server.o"
	rm -f "$(BUILD)/obj/service.o"
	rm -f "$(BUILD)/obj/session.o"
	rm -f "$(BUILD)/obj/stage.o"
	rm -f "$(BUILD)/obj/stream.o"
	rm -f "$(BUILD)/obj/tailFilter.o"
	rm -f "$(BUILD)/obj/trace.o"
	rm -f "$(BUILD)/obj/tx.o"
	rm -f "$(BUILD)/obj/uploadFilter.o"
	rm -f "$(BUILD)/obj/uri.o"
	rm -f "$(BUILD)/obj/user.o"
	rm -f "$(BUILD)/obj/var.o"
	rm -f "$(BUILD)/obj/webSockFilter.o"
	rm -f "$(BUILD)/bin/http.out"
	rm -f "$(BUILD)/.install-certs-modified"
	rm -f "$(BUILD)/bin/libhttp.a"
	rm -f "$(BUILD)/bin/libmbedtls.a"
	rm -f "$(BUILD)/bin/libmpr.a"
	rm -f "$(BUILD)/bin/libmpr-mbedtls.a"
	rm -f "$(BUILD)/bin/libpcre.a"
	rm -f "$(BUILD)/bin/server.out"

clobber: clean
	rm -fr ./$(BUILD)

#
#   embedtls.h
#
DEPS_1 += src/mbedtls/embedtls.h

$(BUILD)/inc/embedtls.h: $(DEPS_1)
	@echo '      [Copy] $(BUILD)/inc/embedtls.h'
	mkdir -p "$(BUILD)/inc"
	cp src/mbedtls/embedtls.h $(BUILD)/inc/embedtls.h

#
#   http.h
#
DEPS_2 += src/http.h

$(BUILD)/inc/http.h: $(DEPS_2)
	@echo '      [Copy] $(BUILD)/inc/http.h'
	mkdir -p "$(BUILD)/inc"
	cp src/http.h $(BUILD)/inc/http.h

#
#   mbedtls.h
#
DEPS_3 += src/mbedtls/mbedtls.h

$(BUILD)/inc/mbedtls.h: $(DEPS_3)
	@echo '      [Copy] $(BUILD)/inc/mbedtls.h'
	mkdir -p "$(BUILD)/inc"
	cp src/mbedtls/mbedtls.h $(BUILD)/inc/mbedtls.h

#
#   me.h
#

$(BUILD)/inc/me.h: $(DEPS_4)

#
#   osdep.h
#
DEPS_5 += src/osdep/osdep.h
DEPS_5 += $(BUILD)/inc/me.h

$(BUILD)/inc/osdep.h: $(DEPS_5)
	@echo '      [Copy] $(BUILD)/inc/osdep.h'
	mkdir -p "$(BUILD)/inc"
	cp src/osdep/osdep.h $(BUILD)/inc/osdep.h

#
#   mpr.h
#
DEPS_6 += src/mpr/mpr.h
DEPS_6 += $(BUILD)/inc/me.h
DEPS_6 += $(BUILD)/inc/osdep.h

$(BUILD)/inc/mpr.h: $(DEPS_6)
	@echo '      [Copy] $(BUILD)/inc/mpr.h'
	mkdir -p "$(BUILD)/inc"
	cp src/mpr/mpr.h $(BUILD)/inc/mpr.h

#
#   pcre.h
#
DEPS_7 += src/pcre/pcre.h

$(BUILD)/inc/pcre.h: $(DEPS_7)
	@echo '      [Copy] $(BUILD)/inc/pcre.h'
	mkdir -p "$(BUILD)/inc"
	cp src/pcre/pcre.h $(BUILD)/inc/pcre.h

#
#   http.h
#

src/http.h: $(DEPS_8)

#
#   actionHandler.o
#
DEPS_9 += src/http.h

$(BUILD)/obj/actionHandler.o: \
    src/actionHandler.c $(DEPS_9)
	@echo '   [Compile] $(BUILD)/obj/actionHandler.o'
	$(CC) -c -o $(BUILD)/obj/actionHandler.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/actionHandler.c

#
#   auth.o
#
DEPS_10 += src/http.h

$(BUILD)/obj/auth.o: \
    src/auth.c $(DEPS_10)
	@echo '   [Compile] $(BUILD)/obj/auth.o'
	$(CC) -c -o $(BUILD)/obj/auth.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/auth.c

#
#   basic.o
#
DEPS_11 += src/http.h

$(BUILD)/obj/basic.o: \
    src/basic.c $(DEPS_11)
	@echo '   [Compile] $(BUILD)/obj/basic.o'
	$(CC) -c -o $(BUILD)/obj/basic.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/basic.c

#
#   cache.o
#
DEPS_12 += src/http.h

$(BUILD)/obj/cache.o: \
    src/cache.c $(DEPS_12)
	@echo '   [Compile] $(BUILD)/obj/cache.o'
	$(CC) -c -o $(BUILD)/obj/cache.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/cache.c

#
#   chunkFilter.o
#
DEPS_13 += src/http.h

$(BUILD)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_13)
	@echo '   [Compile] $(BUILD)/obj/chunkFilter.o'
	$(CC) -c -o $(BUILD)/obj/chunkFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/chunkFilter.c

#
#   client.o
#
DEPS_14 += src/http.h

$(BUILD)/obj/client.o: \
    src/client.c $(DEPS_14)
	@echo '   [Compile] $(BUILD)/obj/client.o'
	$(CC) -c -o $(BUILD)/obj/client.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/client.c

#
#   config.o
#
DEPS_15 += src/http.h

$(BUILD)/obj/config.o: \
    src/config.c $(DEPS_15)
	@echo '   [Compile] $(BUILD)/obj/config.o'
	$(CC) -c -o $(BUILD)/obj/config.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/config.c

#
#   digest.o
#
DEPS_16 += src/http.h

$(BUILD)/obj/digest.o: \
    src/digest.c $(DEPS_16)
	@echo '   [Compile] $(BUILD)/obj/digest.o'
	$(CC) -c -o $(BUILD)/obj/digest.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/digest.c

#
#   dirHandler.o
#
DEPS_17 += src/http.h

$(BUILD)/obj/dirHandler.o: \
    src/dirHandler.c $(DEPS_17)
	@echo '   [Compile] $(BUILD)/obj/dirHandler.o'
	$(CC) -c -o $(BUILD)/obj/dirHandler.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/dirHandler.c

#
#   endpoint.o
#
DEPS_18 += src/http.h
DEPS_18 += $(BUILD)/inc/pcre.h

$(BUILD)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_18)
	@echo '   [Compile] $(BUILD)/obj/endpoint.o'
	$(CC) -c -o $(BUILD)/obj/endpoint.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/endpoint.c

#
#   error.o
#
DEPS_19 += src/http.h

$(BUILD)/obj/error.o: \
    src/error.c $(DEPS_19)
	@echo '   [Compile] $(BUILD)/obj/error.o'
	$(CC) -c -o $(BUILD)/obj/error.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/error.c

#
#   fileHandler.o
#
DEPS_20 += src/http.h

$(BUILD)/obj/fileHandler.o: \
    src/fileHandler.c $(DEPS_20)
	@echo '   [Compile] $(BUILD)/obj/fileHandler.o'
	$(CC) -c -o $(BUILD)/obj/fileHandler.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/fileHandler.c

#
#   host.o
#
DEPS_21 += src/http.h
DEPS_21 += $(BUILD)/inc/pcre.h

$(BUILD)/obj/host.o: \
    src/host.c $(DEPS_21)
	@echo '   [Compile] $(BUILD)/obj/host.o'
	$(CC) -c -o $(BUILD)/obj/host.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/host.c

#
#   hpack.o
#
DEPS_22 += src/http.h

$(BUILD)/obj/hpack.o: \
    src/hpack.c $(DEPS_22)
	@echo '   [Compile] $(BUILD)/obj/hpack.o'
	$(CC) -c -o $(BUILD)/obj/hpack.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/hpack.c

#
#   http.o
#
DEPS_23 += src/http.h

$(BUILD)/obj/http.o: \
    src/http.c $(DEPS_23)
	@echo '   [Compile] $(BUILD)/obj/http.o'
	$(CC) -c -o $(BUILD)/obj/http.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/http.c

#
#   http1Filter.o
#
DEPS_24 += src/http.h

$(BUILD)/obj/http1Filter.o: \
    src/http1Filter.c $(DEPS_24)
	@echo '   [Compile] $(BUILD)/obj/http1Filter.o'
	$(CC) -c -o $(BUILD)/obj/http1Filter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/http1Filter.c

#
#   http2Filter.o
#
DEPS_25 += src/http.h

$(BUILD)/obj/http2Filter.o: \
    src/http2Filter.c $(DEPS_25)
	@echo '   [Compile] $(BUILD)/obj/http2Filter.o'
	$(CC) -c -o $(BUILD)/obj/http2Filter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/http2Filter.c

#
#   huff.o
#
DEPS_26 += src/http.h

$(BUILD)/obj/huff.o: \
    src/huff.c $(DEPS_26)
	@echo '   [Compile] $(BUILD)/obj/huff.o'
	$(CC) -c -o $(BUILD)/obj/huff.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/huff.c

#
#   mbedtls.h
#

src/mbedtls/mbedtls.h: $(DEPS_27)

#
#   mbedtls.o
#
DEPS_28 += src/mbedtls/mbedtls.h

$(BUILD)/obj/mbedtls.o: \
    src/mbedtls/mbedtls.c $(DEPS_28)
	@echo '   [Compile] $(BUILD)/obj/mbedtls.o'
	$(CC) -c -o $(BUILD)/obj/mbedtls.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/mbedtls/mbedtls.c

#
#   monitor.o
#
DEPS_29 += src/http.h

$(BUILD)/obj/monitor.o: \
    src/monitor.c $(DEPS_29)
	@echo '   [Compile] $(BUILD)/obj/monitor.o'
	$(CC) -c -o $(BUILD)/obj/monitor.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/monitor.c

#
#   mpr-mbedtls.o
#
DEPS_30 += $(BUILD)/inc/mpr.h

$(BUILD)/obj/mpr-mbedtls.o: \
    src/mpr-mbedtls/mpr-mbedtls.c $(DEPS_30)
	@echo '   [Compile] $(BUILD)/obj/mpr-mbedtls.o'
	$(CC) -c -o $(BUILD)/obj/mpr-mbedtls.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/mpr-mbedtls/mpr-mbedtls.c

#
#   mpr-openssl.o
#
DEPS_31 += $(BUILD)/inc/mpr.h

$(BUILD)/obj/mpr-openssl.o: \
    src/mpr-openssl/mpr-openssl.c $(DEPS_31)
	@echo '   [Compile] $(BUILD)/obj/mpr-openssl.o'
	$(CC) -c -o $(BUILD)/obj/mpr-openssl.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" $(IFLAGS) "-I$(BUILD)/inc" "-I$(ME_COM_OPENSSL_PATH)/include" src/mpr-openssl/mpr-openssl.c

#
#   mpr.h
#

src/mpr/mpr.h: $(DEPS_32)

#
#   mprLib.o
#
DEPS_33 += src/mpr/mpr.h

$(BUILD)/obj/mprLib.o: \
    src/mpr/mprLib.c $(DEPS_33)
	@echo '   [Compile] $(BUILD)/obj/mprLib.o'
	$(CC) -c -o $(BUILD)/obj/mprLib.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/mpr/mprLib.c

#
#   net.o
#
DEPS_34 += src/http.h

$(BUILD)/obj/net.o: \
    src/net.c $(DEPS_34)
	@echo '   [Compile] $(BUILD)/obj/net.o'
	$(CC) -c -o $(BUILD)/obj/net.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/net.c

#
#   netConnector.o
#
DEPS_35 += src/http.h

$(BUILD)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_35)
	@echo '   [Compile] $(BUILD)/obj/netConnector.o'
	$(CC) -c -o $(BUILD)/obj/netConnector.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/netConnector.c

#
#   packet.o
#
DEPS_36 += src/http.h

$(BUILD)/obj/packet.o: \
    src/packet.c $(DEPS_36)
	@echo '   [Compile] $(BUILD)/obj/packet.o'
	$(CC) -c -o $(BUILD)/obj/packet.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/packet.c

#
#   pam.o
#
DEPS_37 += src/http.h

$(BUILD)/obj/pam.o: \
    src/pam.c $(DEPS_37)
	@echo '   [Compile] $(BUILD)/obj/pam.o'
	$(CC) -c -o $(BUILD)/obj/pam.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/pam.c

#
#   passHandler.o
#
DEPS_38 += src/http.h

$(BUILD)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_38)
	@echo '   [Compile] $(BUILD)/obj/passHandler.o'
	$(CC) -c -o $(BUILD)/obj/passHandler.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/passHandler.c

#
#   pcre.h
#

src/pcre/pcre.h: $(DEPS_39)

#
#   pcre.o
#
DEPS_40 += $(BUILD)/inc/me.h
DEPS_40 += src/pcre/pcre.h

$(BUILD)/obj/pcre.o: \
    src/pcre/pcre.c $(DEPS_40)
	@echo '   [Compile] $(BUILD)/obj/pcre.o'
	$(CC) -c -o $(BUILD)/obj/pcre.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" $(IFLAGS) src/pcre/pcre.c

#
#   pipeline.o
#
DEPS_41 += src/http.h

$(BUILD)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_41)
	@echo '   [Compile] $(BUILD)/obj/pipeline.o'
	$(CC) -c -o $(BUILD)/obj/pipeline.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/pipeline.c

#
#   process.o
#
DEPS_42 += src/http.h

$(BUILD)/obj/process.o: \
    src/process.c $(DEPS_42)
	@echo '   [Compile] $(BUILD)/obj/process.o'
	$(CC) -c -o $(BUILD)/obj/process.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/process.c

#
#   queue.o
#
DEPS_43 += src/http.h

$(BUILD)/obj/queue.o: \
    src/queue.c $(DEPS_43)
	@echo '   [Compile] $(BUILD)/obj/queue.o'
	$(CC) -c -o $(BUILD)/obj/queue.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/queue.c

#
#   rangeFilter.o
#
DEPS_44 += src/http.h

$(BUILD)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_44)
	@echo '   [Compile] $(BUILD)/obj/rangeFilter.o'
	$(CC) -c -o $(BUILD)/obj/rangeFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/rangeFilter.c

#
#   route.o
#
DEPS_45 += src/http.h
DEPS_45 += $(BUILD)/inc/pcre.h

$(BUILD)/obj/route.o: \
    src/route.c $(DEPS_45)
	@echo '   [Compile] $(BUILD)/obj/route.o'
	$(CC) -c -o $(BUILD)/obj/route.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/route.c

#
#   rx.o
#
DEPS_46 += src/http.h

$(BUILD)/obj/rx.o: \
    src/rx.c $(DEPS_46)
	@echo '   [Compile] $(BUILD)/obj/rx.o'
	$(CC) -c -o $(BUILD)/obj/rx.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/rx.c

#
#   server.o
#
DEPS_47 += src/http.h

$(BUILD)/obj/server.o: \
    src/server.c $(DEPS_47)
	@echo '   [Compile] $(BUILD)/obj/server.o'
	$(CC) -c -o $(BUILD)/obj/server.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/server.c

#
#   service.o
#
DEPS_48 += src/http.h

$(BUILD)/obj/service.o: \
    src/service.c $(DEPS_48)
	@echo '   [Compile] $(BUILD)/obj/service.o'
	$(CC) -c -o $(BUILD)/obj/service.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/service.c

#
#   session.o
#
DEPS_49 += src/http.h

$(BUILD)/obj/session.o: \
    src/session.c $(DEPS_49)
	@echo '   [Compile] $(BUILD)/obj/session.o'
	$(CC) -c -o $(BUILD)/obj/session.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/session.c

#
#   stage.o
#
DEPS_50 += src/http.h

$(BUILD)/obj/stage.o: \
    src/stage.c $(DEPS_50)
	@echo '   [Compile] $(BUILD)/obj/stage.o'
	$(CC) -c -o $(BUILD)/obj/stage.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/stage.c

#
#   stream.o
#
DEPS_51 += src/http.h

$(BUILD)/obj/stream.o: \
    src/stream.c $(DEPS_51)
	@echo '   [Compile] $(BUILD)/obj/stream.o'
	$(CC) -c -o $(BUILD)/obj/stream.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/stream.c

#
#   tailFilter.o
#
DEPS_52 += src/http.h

$(BUILD)/obj/tailFilter.o: \
    src/tailFilter.c $(DEPS_52)
	@echo '   [Compile] $(BUILD)/obj/tailFilter.o'
	$(CC) -c -o $(BUILD)/obj/tailFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/tailFilter.c

#
#   trace.o
#
DEPS_53 += src/http.h

$(BUILD)/obj/trace.o: \
    src/trace.c $(DEPS_53)
	@echo '   [Compile] $(BUILD)/obj/trace.o'
	$(CC) -c -o $(BUILD)/obj/trace.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/trace.c

#
#   tx.o
#
DEPS_54 += src/http.h

$(BUILD)/obj/tx.o: \
    src/tx.c $(DEPS_54)
	@echo '   [Compile] $(BUILD)/obj/tx.o'
	$(CC) -c -o $(BUILD)/obj/tx.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/tx.c

#
#   uploadFilter.o
#
DEPS_55 += src/http.h

$(BUILD)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_55)
	@echo '   [Compile] $(BUILD)/obj/uploadFilter.o'
	$(CC) -c -o $(BUILD)/obj/uploadFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/uploadFilter.c

#
#   uri.o
#
DEPS_56 += src/http.h

$(BUILD)/obj/uri.o: \
    src/uri.c $(DEPS_56)
	@echo '   [Compile] $(BUILD)/obj/uri.o'
	$(CC) -c -o $(BUILD)/obj/uri.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/uri.c

#
#   user.o
#
DEPS_57 += src/http.h

$(BUILD)/obj/user.o: \
    src/user.c $(DEPS_57)
	@echo '   [Compile] $(BUILD)/obj/user.o'
	$(CC) -c -o $(BUILD)/obj/user.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/user.c

#
#   var.o
#
DEPS_58 += src/http.h

$(BUILD)/obj/var.o: \
    src/var.c $(DEPS_58)
	@echo '   [Compile] $(BUILD)/obj/var.o'
	$(CC) -c -o $(BUILD)/obj/var.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/var.c

#
#   webSockFilter.o
#
DEPS_59 += src/http.h

$(BUILD)/obj/webSockFilter.o: \
    src/webSockFilter.c $(DEPS_59)
	@echo '   [Compile] $(BUILD)/obj/webSockFilter.o'
	$(CC) -c -o $(BUILD)/obj/webSockFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" -DME_COM_OPENSSL_PATH=$(ME_COM_OPENSSL_PATH) $(IFLAGS) "-I$(ME_COM_OPENSSL_PATH)/include" src/webSockFilter.c

ifeq ($(ME_COM_MBEDTLS),1)
#
#   libmbedtls
#
DEPS_60 += $(BUILD)/inc/osdep.h
DEPS_60 += $(BUILD)/inc/embedtls.h
DEPS_60 += $(BUILD)/inc/mbedtls.h
DEPS_60 += $(BUILD)/obj/mbedtls.o

$(BUILD)/bin/libmbedtls.a: $(DEPS_60)
	@echo '      [Link] $(BUILD)/bin/libmbedtls.a'
	$(AR) -cr $(BUILD)/bin/libmbedtls.a "$(BUILD)/obj/mbedtls.o"
endif

ifeq ($(ME_COM_MBEDTLS),1)
#
#   libmpr-mbedtls
#
DEPS_61 += $(BUILD)/bin/libmbedtls.a
DEPS_61 += $(BUILD)/obj/mpr-mbedtls.o

$(BUILD)/bin/libmpr-mbedtls.a: $(DEPS_61)
	@echo '      [Link] $(BUILD)/bin/libmpr-mbedtls.a'
	$(AR) -cr $(BUILD)/bin/libmpr-mbedtls.a "$(BUILD)/obj/mpr-mbedtls.o"
endif

ifeq ($(ME_COM_OPENSSL),1)
#
#   libmpr-openssl
#
DEPS_62 += $(BUILD)/obj/mpr-openssl.o

$(BUILD)/bin/libmpr-openssl.a: $(DEPS_62)
	@echo '      [Link] $(BUILD)/bin/libmpr-openssl.a'
	$(AR) -cr $(BUILD)/bin/libmpr-openssl.a "$(BUILD)/obj/mpr-openssl.o"
endif

#
#   libmpr
#
DEPS_63 += $(BUILD)/inc/osdep.h
ifeq ($(ME_COM_MBEDTLS),1)
    DEPS_63 += $(BUILD)/bin/libmpr-mbedtls.a
endif
ifeq ($(ME_COM_MBEDTLS),1)
    DEPS_63 += $(BUILD)/bin/libmbedtls.a
endif
ifeq ($(ME_COM_OPENSSL),1)
    DEPS_63 += $(BUILD)/bin/libmpr-openssl.a
endif
DEPS_63 += $(BUILD)/inc/mpr.h
DEPS_63 += $(BUILD)/obj/mprLib.o

$(BUILD)/bin/libmpr.a: $(DEPS_63)
	@echo '      [Link] $(BUILD)/bin/libmpr.a'
	$(AR) -cr $(BUILD)/bin/libmpr.a "$(BUILD)/obj/mprLib.o"

ifeq ($(ME_COM_PCRE),1)
#
#   libpcre
#
DEPS_64 += $(BUILD)/inc/pcre.h
DEPS_64 += $(BUILD)/obj/pcre.o

$(BUILD)/bin/libpcre.a: $(DEPS_64)
	@echo '      [Link] $(BUILD)/bin/libpcre.a'
	$(AR) -cr $(BUILD)/bin/libpcre.a "$(BUILD)/obj/pcre.o"
endif

#
#   libhttp
#
DEPS_65 += $(BUILD)/bin/libmpr.a
ifeq ($(ME_COM_PCRE),1)
    DEPS_65 += $(BUILD)/bin/libpcre.a
endif
DEPS_65 += $(BUILD)/inc/http.h
DEPS_65 += $(BUILD)/obj/actionHandler.o
DEPS_65 += $(BUILD)/obj/auth.o
DEPS_65 += $(BUILD)/obj/basic.o
DEPS_65 += $(BUILD)/obj/cache.o
DEPS_65 += $(BUILD)/obj/chunkFilter.o
DEPS_65 += $(BUILD)/obj/client.o
DEPS_65 += $(BUILD)/obj/config.o
DEPS_65 += $(BUILD)/obj/digest.o
DEPS_65 += $(BUILD)/obj/dirHandler.o
DEPS_65 += $(BUILD)/obj/endpoint.o
DEPS_65 += $(BUILD)/obj/error.o
DEPS_65 += $(BUILD)/obj/fileHandler.o
DEPS_65 += $(BUILD)/obj/host.o
DEPS_65 += $(BUILD)/obj/hpack.o
DEPS_65 += $(BUILD)/obj/http1Filter.o
DEPS_65 += $(BUILD)/obj/http2Filter.o
DEPS_65 += $(BUILD)/obj/huff.o
DEPS_65 += $(BUILD)/obj/monitor.o
DEPS_65 += $(BUILD)/obj/net.o
DEPS_65 += $(BUILD)/obj/netConnector.o
DEPS_65 += $(BUILD)/obj/packet.o
DEPS_65 += $(BUILD)/obj/pam.o
DEPS_65 += $(BUILD)/obj/passHandler.o
DEPS_65 += $(BUILD)/obj/pipeline.o
DEPS_65 += $(BUILD)/obj/process.o
DEPS_65 += $(BUILD)/obj/queue.o
DEPS_65 += $(BUILD)/obj/rangeFilter.o
DEPS_65 += $(BUILD)/obj/route.o
DEPS_65 += $(BUILD)/obj/rx.o
DEPS_65 += $(BUILD)/obj/server.o
DEPS_65 += $(BUILD)/obj/service.o
DEPS_65 += $(BUILD)/obj/session.o
DEPS_65 += $(BUILD)/obj/stage.o
DEPS_65 += $(BUILD)/obj/stream.o
DEPS_65 += $(BUILD)/obj/tailFilter.o
DEPS_65 += $(BUILD)/obj/trace.o
DEPS_65 += $(BUILD)/obj/tx.o
DEPS_65 += $(BUILD)/obj/uploadFilter.o
DEPS_65 += $(BUILD)/obj/uri.o
DEPS_65 += $(BUILD)/obj/user.o
DEPS_65 += $(BUILD)/obj/var.o
DEPS_65 += $(BUILD)/obj/webSockFilter.o

$(BUILD)/bin/libhttp.a: $(DEPS_65)
	@echo '      [Link] $(BUILD)/bin/libhttp.a'
	$(AR) -cr $(BUILD)/bin/libhttp.a "$(BUILD)/obj/actionHandler.o" "$(BUILD)/obj/auth.o" "$(BUILD)/obj/basic.o" "$(BUILD)/obj/cache.o" "$(BUILD)/obj/chunkFilter.o" "$(BUILD)/obj/client.o" "$(BUILD)/obj/config.o" "$(BUILD)/obj/digest.o" "$(BUILD)/obj/dirHandler.o" "$(BUILD)/obj/endpoint.o" "$(BUILD)/obj/error.o" "$(BUILD)/obj/fileHandler.o" "$(BUILD)/obj/host.o" "$(BUILD)/obj/hpack.o" "$(BUILD)/obj/http1Filter.o" "$(BUILD)/obj/http2Filter.o" "$(BUILD)/obj/huff.o" "$(BUILD)/obj/monitor.o" "$(BUILD)/obj/net.o" "$(BUILD)/obj/netConnector.o" "$(BUILD)/obj/packet.o" "$(BUILD)/obj/pam.o" "$(BUILD)/obj/passHandler.o" "$(BUILD)/obj/pipeline.o" "$(BUILD)/obj/process.o" "$(BUILD)/obj/queue.o" "$(BUILD)/obj/rangeFilter.o" "$(BUILD)/obj/route.o" "$(BUILD)/obj/rx.o" "$(BUILD)/obj/server.o" "$(BUILD)/obj/service.o" "$(BUILD)/obj/session.o" "$(BUILD)/obj/stage.o" "$(BUILD)/obj/stream.o" "$(BUILD)/obj/tailFilter.o" "$(BUILD)/obj/trace.o" "$(BUILD)/obj/tx.o" "$(BUILD)/obj/uploadFilter.o" "$(BUILD)/obj/uri.o" "$(BUILD)/obj/user.o" "$(BUILD)/obj/var.o" "$(BUILD)/obj/webSockFilter.o"

#
#   http
#
DEPS_66 += $(BUILD)/bin/libhttp.a
DEPS_66 += $(BUILD)/obj/http.o

ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_66 += -lmbedtls
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_66 += -lmpr-mbedtls
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_66 += -lmbedtls
endif
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_66 += -lmpr-openssl
endif
ifeq ($(ME_COM_OPENSSL),1)
ifeq ($(ME_COM_SSL),1)
    LIBS_66 += -lssl
    LIBPATHS_66 += -L"$(ME_COM_OPENSSL_PATH)"
endif
endif
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_66 += -lcrypto
    LIBPATHS_66 += -L"$(ME_COM_OPENSSL_PATH)"
endif
LIBS_66 += -lmpr
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_66 += -lmpr-openssl
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_66 += -lmpr-mbedtls
endif
ifeq ($(ME_COM_PCRE),1)
    LIBS_66 += -lpcre
endif
LIBS_66 += -lhttp
ifeq ($(ME_COM_PCRE),1)
    LIBS_66 += -lpcre
endif
LIBS_66 += -lmpr

$(BUILD)/bin/http.out: $(DEPS_66)
	@echo '      [Link] $(BUILD)/bin/http.out'
	$(CC) -o $(BUILD)/bin/http.out $(LDFLAGS) $(LIBPATHS)  "$(BUILD)/obj/http.o" $(LIBPATHS_66) $(LIBS_66) $(LIBS_66) $(LIBS) -Wl,-r 

#
#   install-certs
#
DEPS_67 += src/certs/samples/ca.crt
DEPS_67 += src/certs/samples/ca.key
DEPS_67 += src/certs/samples/ec.crt
DEPS_67 += src/certs/samples/ec.key
DEPS_67 += src/certs/samples/roots.crt
DEPS_67 += src/certs/samples/self.crt
DEPS_67 += src/certs/samples/self.key
DEPS_67 += src/certs/samples/test.crt
DEPS_67 += src/certs/samples/test.key

$(BUILD)/.install-certs-modified: $(DEPS_67)
	@echo '      [Copy] $(BUILD)/bin'
	mkdir -p "$(BUILD)/bin"
	cp src/certs/samples/ca.crt $(BUILD)/bin/ca.crt
	cp src/certs/samples/ca.key $(BUILD)/bin/ca.key
	cp src/certs/samples/ec.crt $(BUILD)/bin/ec.crt
	cp src/certs/samples/ec.key $(BUILD)/bin/ec.key
	cp src/certs/samples/roots.crt $(BUILD)/bin/roots.crt
	cp src/certs/samples/self.crt $(BUILD)/bin/self.crt
	cp src/certs/samples/self.key $(BUILD)/bin/self.key
	cp src/certs/samples/test.crt $(BUILD)/bin/test.crt
	cp src/certs/samples/test.key $(BUILD)/bin/test.key
	touch "$(BUILD)/.install-certs-modified"

#
#   server
#
DEPS_68 += $(BUILD)/bin/libhttp.a
DEPS_68 += $(BUILD)/obj/server.o

ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_68 += -lmbedtls
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_68 += -lmpr-mbedtls
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_68 += -lmbedtls
endif
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_68 += -lmpr-openssl
endif
ifeq ($(ME_COM_OPENSSL),1)
ifeq ($(ME_COM_SSL),1)
    LIBS_68 += -lssl
    LIBPATHS_68 += -L"$(ME_COM_OPENSSL_PATH)"
endif
endif
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_68 += -lcrypto
    LIBPATHS_68 += -L"$(ME_COM_OPENSSL_PATH)"
endif
LIBS_68 += -lmpr
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_68 += -lmpr-openssl
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_68 += -lmpr-mbedtls
endif
ifeq ($(ME_COM_PCRE),1)
    LIBS_68 += -lpcre
endif
LIBS_68 += -lhttp
ifeq ($(ME_COM_PCRE),1)
    LIBS_68 += -lpcre
endif
LIBS_68 += -lmpr

$(BUILD)/bin/server.out: $(DEPS_68)
	@echo '      [Link] $(BUILD)/bin/server.out'
	$(CC) -o $(BUILD)/bin/server.out $(LDFLAGS) $(LIBPATHS)  "$(BUILD)/obj/server.o" $(LIBPATHS_68) $(LIBS_68) $(LIBS_68) $(LIBS) -Wl,-r 

#
#   installPrep
#

installPrep: $(DEPS_69)
	if [ "`id -u`" != 0 ] ; \
	then echo "Must run as root. Rerun with sudo." ; \
	exit 255 ; \
	fi

#
#   stop
#

stop: $(DEPS_70)

#
#   installBinary
#

installBinary: $(DEPS_71)

#
#   start
#

start: $(DEPS_72)

#
#   install
#
DEPS_73 += installPrep
DEPS_73 += stop
DEPS_73 += installBinary
DEPS_73 += start

install: $(DEPS_73)

#
#   uninstall
#
DEPS_74 += stop

uninstall: $(DEPS_74)

#
#   uninstallBinary
#

uninstallBinary: $(DEPS_75)

#
#   version
#

version: $(DEPS_76)
	echo $(VERSION)

