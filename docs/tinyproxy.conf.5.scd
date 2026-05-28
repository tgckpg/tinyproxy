tinyproxy.conf(5)

# NAME

tinyproxy.conf - tinyproxy configuration file

# DESCRIPTION

tinyproxy.conf defines TCP and UDP forwarding routes.

# GLOBAL OPTIONS

TINYPROXY_RUNTIME_DIR="/tmp/tinyproxy"

# ROUTES

listen upstream protocol [options...]

# OPTIONS

proxy_v2
	Enable proxy v2 for upstream
	Default: false

keep_alive
	Enable keep alive for tcp
	Default: false

idle_timeout=SECONDS
	Idle timeout for both listen and upstream
	Default: 60

connect_timeout=SECONDS
	Connection timeout for both listen and upstream
	Default: 5

# ENDPOINTS

unix:
	unix:/tmp/backend.sock

unix datagram:
	unix-dgram:/tmp/socket.sock

builtin://client_addr
	Returns the observed client address as plain text, then closes.
	Example response: 127.0.0.1:54321

builtin://discard
	TCP: reads and discards client data until the client closes or timeout expires.
	UDP: silently drops each datagram.

builtin://hang
	Accepts the connection/datagram and intentionally produces no response.
	TCP: the connection remains open until the client closes or the route idle timeout expires.
	UDP: this is effectively a silent drop.

builtin://close
	Accepts the connection and immediately closes it without reading or writing.
	For UDP, this is equivalent to discard/drop because UDP has no connection to close.

file:///path/to/file
	Return the contents of the file in raw bytes.

# EXAMPLES

Forward HTTPS:

	Direct tcp forward with default settings
	0.0.0.0:443 10.0.1.10:443 tcp

	Upstream accepts proxy protocol v2
	0.0.0.0:443 10.0.1.10:443 tcp proxy_v2

UDP route:

	0.0.0.0:53 1.1.1.1:53 udp

Serve a fixed HTTP response:

	$ printf 'HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\nContent-Length: 14\\r\\nConnection: close\\r\\n\\r\\nhello, world!\\n' > response.http

	0.0.0.0:8080 file://response.http tcp

# FILES

/etc/tinyproxy.conf

# SEE ALSO

tinyproxy(1)
