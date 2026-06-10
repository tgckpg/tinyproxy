#!/usr/bin/env python3

from pathlib import Path
import struct

OUT = Path("seeds/stream_sniff")
OUT.mkdir(parents=True, exist_ok=True)


def write(name, data):
    (OUT / name).write_bytes(data)


def ext_sni(hostname: bytes) -> bytes:
    # extension_type: server_name = 0
    # extension_data:
    #   server_name_list length
    #   name_type = host_name
    #   host_name length
    #   host_name bytes
    server_name = b"\x00" + struct.pack("!H", len(hostname)) + hostname
    server_name_list = struct.pack("!H", len(server_name)) + server_name
    return struct.pack("!HH", 0, len(server_name_list)) + server_name_list


def ext_supported_versions() -> bytes:
    # supported_versions extension, client side:
    #   extension_type = 43
    #   extension_data = length-prefixed list of versions
    body = b"\x04\x03\x04\x03\x03"  # TLS 1.3, TLS 1.2
    return struct.pack("!HH", 43, len(body)) + body


def ext_alpn(proto: bytes = b"http/1.1") -> bytes:
    # ALPN extension:
    #   protocol_name_list length
    #   protocol_name length
    #   protocol_name
    name = bytes([len(proto)]) + proto
    body = struct.pack("!H", len(name)) + name
    return struct.pack("!HH", 16, len(body)) + body


def client_hello(hostname: bytes | None = b"example.com", extra_exts: bytes = b"") -> bytes:
    legacy_version = b"\x03\x03"  # TLS 1.2
    random = bytes(range(32))
    session_id = b""

    cipher_suites = b"".join([
        b"\x13\x01",  # TLS_AES_128_GCM_SHA256
        b"\x13\x02",  # TLS_AES_256_GCM_SHA384
        b"\xc0\x2f",  # TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
    ])

    compression_methods = b"\x00"

    extensions = b""
    if hostname is not None:
        extensions += ext_sni(hostname)
    extensions += ext_supported_versions()
    extensions += ext_alpn()
    extensions += extra_exts

    body = (
        legacy_version +
        random +
        bytes([len(session_id)]) + session_id +
        struct.pack("!H", len(cipher_suites)) + cipher_suites +
        bytes([len(compression_methods)]) + compression_methods +
        struct.pack("!H", len(extensions)) + extensions
    )

    handshake = (
        b"\x01" +                         # HandshakeType client_hello
        len(body).to_bytes(3, "big") +     # handshake length
        body
    )

    record = (
        b"\x16" +                         # ContentType handshake
        b"\x03\x01" +                     # TLS record legacy version
        struct.pack("!H", len(handshake)) +
        handshake
    )

    return record


# 1. Non-TLS / junk
write("junk-empty", b"")
write("junk-http-get", b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
write("junk-ssh-banner", b"SSH-2.0-OpenSSH_9.9\r\n")
write("junk-random-small", bytes([0x00, 0xff, 0x13, 0x37, 0x42]))

# 2. Partial TLS records
good = client_hello(b"example.com")
for n in [1, 2, 3, 4, 5, 6, 9, 16, 32, 64]:
    write(f"partial-{n}", good[:n])

# 3. Valid-ish ClientHello with SNI
write("tls-sni-example-com", client_hello(b"example.com"))
write("tls-sni-localhost", client_hello(b"localhost"))
write("tls-sni-long-valid-253", client_hello(b"a" * 253))
write("tls-sni-idn-punycode", client_hello(b"xn--r8jz45g.xn--zckzah"))

# 4. ClientHello without SNI
write("tls-no-sni", client_hello(None))

# 5. Malformed TLS-ish things
write("tls-record-says-zero-len", b"\x16\x03\x01\x00\x00")
write("tls-record-says-too-big", b"\x16\x03\x01\xff\xff" + b"\x01\x00\x00\x00")
write("tls-alert-not-handshake", b"\x15\x03\x03\x00\x02\x02\x28")
write("tls-appdata-not-handshake", b"\x17\x03\x03\x00\x05hello")

# 6. Bad handshake length fields
bad = bytearray(good)
bad[6:9] = b"\xff\xff\xff"  # handshake length too large
write("tls-bad-handshake-len-huge", bytes(bad))

bad = bytearray(good)
bad[6:9] = b"\x00\x00\x01"  # handshake length too small
write("tls-bad-handshake-len-small", bytes(bad))

# 7. SNI edge cases

# Declared SNI hostname length larger than available.
trunc_sni = bytearray(client_hello(b"abc"))
idx = trunc_sni.find(b"\x00\x03abc")
if idx != -1:
    trunc_sni[idx:idx + 2] = b"\x01\x00"
write("tls-sni-declared-len-too-large", bytes(trunc_sni))

# Actual hostname is 254 bytes; should hit NAME_TOO_LONG or parse error,
# depending on your implementation.
write("tls-sni-too-long-254", client_hello(b"a" * 254))

# Empty hostname.
write("tls-sni-empty", client_hello(b""))

print(f"wrote seeds to {OUT}")