FROM alpine:3.23 AS build

RUN --mount=type=cache,target=/var/cache/apk,sharing=locked \
	apk add --update-cache build-base libevent-dev libevent-static

RUN mkdir /src
WORKDIR /src

COPY [ "*.c", "*.h", "*.conf", "Makefile", "/src" ]

RUN make all STATIC=1

RUN --mount=type=cache,target=/var/cache/apk,sharing=locked \
	apk add --update-cache haproxy strace

COPY tests ./tests
RUN make test STATIC=1

RUN cp /src/tinyproxy.conf /etc/ && cp /src/bin/tinyproxy /usr/bin/

FROM scratch

COPY --from=build /src/bin/tinyproxy /usr/bin/tinyproxy
COPY --from=build /src/tinyproxy.conf /etc/tinyproxy.conf

ENTRYPOINT ["/usr/bin/tinyproxy"]
CMD ["-c", "/etc/tinyproxy.conf"]
