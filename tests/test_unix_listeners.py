import asyncio
import os
import socket

from .support import (
	LISTEN_HOST,
	BACKEND_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
)


UNIX_STREAM_SOCK = "/tmp/test-listen.sock"
UNIX_BUILTIN_SOCK = "/tmp/test-listen-2.sock"
UNIX_DGRAM_SOCK = "/tmp/test-listen-dgram.sock"
UNIX_DGRAM_BUILTIN_SOCK = "/tmp/test-listen-3.sock"

UNIX_TO_UNIX_LISTEN_SOCK = "/tmp/test-ping.sock"
UNIX_TO_UNIX_BACKEND_SOCK = "/tmp/test-pong.sock"

UNIX_DGRAM_TO_UNIX_DGRAM_LISTEN_SOCK = "/tmp/test-ping-dgram.sock"
UNIX_DGRAM_TO_UNIX_DGRAM_BACKEND_SOCK = "/tmp/test-pong-dgram.sock"

def unlink_if_exists(path: str) -> None:
	try:
		os.unlink(path)
	except FileNotFoundError:
		pass


async def echo_handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
	try:
		while True:
			data = await reader.read(65536)
			if not data:
				break

			writer.write(data)
			await writer.drain()
	finally:
		writer.close()
		await writer.wait_closed()


async def close_writer(writer: asyncio.StreamWriter) -> None:
	try:
		writer.close()
		await writer.wait_closed()
	except ConnectionResetError:
		pass


async def open_unix_connection(path: str):
	return await asyncio.open_unix_connection(path)


async def udp_echo_server(host: str, port: int):
	loop = asyncio.get_running_loop()

	class EchoProtocol(asyncio.DatagramProtocol):
		def connection_made(self, transport):
			self.transport = transport

		def datagram_received(self, data, addr):
			self.transport.sendto(data, addr)

	return await loop.create_datagram_endpoint(
		lambda: EchoProtocol(),
		local_addr=(host, port),
	)


def unix_dgram_roundtrip(sock_path: str, payload: bytes) -> bytes:
	client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
	client.settimeout(3.0)

	client_path = f"/tmp/tinyproxy-test-client-{os.getpid()}-{id(client)}.sock"
	unlink_if_exists(client_path)

	try:
		client.bind(client_path)
		client.sendto(payload, sock_path)
		return client.recv(65536)
	finally:
		client.close()
		unlink_if_exists(client_path)


async def test_unix_stream_to_tcp() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	unlink_if_exists(UNIX_STREAM_SOCK)

	conf_text = (
		f"listen unix {UNIX_STREAM_SOCK} tcp {LISTEN_HOST}:{BACKEND_PORT}\n"
	)

	backend_server = await asyncio.start_server(
		echo_handler,
		LISTEN_HOST,
		BACKEND_PORT,
		backlog=128,
	)

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			proto="unix",
		):
			reader, writer = await open_unix_connection(UNIX_STREAM_SOCK)

			try:
				payload = b"hello over unix stream\n"

				writer.write(payload)
				await writer.drain()

				got = await asyncio.wait_for(
					reader.readexactly(len(payload)),
					timeout=3.0,
				)

				assert got == payload, (
					f"unix stream roundtrip mismatch: got={got!r} expected={payload!r}"
				)
			finally:
				await close_writer(writer)
	finally:
		backend_server.close()
		await backend_server.wait_closed()
		unlink_if_exists(UNIX_STREAM_SOCK)


async def test_unix_stream_to_builtin_client_addr() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	unlink_if_exists(UNIX_BUILTIN_SOCK)

	conf_text = (
		f"listen unix {UNIX_BUILTIN_SOCK} builtin client_addr\n"
	)

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			proto="unix",
		):
			reader, writer = await open_unix_connection(UNIX_BUILTIN_SOCK)

			try:
				got = await asyncio.wait_for(reader.read(65536), timeout=3.0)

				assert got, "expected builtin client_addr response"
				assert b"unix" in got.lower() or b"unknown" in got.lower() or got.strip(), (
					f"unexpected builtin client_addr response: {got!r}"
				)
			finally:
				await close_writer(writer)
	finally:
		unlink_if_exists(UNIX_BUILTIN_SOCK)


async def test_unix_dgram_to_udp() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	unlink_if_exists(UNIX_DGRAM_SOCK)

	conf_text = (
		f"listen unix-dgram {UNIX_DGRAM_SOCK} udp {LISTEN_HOST}:{BACKEND_PORT}\n"
	)

	transport, _ = await udp_echo_server(LISTEN_HOST, BACKEND_PORT)

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			proto="unix-dgram",
		):
			payload = b"hello over unix datagram\n"

			got = await asyncio.to_thread(
				unix_dgram_roundtrip,
				UNIX_DGRAM_SOCK,
				payload,
			)

			assert got == payload, (
				f"unix dgram roundtrip mismatch: got={got!r} expected={payload!r}"
			)
	finally:
		transport.close()
		unlink_if_exists(UNIX_DGRAM_SOCK)


async def test_unix_dgram_to_builtin_client_addr() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	unlink_if_exists(UNIX_DGRAM_BUILTIN_SOCK)

	conf_text = (
		f"listen unix-dgram {UNIX_DGRAM_BUILTIN_SOCK} builtin client_addr\n"
	)

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			proto="unix-dgram",
		):
			got = await asyncio.to_thread(
				unix_dgram_roundtrip,
				UNIX_DGRAM_BUILTIN_SOCK,
				b"ignored\n",
			)

			assert got, "expected builtin client_addr datagram response"
	finally:
		unlink_if_exists(UNIX_DGRAM_BUILTIN_SOCK)

async def test_unix_stream_to_unix_stream() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	unlink_if_exists(UNIX_TO_UNIX_LISTEN_SOCK)
	unlink_if_exists(UNIX_TO_UNIX_BACKEND_SOCK)

	conf_text = (
		f"listen unix {UNIX_TO_UNIX_LISTEN_SOCK} "
		f"unix {UNIX_TO_UNIX_BACKEND_SOCK}\n"
	)

	backend_server = await asyncio.start_unix_server(
		echo_handler,
		path=UNIX_TO_UNIX_BACKEND_SOCK,
		backlog=128,
	)

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			proto="unix",
		):
			reader, writer = await open_unix_connection(UNIX_TO_UNIX_LISTEN_SOCK)

			try:
				payload = b"hello unix to unix\n"

				writer.write(payload)
				await writer.drain()

				got = await asyncio.wait_for(
					reader.readexactly(len(payload)),
					timeout=3.0,
				)

				assert got == payload, (
					f"unix-to-unix roundtrip mismatch: "
					f"got={got!r} expected={payload!r}"
				)
			finally:
				await close_writer(writer)
	finally:
		backend_server.close()
		await backend_server.wait_closed()
		unlink_if_exists(UNIX_TO_UNIX_LISTEN_SOCK)
		unlink_if_exists(UNIX_TO_UNIX_BACKEND_SOCK)

async def unix_dgram_echo_server(path: str):
	loop = asyncio.get_running_loop()

	class EchoProtocol(asyncio.DatagramProtocol):
		def connection_made(self, transport):
			self.transport = transport

		def datagram_received(self, data, addr):
			self.transport.sendto(data, addr)

	return await loop.create_datagram_endpoint(
		lambda: EchoProtocol(),
		local_addr=path,
		family=socket.AF_UNIX,
	)

async def test_unix_dgram_to_unix_dgram() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	unlink_if_exists(UNIX_DGRAM_TO_UNIX_DGRAM_LISTEN_SOCK)
	unlink_if_exists(UNIX_DGRAM_TO_UNIX_DGRAM_BACKEND_SOCK)

	conf_text = (
		f"listen unix-dgram {UNIX_DGRAM_TO_UNIX_DGRAM_LISTEN_SOCK} "
		f"unix-dgram {UNIX_DGRAM_TO_UNIX_DGRAM_BACKEND_SOCK}\n"
	)

	transport, _ = await unix_dgram_echo_server(
		UNIX_DGRAM_TO_UNIX_DGRAM_BACKEND_SOCK,
	)

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			proto="unix-dgram",
		):
			payload = b"hello unix-dgram to unix-dgram\n"

			got = await asyncio.to_thread(
				unix_dgram_roundtrip,
				UNIX_DGRAM_TO_UNIX_DGRAM_LISTEN_SOCK,
				payload,
			)

			assert got == payload, (
				f"unix-dgram-to-unix-dgram roundtrip mismatch: "
				f"got={got!r} expected={payload!r}"
			)
	finally:
		transport.close()
		unlink_if_exists(UNIX_DGRAM_TO_UNIX_DGRAM_LISTEN_SOCK)
		unlink_if_exists(UNIX_DGRAM_TO_UNIX_DGRAM_BACKEND_SOCK)

TESTS = [
	("test_unix_stream_to_tcp", test_unix_stream_to_tcp),
	("test_unix_stream_to_builtin_client_addr", test_unix_stream_to_builtin_client_addr),
	("test_unix_dgram_to_udp", test_unix_dgram_to_udp),
	("test_unix_dgram_to_builtin_client_addr", test_unix_dgram_to_builtin_client_addr),
	("test_unix_stream_to_unix_stream", test_unix_stream_to_unix_stream),
	("test_unix_dgram_to_unix_dgram", test_unix_dgram_to_unix_dgram),
]
