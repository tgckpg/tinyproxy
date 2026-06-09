## Which image should I use?

Use the default image unless you specifically care about maximum TCP throughput.

For static/minimal deployments:

```sh
ghcr.io/tgckpg/tinyproxy:v0.2.8
```

For high-throughput Linux deployments:

```sh
ghcr.io/tgckpg/tinyproxy:v0.2.8-debian-slim
```

The glibc image is not required for correctness. It is only a performance-oriented variant.

## Benchmarks
Please note that the throughput varies on different builds. The following data only serves as a reference.


### Baseline (Direct loopback, running on Archlinux)
Setup: `iperf3 -> iperf3`

(PENDING)

### Baseline (Rawproxy, running on Archlinux)
Setup: `iperf3 -> rawproxy -> iperf3`

(PENDING)

### glibc, built locally (running on Archlinux)
Setup: `iperf3 -> tinyproxy (glibc) -> iperf3`

(PENDING)

### static musl, built from CI (running on Archlinux)
Setup: `iperf3 -> tinyproxy (musl) -> iperf3`

(PENDING)


## Container Images

`tinyproxy` currently provides two Linux container image variants:

| Image tag                                        | Runtime               |       Size | Intended use                        |
| ------------------------------------------------ | --------------------- | ---------: | ----------------------------------- |
| `ghcr.io/tgckpg/tinyproxy:<version>`             | static musl / scratch | very small | default, minimal deployments        |
| `ghcr.io/tgckpg/tinyproxy:<version>-debian-slim` | glibc / Debian slim   |     larger | higher-throughput Linux deployments |

The default image is the static musl build. It is small in size and suitable for most uses, such as sidecars, internal forwarding, probes, and lightweight Kubernetes deployments.

```sh
docker run --rm ghcr.io/tgckpg/tinyproxy:v0.2.8 \
  -L "tcp :8080 tcp 127.0.0.1:80"
```

### glibc (debian-slim) image

The glibc image is larger because it uses a Debian slim runtime, but it may perform better for high-throughput TCP proxying on normal Linux hosts. In local benchmarks, the glibc build showed better throughput than the fully static musl build for the stream proxy hot path.

Use the glibc image for traffic-heavy routes such as:

* public `:80` / `:443` forwarding
* Git clone / push traffic
* large file transfers
* throughput-sensitive TCP routes
