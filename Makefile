.DEFAULT_GOAL := all

PROJECT_ROOT := $(CURDIR)

include mk/common.mk
include mk/libevent.mk

BIN := $(BIN_DIR)/tinyproxy$(EXEEXT)

SRC := $(wildcard $(SRC_DIR)/*.c)

CFLAGS += -ffile-prefix-map=$(SRC_DIR)/=

all: $(BIN)

$(BIN): $(SRC) $(LIBEVENT_DEPS)
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

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

distclean: clean clean-libevent
	rm -rf $(BUILD_DIR)

.PHONY: all clean distclean test strip
