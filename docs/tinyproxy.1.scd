tinyproxy(1)

# NAME

tinyproxy - lightweight TCP/UDP proxy

# SYNOPSIS

*tinyproxy* [options]

# DESCRIPTION

tinyproxy forwards TCP and UDP traffic.

Supports:
- PROXY protocol v2
- UNIX sockets
- UDP forwarding

# OPTIONS

*-c* <config>
	Load config file.

*-v*
	Show version.

# EXAMPLES

	tinyproxyc -c /etc/tinyproxy.conf

# AUTHORS

Penguin ts
