FROM scratch

ARG TARGETARCH

COPY rootfs-${TARGETARCH}/usr/bin/tinyproxy /usr/bin/tinyproxy

ENTRYPOINT ["/usr/bin/tinyproxy"]
