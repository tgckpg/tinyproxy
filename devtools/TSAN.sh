#!/bin/bash

CC=clang EXTRA_CFLAGS="-Og -g3 -DTINYPROXY_DEBUG -fsanitize=thread -fno-omit-frame-pointer"   LDFLAGS="-fsanitize=thread" LIBEVENT_SRC=libevent2/ make clean all
