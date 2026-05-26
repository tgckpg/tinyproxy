export

CC ?= cc
AR := /usr/bin/ar
RANLIB := /usr/bin/ranlib
STRIP ?= strip
STATIC ?= 1

PROJECT_ROOT := $(CURDIR)
BIN_DIR := $(CURDIR)/bin

SRC := klog.c \
	   proxy_proto_v2.c \
	   route.c \
	   tcp_route.c \
       file_conf.c \
       tinyproxy.c

BIN := $(BIN_DIR)/tinyproxy

BUILD_DIR := $(PROJECT_ROOT)/build

# Optional. Set this only when you want to build vendored libevent.
# Example:
#   make LIBEVENT_SRC=$(PROJECT_ROOT)/libevent2
LIBEVENT_SRC ?=

LIBEVENT_PREFIX := $(BUILD_DIR)/libevent-install
LIBEVENT_CORE_A := $(LIBEVENT_PREFIX)/lib/libevent_core.a

CFLAGS ?= -Os -Wall -Wextra -ffunction-sections -fdata-sections

ifeq ($(STATIC),1)
LDFLAGS += -static
PKG_CONFIG_STATIC := --static
endif

ifeq ($(strip $(LIBEVENT_SRC)),)
# System libevent, statically linked
LIBEVENT_CPPFLAGS := $(shell pkg-config --cflags libevent_core)
LIBEVENT_LDFLAGS  :=
LIBEVENT_LDLIBS   := $(shell pkg-config --static --libs libevent_core)
LIBEVENT_DEPS     :=
else
# Vendored libevent
LIBEVENT_CPPFLAGS := -I$(LIBEVENT_PREFIX)/include
LIBEVENT_LDFLAGS  := -L$(LIBEVENT_PREFIX)/lib
LIBEVENT_LDLIBS   := -levent_core
LIBEVENT_DEPS     := $(LIBEVENT_CORE_A)
endif

CPPFLAGS += $(LIBEVENT_CPPFLAGS)
LDFLAGS  += -static $(LIBEVENT_LDFLAGS)
LDLIBS   += $(LIBEVENT_LDLIBS)

$(BIN): $(SRC) $(LIBEVENT_DEPS)
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

ifneq ($(strip $(LIBEVENT_SRC)),)
$(LIBEVENT_CORE_A):
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

ifeq ($(shell uname),Darwin)
LDFLAGS += -Wl,-dead_strip
TEST_FLAGS := CONCURRENCY=1000 TOTAL=1000 FD_LIMIT=2560
else
LDFLAGS += -Wl,--gc-sections
TEST_FLAGS :=
endif

all: $(BIN)

strip: $(BIN)
	$(STRIP) $(BIN)

test: $(BIN)
	$(TEST_FLAGS) python3 tests/test_proxy.py $(BIN)

clean:
	rm -rf $(BIN_DIR)

clean-libevent:
	-$(MAKE) -C $(LIBEVENT_SRC) distclean
	rm -rf $(LIBEVENT_PREFIX)

distclean: clean clean-libevent
	rm -rf $(BUILD_DIR)

.PHONY: all clean clean-libevent distclean test strip
