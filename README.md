# tinyproxy

This is a work-in-progress tiny L4 proxy.

The goal is not to become HAProxy, Envoy, nginx, or a general-purpose load balancer but to stay small and practical for simple inetd-style forwarding tasks.

## Rule of thumb

1. Should feel like an inet utility.
2. Be simple and effective.
3. Run in the foreground.
4. Use libevent2 for readability and future maintainability.
5. Prefer explicit behavior over magic.
6. Avoid protocol-specific features unless they are small and practical.

## Configuration

Configuration is line-based:

```text
LISTEN_HOST:LISTEN_PORT UPSTREAM_HOST:UPSTREAM_PORT PROTO [OPTIONS...]
```

Example TCP route:

```text
127.0.0.1:31232 127.0.0.1:41232 tcp
```

Example UDP route:

```text
127.0.0.1:31232 127.0.0.1:41232 udp
```

Example TCP route with Proxy Protocol v2:

```text
127.0.0.1:31232 127.0.0.1:41232 tcp proxy_v2
```

Example UDP route with Proxy Protocol v2:

```text
127.0.0.1:31232 127.0.0.1:41232 udp proxy_v2
```

Run with:

```sh
tinyproxy -c tinyproxy.conf
```

## Supported protocols

### TCP

TCP routes accept client connections and forward bytes to the configured upstream.

Each accepted client connection creates one upstream connection.

### UDP

UDP routes listen for datagrams and forward them to the configured upstream.

UDP is connectionless, but the proxy keeps a small per-client mapping internally so upstream replies can be sent back to the correct original client address.

The current UDP implementation is intentionally simple:

- IPv4 only for now.
- One upstream UDP socket per observed client address.
- Idle client mappings are expired after a timeout.
- No attempt is made to provide delivery guarantees.
- No attempt is made to reassemble fragmented UDP traffic.

## Proxy Protocol v2

The `proxy_v2` option makes tinyproxy send a Proxy Protocol v2 header to the upstream server.

This is useful when the upstream server needs to know the original client address and port, but the proxy itself would otherwise hide that information.

### TCP Proxy Protocol v2

For TCP routes, tinyproxy writes one Proxy Protocol v2 header before forwarding the client stream.

The upstream must understand Proxy Protocol v2. If it does not, the upstream will see binary header bytes before the application payload and will likely fail.

### UDP Proxy Protocol v2

For UDP routes, tinyproxy prepends a Proxy Protocol v2 header to each forwarded client datagram.

The upstream receives:

```text
PROXY_V2_HEADER + ORIGINAL_UDP_PAYLOAD
```

For IPv4 UDP, the header uses:

```text
version/command = PROXY
family/proto    = INET/DGRAM
address length  = 12
```

This behavior is intended to match the useful subset of Proxy Protocol v2 used by load balancers that need to preserve client address information for UDP services.

AWS documents Proxy Protocol v2 support on Network Load Balancer target groups, and its AWS networking blog notes that UDP is supported even though the walkthrough focuses on TCP:

- [AWS: Preserving client IP address with Proxy protocol v2 and Network Load Balancer](https://aws.amazon.com/blogs/networking-and-content-delivery/preserving-client-ip-address-with-proxy-protocol-v2-and-network-load-balancer/)
- [AWS: Target groups for your Network Load Balancers](https://docs.aws.amazon.com/elasticloadbalancing/latest/network/load-balancer-target-groups.html)

tinyproxy adds a Proxy Protocol v2 header so the upstream can recover:

- source IP
- source port
- destination IP
- destination port
- transport protocol

## Non-goals

tinyproxy is not trying to provide:

- TLS termination
- HTTP routing
- retries
- load balancing
- service discovery
- health checks
- dynamic configuration reloads
- QUIC awareness
- full AWS NLB emulation
- full HAProxy compatibility

Use HAProxy, Envoy, nginx, Cilium, or a real load balancer if you need those.

## Development status

This project is still experimental.

Currently tested areas include:

- TCP roundtrip forwarding
- TCP stress forwarding
- TCP Proxy Protocol v2 forwarding
- UDP roundtrip forwarding
- UDP Proxy Protocol v2 forwarding

Expect rough edges.
