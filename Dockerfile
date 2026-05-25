FROM alpine:3.23 AS build

RUN apk add --no-cache build-base libevent-dev

RUN mkdir /src
WORKDIR /src

COPY [ "*.c", "*.h", "*.conf", "Makefile", "/src" ]

RUN make all

COPY tests ./tests
RUN make test

FROM alpine:3.23

RUN apk add --no-cache libevent
COPY --from=build /src/bin/tinyproxy /usr/bin/tinyproxy
COPY --from=build /src/tinyproxy.conf /etc/tinyproxy.conf

CMD [ "tinyproxy", "-c", "/etc/tinyproxy.conf" ]
