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
    -L "udp :19132 udp 123.123.123.123:19132 broadcast_reply=listen"
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
- QUIC awareness (just forward 443 udp)
- full HAProxy compatibility

Use HAProxy, Envoy, nginx, Cilium, or a real load balancer if you need those.

## Development status

Before releasing v1.0.0

I'm testing this in prod right now. This might take a long time since I'm one person.

If you found this useful and want to help please do. I really need some help on writing the man pages.

### TODO

These are features that would be nice to have, but I don't need them in my Kubernetes environment.

- Config auto-reload
  - Resolution: just restart it.
  - This ensures a cleaner state. Reloading configuration is hard to handle properly.

- Health checks
  - Resolution: let Kubernetes handle them.

- Domain support
  - Resolution: use an external tool to rewrite the config and restart the proxy.
  - For my setup, I would use initContainers for this.
  - In general, when something requires hostname resolution, I usually end up using a full-featured server instead, such as HAProxy or nginx.
