#!/usr/bin/env bash
set -euo pipefail

# Run from repo root:
#   sudo ./test_tinyproxy.sh
#
# Routes:
#   :12700 -> builtin http_ok
#   :12800 -> TCP proxy to 127.0.0.1:12700

NOFILE="${NOFILE:-1048576}"
WORKERS="${WORKERS:-0}"
TINYPROXY="${TINYPROXY:-/usr/local/bin/tinyproxy}"

LISTEN_BACKLOG="${LISTEN_BACKLOG:-65535}"
SYN_BACKLOG="${SYN_BACKLOG:-65535}"
EPHEMERAL_RANGE="${EPHEMERAL_RANGE:-10000 65535}"

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run as root: sudo $0" >&2
	exit 1
fi

if [ ! -x "$TINYPROXY" ]; then
	echo "error: tinyproxy binary not found/executable: $TINYPROXY" >&2
	exit 1
fi

echo "== tinyproxy stress-test launcher =="
echo "tinyproxy:        $TINYPROXY"
echo "workers:          $WORKERS"
echo "nofile:           $NOFILE"
echo "somaxconn:        $LISTEN_BACKLOG"
echo "tcp_max_syn_backlog: $SYN_BACKLOG"
echo "ephemeral ports:  $EPHEMERAL_RANGE"
echo

echo "== raising process fd limit =="
ulimit -n "$NOFILE"
echo "soft nofile: $(ulimit -Sn)"
echo "hard nofile: $(ulimit -Hn)"
echo

echo "== applying temporary sysctl tuning =="
sysctl -w "net.core.somaxconn=$LISTEN_BACKLOG"
sysctl -w "net.ipv4.tcp_max_syn_backlog=$SYN_BACKLOG"
sysctl -w "net.ipv4.ip_local_port_range=$EPHEMERAL_RANGE"
echo

echo "== current relevant sysctls =="
sysctl net.core.somaxconn
sysctl net.ipv4.tcp_max_syn_backlog
sysctl net.ipv4.ip_local_port_range
echo

echo "== starting tinyproxy =="
echo "test direct builtin:"
echo "  ab -c 10000 -n 100000 http://127.0.0.1:12700/"
echo
echo "test chained tcp->tcp->builtin:"
echo "  ab -c 10000 -n 100000 http://127.0.0.1:12800/"
echo
echo "watch sockets:"
echo "  ./watch_tinyproxy_sockets.sh"
echo

exec "$TINYPROXY" \
	-w"$WORKERS" \
	-L "tcp :12700 builtin http_ok" \
	-L "tcp :12800 tcp 127.0.0.1:12700"
