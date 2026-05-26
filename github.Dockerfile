FROM scratch

ARG TARGETARCH

COPY rootfs-${TARGETARCH}/usr/bin/tinyproxy /usr/bin/tinyproxy
COPY rootfs-${TARGETARCH}/etc/tinyproxy.conf /etc/tinyproxy.conf

ENTRYPOINT ["/usr/bin/tinyproxy"]
CMD ["-c", "/etc/tinyproxy.conf"]
