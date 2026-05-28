tinyproxy(1)

# NAME

tinyproxy - lightweight TCP/UDP proxy

# SYNOPSIS

*tinyproxy* [options]

# DESCRIPTION

tinyproxy forwards TCP and UDP traffic between endpoints.

Supported endpoint types:
- TCP
- UDP
- UNIX stream sockets
- UNIX datagram sockets
- file backends
- Some builtin backends

Features:
- PROXY protocol v2
- idle and connect timeouts
- keep-alive support
- multiple routes
- IPv4 and IPv6

Routes may be configured using a config file or directly from the command line with *-L*.

# OPTIONS

*-c* <config>
	Load configuration file.

*-L* <route>
	Add a route using the same syntax as one config file line.
	May be specified multiple times.

*-v*
	Show version information.

*-h*
	Show help.

# CONFIGURATION

Each route uses the following syntax:

	listen <listen-proto> <listen-endpoint> <backend-proto> <backend-endpoint> [options...]

Supported protocols:
- tcp
- udp
- unix
- unix-dgram
- file

# CONFIGURATION EXAMPLES

Forward TCP traffic:

	listen tcp 0.0.0.0:80 tcp 10.0.1.1:80

Enable PROXY protocol v2 and keep-alive:

	listen tcp 0.0.0.0:443 tcp 10.0.1.1:443 proxy_v2 keep_alive

Forward UDP traffic:

	listen udp 0.0.0.0:5353 udp 10.0.1.1:53

Forward to UNIX socket:

	listen tcp 127.0.0.1:8080 unix /tmp/backend.sock

Serve static HTTP response from file:

	listen tcp 127.0.0.1:8080 file /tmp/response.http

# COMMAND LINE EXAMPLES

Run with config file:

	tinyproxy -c /etc/tinyproxy.conf

Define routes directly from the command line:

	tinyproxy \
		-L "tcp 0.0.0.0:80 tcp 10.0.1.1:80" \
		-L "tcp 0.0.0.0:443 tcp 10.0.1.1:443 proxy_v2"

# EXIT STATUS

*0*
	Success.

*1*
	Failure.

# SEE ALSO

	https://github.com/tgckpg/tinyproxy

# AUTHORS

Penguin ts
