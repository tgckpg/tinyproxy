#!/bin/sh
set -eu

MAIN="${1:-main}"

IGNORE='
LOG_ERROR
LOG_INFO
LOG_WARN
calloc
clock_gettime
fclose
feof
ferror
fgets
fopen
fprintf
fputc
fputs
fread
free
getpid
htons
inet_ntop
inet_pton
isspace
localtime_compat
localtime_r
localtime_s
malloc
memcmp
memcpy
memmove
memset
ntohs
printf
putc
putchar
puts
realloc
snprintf
strchr
strcmp
strcpy
strdup
strerror
strftime
strlen
strncmp
strrchr
strtok_r
strtol
strtoul
time
trim
unlink
'

IGNORE_RE="$(printf '%s\n' "$IGNORE" \
	| sed '/^[[:space:]]*$/d' \
	| paste -sd '|' -)"

cflow --brief -n --main="$MAIN" src/*.c |
	grep -Ev "^[[:space:]]+[[:digit:]]+[[:space:]]*(${IGNORE_RE})\\("
