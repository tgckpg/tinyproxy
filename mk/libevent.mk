LIBEVENT_SRC ?=
LIBEVENT_PREFIX := $(BUILD_DIR)/libevent-install
LIBEVENT_CORE_A := $(LIBEVENT_PREFIX)/lib/libevent_core.a
LIBEVENT_PATCH_STAMP := $(BUILD_DIR)/libevent-evbuffer-max-read.patch.stamp

ifeq ($(strip $(LIBEVENT_SRC)),)
LIBEVENT_CPPFLAGS := $(shell $(PKG_CONFIG) --cflags libevent_core 2>/dev/null)
LIBEVENT_LDFLAGS  :=
LIBEVENT_LDLIBS   := $(shell $(PKG_CONFIG) $(PKG_CONFIG_STATIC) --libs libevent_core 2>/dev/null)
LIBEVENT_DEPS     :=
else
LIBEVENT_CPPFLAGS := -I$(LIBEVENT_PREFIX)/include
LIBEVENT_LDFLAGS  := -L$(LIBEVENT_PREFIX)/lib
LIBEVENT_LDLIBS   := -levent_core
LIBEVENT_DEPS     := $(LIBEVENT_CORE_A)
endif

CPPFLAGS += $(LIBEVENT_CPPFLAGS)
LDFLAGS  += $(LIBEVENT_LDFLAGS)
LDLIBS   += $(LIBEVENT_LDLIBS)

$(LIBEVENT_PATCH_STAMP):
	mkdir -p $(BUILD_DIR)
	grep -q 'EVBUFFER_MAX_READ.*128 \* 1024' $(LIBEVENT_SRC)/buffer.c || \
		sed -i.bak 's/#define EVBUFFER_MAX_READ[[:space:]]*4096/#define EVBUFFER_MAX_READ (128 * 1024)/' $(LIBEVENT_SRC)/buffer.c
	touch $@

ifneq ($(strip $(LIBEVENT_SRC)),)
$(LIBEVENT_CORE_A): $(LIBEVENT_PATCH_STAMP)
	chmod +x $(LIBEVENT_SRC)/configure
	cd $(LIBEVENT_SRC) && \
		AR=$(AR) RANLIB=$(RANLIB) ./configure \
		--prefix="$(LIBEVENT_PREFIX)" \
		--disable-openssl \
		--disable-samples \
		--disable-libevent-regress \
		--disable-debug-mode \
		--disable-malloc-replacement \
		--disable-thread-support \
		--disable-dependency-tracking \
		--enable-static \
		--disable-shared
	AR=$(AR) RANLIB=$(RANLIB) $(MAKE) -C $(LIBEVENT_SRC)
	AR=$(AR) RANLIB=$(RANLIB) $(MAKE) -C $(LIBEVENT_SRC) install
endif

.PHONY: clean-libevent

clean-libevent:
ifneq ($(strip $(LIBEVENT_SRC)),)
	-$(MAKE) -C $(LIBEVENT_SRC) distclean
endif
	rm -rf $(LIBEVENT_PREFIX)
