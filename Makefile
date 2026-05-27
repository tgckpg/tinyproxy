export

CC ?= cc
AR ?= ar
RANLIB ?= ranlib
STRIP ?= strip
PKG_CONFIG ?= pkg-config

PROJECT_ROOT := $(CURDIR)
BIN_DIR := $(CURDIR)/bin
BUILD_DIR := $(PROJECT_ROOT)/build

SRC := klog.c \
       proxy_proto_v2.c \
       signal.c \
       route.c \
       tcp_route.c \
       udp_route.c \
	   env.c \
       file_conf.c \
       tinyproxy.c

EXEEXT :=
WINDOWS_LDLIBS :=

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
WINDOWS_LDLIBS += -lws2_32
endif

BIN := $(BIN_DIR)/tinyproxy$(EXEEXT)

LIBEVENT_SRC ?=
LIBEVENT_PREFIX := $(BUILD_DIR)/libevent-install
LIBEVENT_CORE_A := $(LIBEVENT_PREFIX)/lib/libevent_core.a

CFLAGS ?= -Os -Wall -Wextra -ffunction-sections -fdata-sections

UNAME_S := $(shell uname)

ifeq ($(UNAME_S),Darwin)
STATIC ?= 0
LDFLAGS += -Wl,-dead_strip
TEST_FLAGS := CONCURRENCY=1000 TOTAL=1000 FD_LIMIT=2560
else ifeq ($(OS),Windows_NT)
STATIC ?= 0
LDFLAGS += -Wl,--gc-sections
TEST_FLAGS :=
TEST_FLAGS := CONCURRENCY=100 TOTAL=100 FD_LIMIT=512
else
STATIC ?= 1
LDFLAGS += -Wl,--gc-sections
TEST_FLAGS :=
endif

ifeq ($(STATIC),1)
LDFLAGS += -static
PKG_CONFIG_STATIC := --static
else
PKG_CONFIG_STATIC :=
endif

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
LDLIBS   += $(WINDOWS_LDLIBS)

all: $(BIN)

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

strip: $(BIN)
ifeq ($(UNAME_S),Darwin)
	$(error strip target is not supported on macOS; build without stripping, or strip inside the Linux/Alpine container)
else
	$(STRIP) $(BIN)
endif

test: $(BIN)
	$(TEST_FLAGS) python3 -m tests.run_tests $(BIN)

clean:
	rm -rf $(BIN_DIR)

clean-libevent:
	-$(MAKE) -C $(LIBEVENT_SRC) distclean
	rm -rf $(LIBEVENT_PREFIX)

distclean: clean clean-libevent
	rm -rf $(BUILD_DIR)

.PHONY: all clean clean-libevent distclean test strip
