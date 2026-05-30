# tinyproxy

An L4 proxy designed to act as a tiny transparent shim.

## Philosophy

1. Should feel like an inet utility.
2. Be simple and effective.
3. Be performant and explicit.
4. Run in the foreground.

## Configuration

Configuration is line-based:

```text
listen <listen-proto> <listen-endpoint> <backend-proto> <backend-endpoint> [options...]
```

Example routes:

```sh
tinyproxy \
    -L "tcp :31232 tcp 127.0.0.1:41232" \
    -L "udp :31232 udp 127.0.0.1:41232"
```

Minecraft Bedrock public-to-LAN proxy

Makes a remote Minecraft Bedrock server appear as a LAN server.

```sh
tinyproxy \
    -L "udp :19132 udp 123.123.123.123:19132 broadcast_reply"
```

Run with config file:

```sh
tinyproxy -c tinyproxy.conf
```

check example/tinyproxy.conf or use `man 5 tinyproxy` after `make install-man` for details

## Non-goals

- TLS termination
- HTTP routing
- retries
- load balancing
- service discovery
- health checks
- dynamic configuration reloads
- QUIC awareness (just forward 443 udp)
- full HAProxy compatibility

Use HAProxy, Envoy, nginx, Cilium, or a real load balancer if you need those.

## Development status

This project is still work in progress. Basic funtinoality works. (See tests)

### TODO
These are the major features I want to implement before calling it done. (going into maintainance mode)

* Core
  * pthread support (planned for v0.2.x)
* Forwarding
  * IPv6 (planned for v0.2.x)
