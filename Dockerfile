FROM --platform=$BUILDPLATFORM alpine:3.23 AS build

RUN --mount=type=cache,target=/var/cache/apk,sharing=locked \
	apk add --update-cache build-base libevent-dev libevent-static

RUN mkdir /src
WORKDIR /src

COPY [ "*.c", "*.h", "*.conf", "Makefile", "/src" ]

RUN make all

COPY tests ./tests
RUN STATIC=1 make test

RUN cp /src/tinyproxy.conf /etc/ && cp /src/bin/tinyproxy /usr/bin/

FROM scratch

COPY --from=build /src/bin/tinyproxy /usr/bin/tinyproxy
COPY --from=build /src/tinyproxy.conf /etc/tinyproxy.conf

RUN /usr/bin/tinyproxy

CMD [ "tinyproxy", "-c", "/etc/tinyproxy.conf" ]
