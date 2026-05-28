tinyproxy-protocols(7)

# NAME

tinyproxy-protocols - protocol behavior and forwarding semantics

# DESCRIPTION

This document describes how tinyproxy handles supported transport protocols
and Proxy Protocol v2 forwarding behavior.

# TCP

TCP routes accept client connections and forward bytes to the configured upstream.

Each accepted client connection creates one upstream connection.

# UDP

UDP routes listen for datagrams and forward them to the configured upstream.

UDP is connectionless, but the proxy keeps a small per-client mapping internally so upstream replies can be sent back to the correct original client address.

The current UDP implementation is intentionally simple:

- IPv4 only for now.
- One upstream UDP socket per observed client address.
- Idle client mappings are expired after a timeout.
- No attempt is made to provide delivery guarantees.
- No attempt is made to reassemble fragmented UDP traffic.

# Proxy Protocol V2

tinyproxy adds a Proxy Protocol v2 header so the upstream can recover:

- source IP
- source port
- destination IP
- destination port
- transport protocol

The `proxy_v2` option makes tinyproxy send a Proxy Protocol v2 header to the upstream server.

This is useful when the upstream server needs to know the original client address and port, but the proxy itself would otherwise hide that information.

## TCP Proxy Protocol v2

For TCP routes, tinyproxy writes one Proxy Protocol v2 header before forwarding the client stream.

The upstream must understand Proxy Protocol v2. If it does not, the upstream will see binary header bytes before the application payload and will likely fail.

## UDP Proxy Protocol v2

For UDP routes, tinyproxy prepends a Proxy Protocol v2 header to each forwarded client datagram.

The upstream receives:

```
PROXY_V2_HEADER + ORIGINAL_UDP_PAYLOAD
```

For IPv4 UDP, the header uses:

```
version/command = PROXY
family/proto    = INET/DGRAM
address length  = 12
```

This behavior is intended to match the useful subset of Proxy Protocol v2 used by load balancers that need to preserve client address information for UDP services.

AWS documents Proxy Protocol v2 support on Network Load Balancer target groups, and its AWS networking blog notes that UDP is supported even though the walkthrough focuses on TCP:

<https://aws.amazon.com/blogs/networking-and-content-delivery/preserving-client-ip-address-with-proxy-protocol-v2-and-network-load-balancer/>

<https://docs.aws.amazon.com/elasticloadbalancing/latest/network/load-balancer-target-groups.html>

# SEE ALSO

tinyproxy(1), tinyproxy.conf(5)
