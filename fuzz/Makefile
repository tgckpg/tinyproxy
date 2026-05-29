PROJECT_ROOT := $(abspath ..)

# Sanitizers + static linking are a bad combo. Must be before common.mk.
STATIC := 0

include $(PROJECT_ROOT)/mk/common.mk
include $(PROJECT_ROOT)/mk/libevent.mk

CC ?= clang

OUT := $(PROJECT_ROOT)/bin/fuzz
CORPUS := $(PROJECT_ROOT)/corpus
SEEDS := seeds

TARGETS := proxy_v2_build file_conf stream_conn
BINS := $(addprefix $(OUT)/fuzz_,$(TARGETS))

PROJECT_SRCS := $(filter-out $(SRC_DIR)/tinyproxy.c,$(wildcard $(SRC_DIR)/*.c))

FUZZ_CFLAGS ?= -g -O1 -DFUZZ -fsanitize=fuzzer,address,undefined
FUZZ_LDFLAGS ?= -fsanitize=fuzzer,address,undefined

COMMON_FUZZ_SRC := \
	$(SRC_DIR)/klog.c

.PHONY: all run clean FORCE
.PRECIOUS: $(OUT)/fuzz_%

all: $(BINS)

$(OUT)/fuzz_proxy_v2_build: fuzz_proxy_v2_build.c $(PROJECT_SRCS)
	$(CC) $(CFLAGS) $(FUZZ_CFLAGS) $(CPPFLAGS) \
		-I$(SRC_DIR) \
		$^ \
		-o $@ \
		$(LDFLAGS) $(FUZZ_LDFLAGS) $(LDLIBS)

$(OUT)/fuzz_file_conf: fuzz_file_conf.c $(PROJECT_SRCS)
	$(CC) $(CFLAGS) $(FUZZ_CFLAGS) $(CPPFLAGS) \
		-I$(SRC_DIR) \
		$^ \
		-o $@ \
		$(LDFLAGS) $(FUZZ_LDFLAGS) $(LDLIBS)

$(OUT)/fuzz_stream_conn: fuzz_stream_conn.c $(PROJECT_SRCS)
	$(CC) $(CFLAGS) $(FUZZ_CFLAGS) $(CPPFLAGS) \
		-I$(SRC_DIR) \
		$^ \
		-o $@ \
		$(LDFLAGS) $(LDLIBS)

seed-%: FORCE
	mkdir -p $(CORPUS)/$*
	if [ -d "$(SEEDS)/$*" ]; then \
		cp "$(SEEDS)/$*"/* "$(CORPUS)/$*"/ 2>/dev/null || true; \
	fi

run-%: $(OUT)/fuzz_% seed-%
	mkdir -p $(OUT)
	$(OUT)/fuzz_$* \
		$(CORPUS)/$* \
		-max_total_time=$${MAX_TOTAL_TIME:-60} \
		-max_len=$${MAX_LEN:-8192}

run: $(addprefix run-,$(TARGETS))

clean:
	rm -rf $(OUT)

FORCE:
