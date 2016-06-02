#
#   http-vxworks-static.mk -- Makefile to build Embedthis Http for vxworks
#

NAME                  := http
VERSION               := 6.2.3
PROFILE               ?= static
ARCH                  ?= $(shell echo $(WIND_HOST_TYPE) | sed 's/-.*$(ME_ROOT_PREFIX)/')
CPU                   ?= $(subst X86,PENTIUM,$(shell echo $(ARCH) | tr a-z A-Z))
OS                    ?= vxworks
CC                    ?= cc$(subst x86,pentium,$(ARCH))
LD                    ?= ldundefined
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
ME_COM_SSL            ?= 0
ME_COM_VXWORKS        ?= 0

ME_COM_OPENSSL_PATH   ?= "/usr/lib"

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
DFLAGS                += -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h" $(patsubst %,-D%,$(filter ME_%,$(MAKEFLAGS))) -DME_COM_COMPILER=$(ME_COM_COMPILER) -DME_COM_LIB=$(ME_COM_LIB) -DME_COM_LINK=$(ME_COM_LINK) -DME_COM_MATRIXSSL=$(ME_COM_MATRIXSSL) -DME_COM_MBEDTLS=$(ME_COM_MBEDTLS) -DME_COM_MPR=$(ME_COM_MPR) -DME_COM_NANOSSL=$(ME_COM_NANOSSL) -DME_COM_OPENSSL=$(ME_COM_OPENSSL) -DME_COM_OSDEP=$(ME_COM_OSDEP) -DME_COM_PCRE=$(ME_COM_PCRE) -DME_COM_SSL=$(ME_COM_SSL) -DME_COM_VXWORKS=$(ME_COM_VXWORKS) 
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


TARGETS               += $(BUILD)/bin/http-server.out
TARGETS               += $(BUILD)/bin/http.out
TARGETS               += $(BUILD)/.install-certs-modified

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
	rm -f "$(BUILD)/obj/conn.o"
	rm -f "$(BUILD)/obj/digest.o"
	rm -f "$(BUILD)/obj/dirHandler.o"
	rm -f "$(BUILD)/obj/endpoint.o"
	rm -f "$(BUILD)/obj/error.o"
	rm -f "$(BUILD)/obj/fileHandler.o"
	rm -f "$(BUILD)/obj/host.o"
	rm -f "$(BUILD)/obj/http-server.o"
	rm -f "$(BUILD)/obj/http.o"
	rm -f "$(BUILD)/obj/mbedtls.o"
	rm -f "$(BUILD)/obj/monitor.o"
	rm -f "$(BUILD)/obj/mpr-mbedtls.o"
	rm -f "$(BUILD)/obj/mpr-openssl.o"
	rm -f "$(BUILD)/obj/mprLib.o"
	rm -f "$(BUILD)/obj/netConnector.o"
	rm -f "$(BUILD)/obj/packet.o"
	rm -f "$(BUILD)/obj/pam.o"
	rm -f "$(BUILD)/obj/passHandler.o"
	rm -f "$(BUILD)/obj/pcre.o"
	rm -f "$(BUILD)/obj/pipeline.o"
	rm -f "$(BUILD)/obj/queue.o"
	rm -f "$(BUILD)/obj/rangeFilter.o"
	rm -f "$(BUILD)/obj/route.o"
	rm -f "$(BUILD)/obj/rx.o"
	rm -f "$(BUILD)/obj/sendConnector.o"
	rm -f "$(BUILD)/obj/service.o"
	rm -f "$(BUILD)/obj/session.o"
	rm -f "$(BUILD)/obj/stage.o"
	rm -f "$(BUILD)/obj/trace.o"
	rm -f "$(BUILD)/obj/tx.o"
	rm -f "$(BUILD)/obj/uploadFilter.o"
	rm -f "$(BUILD)/obj/uri.o"
	rm -f "$(BUILD)/obj/user.o"
	rm -f "$(BUILD)/obj/var.o"
	rm -f "$(BUILD)/obj/webSockFilter.o"
	rm -f "$(BUILD)/bin/http-server.out"
	rm -f "$(BUILD)/bin/http.out"
	rm -f "$(BUILD)/.install-certs-modified"
	rm -f "$(BUILD)/bin/libhttp.a"
	rm -f "$(BUILD)/bin/libmbedtls.a"
	rm -f "$(BUILD)/bin/libmpr.a"
	rm -f "$(BUILD)/bin/libmpr-mbedtls.a"
	rm -f "$(BUILD)/bin/libpcre.a"

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
#   me.h
#

$(BUILD)/inc/me.h: $(DEPS_2)

#
#   osdep.h
#
DEPS_3 += src/osdep/osdep.h
DEPS_3 += $(BUILD)/inc/me.h

$(BUILD)/inc/osdep.h: $(DEPS_3)
	@echo '      [Copy] $(BUILD)/inc/osdep.h'
	mkdir -p "$(BUILD)/inc"
	cp src/osdep/osdep.h $(BUILD)/inc/osdep.h

#
#   mpr.h
#
DEPS_4 += src/mpr/mpr.h
DEPS_4 += $(BUILD)/inc/me.h
DEPS_4 += $(BUILD)/inc/osdep.h

$(BUILD)/inc/mpr.h: $(DEPS_4)
	@echo '      [Copy] $(BUILD)/inc/mpr.h'
	mkdir -p "$(BUILD)/inc"
	cp src/mpr/mpr.h $(BUILD)/inc/mpr.h

#
#   http.h
#
DEPS_5 += src/http.h
DEPS_5 += $(BUILD)/inc/mpr.h

$(BUILD)/inc/http.h: $(DEPS_5)
	@echo '      [Copy] $(BUILD)/inc/http.h'
	mkdir -p "$(BUILD)/inc"
	cp src/http.h $(BUILD)/inc/http.h

#
#   mbedtls.h
#
DEPS_6 += src/mbedtls/mbedtls.h

$(BUILD)/inc/mbedtls.h: $(DEPS_6)
	@echo '      [Copy] $(BUILD)/inc/mbedtls.h'
	mkdir -p "$(BUILD)/inc"
	cp src/mbedtls/mbedtls.h $(BUILD)/inc/mbedtls.h

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
	$(CC) -c -o $(BUILD)/obj/actionHandler.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/actionHandler.c

#
#   auth.o
#
DEPS_10 += src/http.h

$(BUILD)/obj/auth.o: \
    src/auth.c $(DEPS_10)
	@echo '   [Compile] $(BUILD)/obj/auth.o'
	$(CC) -c -o $(BUILD)/obj/auth.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/auth.c

#
#   basic.o
#
DEPS_11 += src/http.h

$(BUILD)/obj/basic.o: \
    src/basic.c $(DEPS_11)
	@echo '   [Compile] $(BUILD)/obj/basic.o'
	$(CC) -c -o $(BUILD)/obj/basic.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/basic.c

#
#   cache.o
#
DEPS_12 += src/http.h

$(BUILD)/obj/cache.o: \
    src/cache.c $(DEPS_12)
	@echo '   [Compile] $(BUILD)/obj/cache.o'
	$(CC) -c -o $(BUILD)/obj/cache.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/cache.c

#
#   chunkFilter.o
#
DEPS_13 += src/http.h

$(BUILD)/obj/chunkFilter.o: \
    src/chunkFilter.c $(DEPS_13)
	@echo '   [Compile] $(BUILD)/obj/chunkFilter.o'
	$(CC) -c -o $(BUILD)/obj/chunkFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/chunkFilter.c

#
#   client.o
#
DEPS_14 += src/http.h

$(BUILD)/obj/client.o: \
    src/client.c $(DEPS_14)
	@echo '   [Compile] $(BUILD)/obj/client.o'
	$(CC) -c -o $(BUILD)/obj/client.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/client.c

#
#   config.o
#
DEPS_15 += src/http.h

$(BUILD)/obj/config.o: \
    src/config.c $(DEPS_15)
	@echo '   [Compile] $(BUILD)/obj/config.o'
	$(CC) -c -o $(BUILD)/obj/config.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/config.c

#
#   conn.o
#
DEPS_16 += src/http.h

$(BUILD)/obj/conn.o: \
    src/conn.c $(DEPS_16)
	@echo '   [Compile] $(BUILD)/obj/conn.o'
	$(CC) -c -o $(BUILD)/obj/conn.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/conn.c

#
#   digest.o
#
DEPS_17 += src/http.h

$(BUILD)/obj/digest.o: \
    src/digest.c $(DEPS_17)
	@echo '   [Compile] $(BUILD)/obj/digest.o'
	$(CC) -c -o $(BUILD)/obj/digest.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/digest.c

#
#   dirHandler.o
#
DEPS_18 += src/http.h

$(BUILD)/obj/dirHandler.o: \
    src/dirHandler.c $(DEPS_18)
	@echo '   [Compile] $(BUILD)/obj/dirHandler.o'
	$(CC) -c -o $(BUILD)/obj/dirHandler.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/dirHandler.c

#
#   endpoint.o
#
DEPS_19 += src/http.h
DEPS_19 += $(BUILD)/inc/pcre.h

$(BUILD)/obj/endpoint.o: \
    src/endpoint.c $(DEPS_19)
	@echo '   [Compile] $(BUILD)/obj/endpoint.o'
	$(CC) -c -o $(BUILD)/obj/endpoint.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/endpoint.c

#
#   error.o
#
DEPS_20 += src/http.h

$(BUILD)/obj/error.o: \
    src/error.c $(DEPS_20)
	@echo '   [Compile] $(BUILD)/obj/error.o'
	$(CC) -c -o $(BUILD)/obj/error.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/error.c

#
#   fileHandler.o
#
DEPS_21 += src/http.h

$(BUILD)/obj/fileHandler.o: \
    src/fileHandler.c $(DEPS_21)
	@echo '   [Compile] $(BUILD)/obj/fileHandler.o'
	$(CC) -c -o $(BUILD)/obj/fileHandler.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/fileHandler.c

#
#   host.o
#
DEPS_22 += src/http.h
DEPS_22 += $(BUILD)/inc/pcre.h

$(BUILD)/obj/host.o: \
    src/host.c $(DEPS_22)
	@echo '   [Compile] $(BUILD)/obj/host.o'
	$(CC) -c -o $(BUILD)/obj/host.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/host.c

#
#   http-server.o
#
DEPS_23 += $(BUILD)/inc/http.h

$(BUILD)/obj/http-server.o: \
    test/http-server.c $(DEPS_23)
	@echo '   [Compile] $(BUILD)/obj/http-server.o'
	$(CC) -c -o $(BUILD)/obj/http-server.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) test/http-server.c

#
#   http.o
#
DEPS_24 += src/http.h

$(BUILD)/obj/http.o: \
    src/http.c $(DEPS_24)
	@echo '   [Compile] $(BUILD)/obj/http.o'
	$(CC) -c -o $(BUILD)/obj/http.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/http.c

#
#   mbedtls.h
#

src/mbedtls/mbedtls.h: $(DEPS_25)

#
#   mbedtls.o
#
DEPS_26 += src/mbedtls/mbedtls.h

$(BUILD)/obj/mbedtls.o: \
    src/mbedtls/mbedtls.c $(DEPS_26)
	@echo '   [Compile] $(BUILD)/obj/mbedtls.o'
	$(CC) -c -o $(BUILD)/obj/mbedtls.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/mbedtls/mbedtls.c

#
#   monitor.o
#
DEPS_27 += src/http.h

$(BUILD)/obj/monitor.o: \
    src/monitor.c $(DEPS_27)
	@echo '   [Compile] $(BUILD)/obj/monitor.o'
	$(CC) -c -o $(BUILD)/obj/monitor.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/monitor.c

#
#   mpr-mbedtls.o
#
DEPS_28 += $(BUILD)/inc/mpr.h

$(BUILD)/obj/mpr-mbedtls.o: \
    src/mpr-mbedtls/mpr-mbedtls.c $(DEPS_28)
	@echo '   [Compile] $(BUILD)/obj/mpr-mbedtls.o'
	$(CC) -c -o $(BUILD)/obj/mpr-mbedtls.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/mpr-mbedtls/mpr-mbedtls.c

#
#   mpr-openssl.o
#
DEPS_29 += $(BUILD)/inc/mpr.h

$(BUILD)/obj/mpr-openssl.o: \
    src/mpr-openssl/mpr-openssl.c $(DEPS_29)
	@echo '   [Compile] $(BUILD)/obj/mpr-openssl.o'
	$(CC) -c -o $(BUILD)/obj/mpr-openssl.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" $(IFLAGS) src/mpr-openssl/mpr-openssl.c

#
#   mpr.h
#

src/mpr/mpr.h: $(DEPS_30)

#
#   mprLib.o
#
DEPS_31 += src/mpr/mpr.h

$(BUILD)/obj/mprLib.o: \
    src/mpr/mprLib.c $(DEPS_31)
	@echo '   [Compile] $(BUILD)/obj/mprLib.o'
	$(CC) -c -o $(BUILD)/obj/mprLib.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/mpr/mprLib.c

#
#   netConnector.o
#
DEPS_32 += src/http.h

$(BUILD)/obj/netConnector.o: \
    src/netConnector.c $(DEPS_32)
	@echo '   [Compile] $(BUILD)/obj/netConnector.o'
	$(CC) -c -o $(BUILD)/obj/netConnector.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/netConnector.c

#
#   packet.o
#
DEPS_33 += src/http.h

$(BUILD)/obj/packet.o: \
    src/packet.c $(DEPS_33)
	@echo '   [Compile] $(BUILD)/obj/packet.o'
	$(CC) -c -o $(BUILD)/obj/packet.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/packet.c

#
#   pam.o
#
DEPS_34 += src/http.h

$(BUILD)/obj/pam.o: \
    src/pam.c $(DEPS_34)
	@echo '   [Compile] $(BUILD)/obj/pam.o'
	$(CC) -c -o $(BUILD)/obj/pam.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/pam.c

#
#   passHandler.o
#
DEPS_35 += src/http.h

$(BUILD)/obj/passHandler.o: \
    src/passHandler.c $(DEPS_35)
	@echo '   [Compile] $(BUILD)/obj/passHandler.o'
	$(CC) -c -o $(BUILD)/obj/passHandler.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/passHandler.c

#
#   pcre.h
#

src/pcre/pcre.h: $(DEPS_36)

#
#   pcre.o
#
DEPS_37 += $(BUILD)/inc/me.h
DEPS_37 += src/pcre/pcre.h

$(BUILD)/obj/pcre.o: \
    src/pcre/pcre.c $(DEPS_37)
	@echo '   [Compile] $(BUILD)/obj/pcre.o'
	$(CC) -c -o $(BUILD)/obj/pcre.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" $(IFLAGS) src/pcre/pcre.c

#
#   pipeline.o
#
DEPS_38 += src/http.h

$(BUILD)/obj/pipeline.o: \
    src/pipeline.c $(DEPS_38)
	@echo '   [Compile] $(BUILD)/obj/pipeline.o'
	$(CC) -c -o $(BUILD)/obj/pipeline.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/pipeline.c

#
#   queue.o
#
DEPS_39 += src/http.h

$(BUILD)/obj/queue.o: \
    src/queue.c $(DEPS_39)
	@echo '   [Compile] $(BUILD)/obj/queue.o'
	$(CC) -c -o $(BUILD)/obj/queue.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/queue.c

#
#   rangeFilter.o
#
DEPS_40 += src/http.h

$(BUILD)/obj/rangeFilter.o: \
    src/rangeFilter.c $(DEPS_40)
	@echo '   [Compile] $(BUILD)/obj/rangeFilter.o'
	$(CC) -c -o $(BUILD)/obj/rangeFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/rangeFilter.c

#
#   route.o
#
DEPS_41 += src/http.h
DEPS_41 += $(BUILD)/inc/pcre.h

$(BUILD)/obj/route.o: \
    src/route.c $(DEPS_41)
	@echo '   [Compile] $(BUILD)/obj/route.o'
	$(CC) -c -o $(BUILD)/obj/route.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/route.c

#
#   rx.o
#
DEPS_42 += src/http.h

$(BUILD)/obj/rx.o: \
    src/rx.c $(DEPS_42)
	@echo '   [Compile] $(BUILD)/obj/rx.o'
	$(CC) -c -o $(BUILD)/obj/rx.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/rx.c

#
#   sendConnector.o
#
DEPS_43 += src/http.h

$(BUILD)/obj/sendConnector.o: \
    src/sendConnector.c $(DEPS_43)
	@echo '   [Compile] $(BUILD)/obj/sendConnector.o'
	$(CC) -c -o $(BUILD)/obj/sendConnector.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/sendConnector.c

#
#   service.o
#
DEPS_44 += src/http.h

$(BUILD)/obj/service.o: \
    src/service.c $(DEPS_44)
	@echo '   [Compile] $(BUILD)/obj/service.o'
	$(CC) -c -o $(BUILD)/obj/service.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/service.c

#
#   session.o
#
DEPS_45 += src/http.h

$(BUILD)/obj/session.o: \
    src/session.c $(DEPS_45)
	@echo '   [Compile] $(BUILD)/obj/session.o'
	$(CC) -c -o $(BUILD)/obj/session.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/session.c

#
#   stage.o
#
DEPS_46 += src/http.h

$(BUILD)/obj/stage.o: \
    src/stage.c $(DEPS_46)
	@echo '   [Compile] $(BUILD)/obj/stage.o'
	$(CC) -c -o $(BUILD)/obj/stage.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/stage.c

#
#   trace.o
#
DEPS_47 += src/http.h

$(BUILD)/obj/trace.o: \
    src/trace.c $(DEPS_47)
	@echo '   [Compile] $(BUILD)/obj/trace.o'
	$(CC) -c -o $(BUILD)/obj/trace.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/trace.c

#
#   tx.o
#
DEPS_48 += src/http.h

$(BUILD)/obj/tx.o: \
    src/tx.c $(DEPS_48)
	@echo '   [Compile] $(BUILD)/obj/tx.o'
	$(CC) -c -o $(BUILD)/obj/tx.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/tx.c

#
#   uploadFilter.o
#
DEPS_49 += src/http.h

$(BUILD)/obj/uploadFilter.o: \
    src/uploadFilter.c $(DEPS_49)
	@echo '   [Compile] $(BUILD)/obj/uploadFilter.o'
	$(CC) -c -o $(BUILD)/obj/uploadFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/uploadFilter.c

#
#   uri.o
#
DEPS_50 += src/http.h

$(BUILD)/obj/uri.o: \
    src/uri.c $(DEPS_50)
	@echo '   [Compile] $(BUILD)/obj/uri.o'
	$(CC) -c -o $(BUILD)/obj/uri.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/uri.c

#
#   user.o
#
DEPS_51 += src/http.h

$(BUILD)/obj/user.o: \
    src/user.c $(DEPS_51)
	@echo '   [Compile] $(BUILD)/obj/user.o'
	$(CC) -c -o $(BUILD)/obj/user.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/user.c

#
#   var.o
#
DEPS_52 += src/http.h

$(BUILD)/obj/var.o: \
    src/var.c $(DEPS_52)
	@echo '   [Compile] $(BUILD)/obj/var.o'
	$(CC) -c -o $(BUILD)/obj/var.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/var.c

#
#   webSockFilter.o
#
DEPS_53 += src/http.h

$(BUILD)/obj/webSockFilter.o: \
    src/webSockFilter.c $(DEPS_53)
	@echo '   [Compile] $(BUILD)/obj/webSockFilter.o'
	$(CC) -c -o $(BUILD)/obj/webSockFilter.o $(CFLAGS) -DME_DEBUG=1 -DVXWORKS -DRW_MULTI_THREAD -DCPU=PENTIUM -DTOOL_FAMILY=gnu -DTOOL=gnu -D_GNU_TOOL -D_WRS_KERNEL_ -D_VSB_CONFIG_FILE=\"/WindRiver/vxworks-7/samples/prebuilt_projects/vsb_vxsim_linux/h/config/vsbConfig.h\" -D_FILE_OFFSET_BITS=64 -DMBEDTLS_USER_CONFIG_FILE=\"embedtls.h\" $(IFLAGS) src/webSockFilter.c

ifeq ($(ME_COM_MBEDTLS),1)
#
#   libmbedtls
#
DEPS_54 += $(BUILD)/inc/osdep.h
DEPS_54 += $(BUILD)/inc/embedtls.h
DEPS_54 += $(BUILD)/inc/mbedtls.h
DEPS_54 += $(BUILD)/obj/mbedtls.o

$(BUILD)/bin/libmbedtls.a: $(DEPS_54)
	@echo '      [Link] $(BUILD)/bin/libmbedtls.a'
	arundefined -cr $(BUILD)/bin/libmbedtls.a "$(BUILD)/obj/mbedtls.o"
endif

ifeq ($(ME_COM_MBEDTLS),1)
#
#   libmpr-mbedtls
#
DEPS_55 += $(BUILD)/bin/libmbedtls.a
DEPS_55 += $(BUILD)/obj/mpr-mbedtls.o

$(BUILD)/bin/libmpr-mbedtls.a: $(DEPS_55)
	@echo '      [Link] $(BUILD)/bin/libmpr-mbedtls.a'
	arundefined -cr $(BUILD)/bin/libmpr-mbedtls.a "$(BUILD)/obj/mpr-mbedtls.o"
endif

ifeq ($(ME_COM_OPENSSL),1)
#
#   libmpr-openssl
#
DEPS_56 += $(BUILD)/obj/mpr-openssl.o

$(BUILD)/bin/libmpr-openssl.a: $(DEPS_56)
	@echo '      [Link] $(BUILD)/bin/libmpr-openssl.a'
	arundefined -cr $(BUILD)/bin/libmpr-openssl.a "$(BUILD)/obj/mpr-openssl.o"
endif

#
#   libmpr
#
DEPS_57 += $(BUILD)/inc/osdep.h
ifeq ($(ME_COM_MBEDTLS),1)
    DEPS_57 += $(BUILD)/bin/libmpr-mbedtls.a
endif
ifeq ($(ME_COM_MBEDTLS),1)
    DEPS_57 += $(BUILD)/bin/libmbedtls.a
endif
ifeq ($(ME_COM_OPENSSL),1)
    DEPS_57 += $(BUILD)/bin/libmpr-openssl.a
endif
DEPS_57 += $(BUILD)/inc/mpr.h
DEPS_57 += $(BUILD)/obj/mprLib.o

$(BUILD)/bin/libmpr.a: $(DEPS_57)
	@echo '      [Link] $(BUILD)/bin/libmpr.a'
	arundefined -cr $(BUILD)/bin/libmpr.a "$(BUILD)/obj/mprLib.o"

ifeq ($(ME_COM_PCRE),1)
#
#   libpcre
#
DEPS_58 += $(BUILD)/inc/pcre.h
DEPS_58 += $(BUILD)/obj/pcre.o

$(BUILD)/bin/libpcre.a: $(DEPS_58)
	@echo '      [Link] $(BUILD)/bin/libpcre.a'
	arundefined -cr $(BUILD)/bin/libpcre.a "$(BUILD)/obj/pcre.o"
endif

#
#   libhttp
#
DEPS_59 += $(BUILD)/bin/libmpr.a
ifeq ($(ME_COM_PCRE),1)
    DEPS_59 += $(BUILD)/bin/libpcre.a
endif
DEPS_59 += $(BUILD)/inc/http.h
DEPS_59 += $(BUILD)/obj/actionHandler.o
DEPS_59 += $(BUILD)/obj/auth.o
DEPS_59 += $(BUILD)/obj/basic.o
DEPS_59 += $(BUILD)/obj/cache.o
DEPS_59 += $(BUILD)/obj/chunkFilter.o
DEPS_59 += $(BUILD)/obj/client.o
DEPS_59 += $(BUILD)/obj/config.o
DEPS_59 += $(BUILD)/obj/conn.o
DEPS_59 += $(BUILD)/obj/digest.o
DEPS_59 += $(BUILD)/obj/dirHandler.o
DEPS_59 += $(BUILD)/obj/endpoint.o
DEPS_59 += $(BUILD)/obj/error.o
DEPS_59 += $(BUILD)/obj/fileHandler.o
DEPS_59 += $(BUILD)/obj/host.o
DEPS_59 += $(BUILD)/obj/monitor.o
DEPS_59 += $(BUILD)/obj/netConnector.o
DEPS_59 += $(BUILD)/obj/packet.o
DEPS_59 += $(BUILD)/obj/pam.o
DEPS_59 += $(BUILD)/obj/passHandler.o
DEPS_59 += $(BUILD)/obj/pipeline.o
DEPS_59 += $(BUILD)/obj/queue.o
DEPS_59 += $(BUILD)/obj/rangeFilter.o
DEPS_59 += $(BUILD)/obj/route.o
DEPS_59 += $(BUILD)/obj/rx.o
DEPS_59 += $(BUILD)/obj/sendConnector.o
DEPS_59 += $(BUILD)/obj/service.o
DEPS_59 += $(BUILD)/obj/session.o
DEPS_59 += $(BUILD)/obj/stage.o
DEPS_59 += $(BUILD)/obj/trace.o
DEPS_59 += $(BUILD)/obj/tx.o
DEPS_59 += $(BUILD)/obj/uploadFilter.o
DEPS_59 += $(BUILD)/obj/uri.o
DEPS_59 += $(BUILD)/obj/user.o
DEPS_59 += $(BUILD)/obj/var.o
DEPS_59 += $(BUILD)/obj/webSockFilter.o

$(BUILD)/bin/libhttp.a: $(DEPS_59)
	@echo '      [Link] $(BUILD)/bin/libhttp.a'
	arundefined -cr $(BUILD)/bin/libhttp.a "$(BUILD)/obj/actionHandler.o" "$(BUILD)/obj/auth.o" "$(BUILD)/obj/basic.o" "$(BUILD)/obj/cache.o" "$(BUILD)/obj/chunkFilter.o" "$(BUILD)/obj/client.o" "$(BUILD)/obj/config.o" "$(BUILD)/obj/conn.o" "$(BUILD)/obj/digest.o" "$(BUILD)/obj/dirHandler.o" "$(BUILD)/obj/endpoint.o" "$(BUILD)/obj/error.o" "$(BUILD)/obj/fileHandler.o" "$(BUILD)/obj/host.o" "$(BUILD)/obj/monitor.o" "$(BUILD)/obj/netConnector.o" "$(BUILD)/obj/packet.o" "$(BUILD)/obj/pam.o" "$(BUILD)/obj/passHandler.o" "$(BUILD)/obj/pipeline.o" "$(BUILD)/obj/queue.o" "$(BUILD)/obj/rangeFilter.o" "$(BUILD)/obj/route.o" "$(BUILD)/obj/rx.o" "$(BUILD)/obj/sendConnector.o" "$(BUILD)/obj/service.o" "$(BUILD)/obj/session.o" "$(BUILD)/obj/stage.o" "$(BUILD)/obj/trace.o" "$(BUILD)/obj/tx.o" "$(BUILD)/obj/uploadFilter.o" "$(BUILD)/obj/uri.o" "$(BUILD)/obj/user.o" "$(BUILD)/obj/var.o" "$(BUILD)/obj/webSockFilter.o"

#
#   http-server
#
DEPS_60 += $(BUILD)/bin/libhttp.a
DEPS_60 += $(BUILD)/obj/http-server.o

ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_60 += -lmbedtls
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_60 += -lmpr-mbedtls
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_60 += -lmbedtls
endif
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_60 += -lmpr-openssl
endif
LIBS_60 += -lmpr
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_60 += -lmpr-mbedtls
endif
ifeq ($(ME_COM_PCRE),1)
    LIBS_60 += -lpcre
endif
LIBS_60 += -lhttp
ifeq ($(ME_COM_PCRE),1)
    LIBS_60 += -lpcre
endif
LIBS_60 += -lmpr

$(BUILD)/bin/http-server.out: $(DEPS_60)
	@echo '      [Link] $(BUILD)/bin/http-server.out'
	$(CC) -o $(BUILD)/bin/http-server.out $(LDFLAGS) $(LIBPATHS) "$(BUILD)/obj/http-server.o" $(LIBPATHS_60) $(LIBS_60) $(LIBS_60) $(LIBS) -Wl,-r 

#
#   httpcmd
#
DEPS_61 += $(BUILD)/bin/libhttp.a
DEPS_61 += $(BUILD)/obj/http.o

ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_61 += -lmbedtls
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_61 += -lmpr-mbedtls
endif
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_61 += -lmbedtls
endif
ifeq ($(ME_COM_OPENSSL),1)
    LIBS_61 += -lmpr-openssl
endif
LIBS_61 += -lmpr
ifeq ($(ME_COM_MBEDTLS),1)
    LIBS_61 += -lmpr-mbedtls
endif
ifeq ($(ME_COM_PCRE),1)
    LIBS_61 += -lpcre
endif
LIBS_61 += -lhttp
ifeq ($(ME_COM_PCRE),1)
    LIBS_61 += -lpcre
endif
LIBS_61 += -lmpr

$(BUILD)/bin/http.out: $(DEPS_61)
	@echo '      [Link] $(BUILD)/bin/http.out'
	$(CC) -o $(BUILD)/bin/http.out $(LDFLAGS) $(LIBPATHS) "$(BUILD)/obj/http.o" $(LIBPATHS_61) $(LIBS_61) $(LIBS_61) $(LIBS) -Wl,-r 

#
#   install-certs
#
DEPS_62 += src/certs/samples/ca.crt
DEPS_62 += src/certs/samples/ca.key
DEPS_62 += src/certs/samples/ec.crt
DEPS_62 += src/certs/samples/ec.key
DEPS_62 += src/certs/samples/roots.crt
DEPS_62 += src/certs/samples/self.crt
DEPS_62 += src/certs/samples/self.key
DEPS_62 += src/certs/samples/test.crt
DEPS_62 += src/certs/samples/test.key

$(BUILD)/.install-certs-modified: $(DEPS_62)
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
#   installPrep
#

installPrep: $(DEPS_63)
	if [ "`id -u`" != 0 ] ; \
	then echo "Must run as root. Rerun with sudo." ; \
	exit 255 ; \
	fi

#
#   stop
#

stop: $(DEPS_64)

#
#   installBinary
#

installBinary: $(DEPS_65)

#
#   start
#

start: $(DEPS_66)

#
#   install
#
DEPS_67 += installPrep
DEPS_67 += stop
DEPS_67 += installBinary
DEPS_67 += start

install: $(DEPS_67)

#
#   uninstall
#
DEPS_68 += stop

uninstall: $(DEPS_68)

#
#   uninstallBinary
#

uninstallBinary: $(DEPS_69)

#
#   version
#

version: $(DEPS_70)
	echo $(VERSION)

