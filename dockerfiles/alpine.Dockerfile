FROM alpine:3.23 AS build

RUN --mount=type=cache,target=/var/cache/apk,sharing=locked \
	apk add --update-cache build-base libevent-dev libevent-static

RUN mkdir /build
WORKDIR /build

COPY [ "*.conf", "Makefile", "/build" ]
COPY ./mk /build/mk
COPY ./src /build/src

RUN make all STATIC=1

RUN --mount=type=cache,target=/var/cache/apk,sharing=locked \
	apk add --update-cache haproxy strace

COPY tests ./tests
RUN make test STATIC=1

RUN cp /build/bin/tinyproxy /usr/bin/

FROM scratch

COPY --from=build /build/bin/tinyproxy /usr/bin/tinyproxy

ENTRYPOINT ["/usr/bin/tinyproxy"]
