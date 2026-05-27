PROJECT_ROOT := $(abspath ..)

# Sanitizers + static linking are a bad combo. Must be before common.mk.
STATIC := 0

include $(PROJECT_ROOT)/mk/common.mk
include $(PROJECT_ROOT)/mk/libevent.mk

CC ?= clang

OUT := $(PROJECT_ROOT)/bin/fuzz
CORPUS := $(PROJECT_ROOT)/corpus
SEEDS := seeds

TARGETS := proxy_v2_build file_conf tcp_route
BINS := $(addprefix $(OUT)/fuzz_,$(TARGETS))

FUZZ_CFLAGS ?= -g -O1 -DFUZZ -fsanitize=fuzzer,address,undefined
FUZZ_LDFLAGS ?= -fsanitize=fuzzer,address,undefined

COMMON_FUZZ_SRC := \
	$(SRC_DIR)/klog.c

.PHONY: all run clean FORCE
.PRECIOUS: $(OUT)/fuzz_%

all: $(BINS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/fuzz_proxy_v2_build: \
	fuzz_proxy_v2_build.c \
	$(SRC_DIR)/proxy_proto_v2.c \
	| $(OUT)
	$(CC) $(CFLAGS) $(FUZZ_CFLAGS) $(CPPFLAGS) \
		$(SRC_DIR)/proxy_proto_v2.c \
		fuzz_proxy_v2_build.c \
		-o $@ \
		$(LDFLAGS) $(FUZZ_LDFLAGS) $(LDLIBS)

$(OUT)/fuzz_file_conf: \
	fuzz_file_conf.c \
	$(SRC_DIR)/file_conf.c \
	$(SRC_DIR)/klog.c \
	| $(OUT)
	$(CC) $(CFLAGS) $(FUZZ_CFLAGS) $(CPPFLAGS) \
		$(SRC_DIR)/file_conf.c \
		$(SRC_DIR)/klog.c \
		fuzz_file_conf.c \
		-o $@ \
		$(LDFLAGS) $(FUZZ_LDFLAGS) $(LDLIBS)

$(OUT)/fuzz_tcp_route: \
	fuzz_tcp_route.c \
	$(SRC_DIR)/tcp_route.c \
	$(SRC_DIR)/proxy_proto_v2.c \
	$(SRC_DIR)/klog.c \
	$(LIBEVENT_DEPS) \
	| $(OUT)
	$(CC) $(CFLAGS) $(FUZZ_CFLAGS) $(CPPFLAGS) \
		$(SRC_DIR)/tcp_route.c \
		$(SRC_DIR)/proxy_proto_v2.c \
		$(SRC_DIR)/route.c \
		$(SRC_DIR)/klog.c \
		fuzz_tcp_route.c \
		-o $@ \
		$(LDFLAGS) $(FUZZ_LDFLAGS) $(LDLIBS)

seed-%: FORCE
	mkdir -p $(CORPUS)/$*
	if [ -d "$(SEEDS)/$*" ]; then \
		cp "$(SEEDS)/$*"/* "$(CORPUS)/$*"/ 2>/dev/null || true; \
	fi

run-%: $(OUT)/fuzz_% seed-%
	$(OUT)/fuzz_$* \
		$(CORPUS)/$* \
		-max_total_time=$${MAX_TOTAL_TIME:-60} \
		-max_len=$${MAX_LEN:-8192}

run: $(addprefix run-,$(TARGETS))

clean:
	rm -rf $(OUT)

FORCE:
