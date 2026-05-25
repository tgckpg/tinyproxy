export

CC ?= cc
AR := /usr/bin/ar
RANLIB := /usr/bin/ranlib
STRIP ?= strip

PROJECT_ROOT := $(CURDIR)
BIN_DIR := $(CURDIR)/bin

SRC := proxy_proto_v2.c \
	   tcp_route.c \
       file_conf.c \
       tinyproxy.c

BIN := $(BIN_DIR)/tinyproxy

BUILD_DIR := $(PROJECT_ROOT)/build

LIBEVENT_SRC := $(PROJECT_ROOT)/libevent2
LIBEVENT_PREFIX := $(BUILD_DIR)/libevent-install
LIBEVENT_CORE_A := $(LIBEVENT_PREFIX)/lib/libevent_core.a

CFLAGS ?= -Os -Wall -Wextra -ffunction-sections -fdata-sections
CPPFLAGS += -I$(LIBEVENT_PREFIX)/include
LDFLAGS += -L$(LIBEVENT_PREFIX)/lib

ifeq ($(shell uname),Darwin)
LDFLAGS += -Wl,-dead_strip
else
LDFLAGS += -Wl,--gc-sections
endif

LDLIBS += -levent_core

all: $(BIN)

$(BIN): $(SRC) $(LIBEVENT_CORE_A)
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

strip: $(BIN)
	$(STRIP) $(BIN)

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

test: $(BIN)
	python3 tests/test_proxy.py $(BIN)

clean:
	rm -rf $(BIN_DIR)

clean-libevent:
	-$(MAKE) -C $(LIBEVENT_SRC) distclean
	rm -rf $(LIBEVENT_PREFIX)

distclean: clean clean-libevent
	rm -rf $(BUILD_DIR)

.PHONY: all clean clean-libevent distclean test strip
