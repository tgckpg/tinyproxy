export

CC ?= cc
AR ?= ar
RANLIB ?= ranlib
STRIP ?= strip
PKG_CONFIG ?= pkg-config

PROJECT_ROOT ?= $(CURDIR)
SRC_DIR ?= $(PROJECT_ROOT)/src
BIN_DIR ?= $(PROJECT_ROOT)/bin
BUILD_DIR ?= $(PROJECT_ROOT)/build

EXEEXT :=
WINDOWS_LDLIBS :=

ifeq ($(OS),Windows_NT)
EXEEXT := .exe
WINDOWS_LDLIBS += -lws2_32
endif

CFLAGS ?= -Os -Wall -Wextra -ffunction-sections -fdata-sections
CPPFLAGS += -I$(SRC_DIR)

UNAME_S := $(shell uname)

ifeq ($(UNAME_S),Darwin)
STATIC ?= 0
LDFLAGS += -Wl,-dead_strip
TEST_FLAGS := CONCURRENCY=1000 TOTAL=1000 FD_LIMIT=2560
else ifeq ($(OS),Windows_NT)
STATIC ?= 0
LDFLAGS += -Wl,--gc-sections
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

LDLIBS += $(WINDOWS_LDLIBS)