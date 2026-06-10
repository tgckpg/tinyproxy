FROM debian:trixie-slim AS build

ARG LIBEVENT_TARBALL
ARG BUILD_VER

RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		python3 \
		build-essential \
		ca-certificates \
		pkg-config \
		file \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY ${LIBEVENT_TARBALL} /build/libevent.tar.gz
COPY tests /build/tests
COPY src /build/src
COPY mk /build/mk
COPY Makefile /build/Makefile

# We've already ran the real test in earlier build stages
# Reduce the stress test requirments for faster build here
RUN mkdir -p /build/libevent \
	&& tar zxf /build/libevent.tar.gz -C /build/libevent --strip-components=1 \
	&& find . \
	&& make clean test LIBEVENT_SRC=./libevent \
		CONCURRENCY=20 \
		TOTAL=20 \
		FD_LIMIT=512 \
		LARGE_ROUNDTRIP_SIZE=4096 \
		SEQUENTIAL_CONNECTIONS=20 \
		VERSION=${BUILD_VER}

FROM debian:trixie-slim AS runtime

COPY --from=build /build/bin/tinyproxy /usr/local/bin/tinyproxy

ENTRYPOINT ["/usr/local/bin/tinyproxy"]
