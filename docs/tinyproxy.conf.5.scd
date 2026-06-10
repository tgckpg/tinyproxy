tinyproxy.conf(5)

# NAME

tinyproxy.conf - tinyproxy configuration file

# DESCRIPTION

tinyproxy.conf defines TCP and UDP forwarding routes.

# GLOBAL OPTIONS

TINYPROXY_RUNTIME_DIR="/tmp/tinyproxy"

# ROUTES

listen <listen-proto> <listen-endpoint> <backend-proto> <backend-endpoint> [options...]

# OPTIONS

proxy_v2
	Enable proxy v2 for upstream.
	Default: false

sni_sniff
	Enable passive SNI sniffing for TLS traffic.

	When enabled, tinyproxy inspects bytes already read from the client's
	TLS ClientHello and logs the observed Server Name Indication as
	client_sni=. TLS traffic is not decrypted or modified.

	If no usable SNI is observed, client_sni= contains a bracketed status
	such as <missing>, <not_tls>, <not_observed>, or <parse_error>.

	This option is mainly intended for debugging routes with shared TLS
	upstreams, such as investigating which hostname is associated with
	upstream timeout or reset errors.

	Do not enable this option on stable routes unless the extra diagnostic
	logging is needed. It adds a small bounded parsing cost to client reads.

	Default: false

keep_alive
	Enable keep alive for tcp.
	Default: false

idle_timeout=SECONDS
	Idle timeout for both listen and upstream.
	Default: 60

connect_timeout=SECONDS
	Connection timeout for both listen and upstream.
	Default: 5

bind_wait=SECONDS
	Wait up to SECONDS for the listen address to become available when
	bind(2) fails with EADDRINUSE.

	This is useful during rolling restarts where a replacement process may
	start before the previous process has released the listen port. If the
	address becomes available before the timeout expires, tinyproxy binds it
	and continues startup. Otherwise startup fails.

	The default is 0, which fails immediately.

broadcast_reply=listen
	UDP only. Send upstream replies back to the original client from the listening endpoint.

broadcast_reply=upstream
	UDP/IPv4 only. Forward broadcast requests to the configured upstream and
	send upstream replies back to the original client using raw IPv4 packets
	whose source address and port are the upstream endpoint.

	This lets broadcast-based discovery advertise the upstream address directly,
	so clients can connect to the upstream after discovery instead of sending
	gameplay traffic through tinyproxy. Linux only; requires CAP_NET_RAW.

# ENDPOINTS

unix /tmp/backend.sock
	Unix sock

unix-dgram /tmp/socket.sock
	Unix sock datagram

file /path/to/file
	File contents in raw bytes

## Builtin

builtin clientaddr
	Returns the observed client address as plain text, then closes.

	Example response: 127.0.0.1:54321

builtin discard
	- TCP: reads and discards client data until the client closes or timeout expires.
	- UDP: silently drops each datagram.

builtin hang
	Accepts the connection/datagram and intentionally produces no response.

	- TCP: the connection remains open until the client closes or the route idle timeout expires.
	- UDP: this is effectively a silent drop.

builtin close
	- TCP: Accepts the connection and immediately closes it without reading or writing.
	- UDP: this is equivalent to discard/drop because UDP has no connection to close.

buitin http_ok
	Returns "HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nOK".
	Waits for client to close the connection.

	Useful for probing

buitin log_conn
	Logs the connection.

# EXAMPLES

Forward HTTPS:

	Direct tcp forward with default settings
	- listen tcp 0.0.0.0:443 tcp 10.0.1.10:443

	Upstream accepts proxy protocol v2
	- listen tcp 0.0.0.0:443 tcp 10.0.1.10:443 proxy_v2

Redirect UDP:

	- listen udp 0.0.0.0:53 udp 1.1.1.1:53

Serve a fixed HTTP response:

	$ printf 'HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\nContent-Length: 14\\r\\nConnection: close\\r\\n\\r\\nhello, world!\\n' > response.http

	- listen tcp 0.0.0.0:8080 file response.http

Rule chaining:

```
Multiple rules can be chained together. This is useful for testing mixed
endpoint paths. For example, the following setup sends a client request
through TCP, UNIX stream sockets, more TCP hops, and finally a static file
backend:

client
	=> tcp :14001
	=> unix /tmp/tinyproxy-chain-4.sock
	=> tcp 127.0.0.1:13003
	=> tcp 127.0.0.1:13002
	=> unix /tmp/tinyproxy-chain-1.sock
	=> file examples/http-response.txt

Configuration:

listen unix /tmp/tinyproxy-chain-1.sock file examples/http-response.txt
listen tcp  :13002                      unix /tmp/tinyproxy-chain-1.sock
listen tcp  :13003                      tcp  127.0.0.1:13002
listen unix /tmp/tinyproxy-chain-4.sock tcp  127.0.0.1:13003
listen tcp  :14001                      unix /tmp/tinyproxy-chain-4.sock
```


# FILES

/etc/tinyproxy.conf

# SEE ALSO

tinyproxy(1)
