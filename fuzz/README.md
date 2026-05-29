#!/bin/bash

macos

Fuzz for 8 hours
```
LIBEVENT_SRC=libevent2/ MAX_TOTAL_TIME=28800 MAX_LEN=65536 CC="$(brew --prefix llvm)/bin/clang" caffeinate make -C fuzz run-file_conf
```
