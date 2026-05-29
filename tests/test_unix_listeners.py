import asyncio
import os
import socket
import sys
import tempfile

from .support import (
	LISTEN_HOST,
	BACKEND_PORT,
	SkipTest,
	echo_handler,
	start_tracked_stream_server,
	run_tinyproxy_with_conf,
)

def unix_sock_path(name):
	from pathlib import Path

	if sys.platform == "win32":
		d = Path("C:/tmp/tinyproxy-tests")
		d.mkdir(parents=True, exist_ok=True)
		return str(d / name).replace("\\", "/")

	d = tempfile.mkdtemp(prefix="tinyproxy-")
	return os.path.join(d, name)

UNIX_STREAM_SOCK = unix_sock_path("test-listen.sock")
UNIX_BUILTIN_SOCK = unix_sock_path("test-listen-2.sock")
UNIX_DGRAM_SOCK = unix_sock_path("test-listen-dgram.sock")
UNIX_DGRAM_BUILTIN_SOCK = unix_sock_path("test-listen-3.sock")

UNIX_TO_UNIX_LISTEN_SOCK = unix_sock_path("test-ping.sock")
UNIX_TO_UNIX_BACKEND_SOCK = unix_sock_path("test-pong.sock")

UNIX_DGRAM_TO_UNIX_DGRAM_LISTEN_SOCK = unix_sock_path("test-ping-dgram.sock")
UNIX_DGRAM_TO_UNIX_DGRAM_BACKEND_SOCK = unix_sock_path("test-pong-dgram.sock")

FRONT_PORT = BACKEND_PORT + 1

def unlink_if_exists(path: str) -> None:
	try:
		os.unlink(path)
	except FileNotFoundError:
		pass

async def close_writer(writer: asyncio.StreamWriter) -> None:
	try:
		writer.close()
		await writer.wait_closed()
	except ConnectionResetError:
		pass

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

async def test_unix_dgram_to_udp() -> None:
	if sys.platform.startswith("win"):
		raise SkipTest("Unix socks tests are skipped on Windows")
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
	if sys.platform.startswith("win"):
		raise SkipTest("Unix socks tests are skipped on Windows")
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

async def tcp_roundtrip(host: str, port: int, payload: bytes) -> bytes:
	reader, writer = await asyncio.open_connection(host, port)

	try:
		writer.write(payload)
		await writer.drain()

		return await asyncio.wait_for(
			reader.readexactly(len(payload)),
			timeout=3.0,
		)
	finally:
		await close_writer(writer)


async def run_stream_chain_roundtrip(
	conf_text: str,
	front_port: int,
	payload: bytes,
	socks_to_cleanup: list[str],
) -> bytes:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	for path in socks_to_cleanup:
		unlink_if_exists(path)

	backend_server = await start_tracked_stream_server(
		echo_handler,
		LISTEN_HOST,
		BACKEND_PORT,
		backlog=128,
	)

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			proto="tcp",
			listen_port=front_port,
		):
			return await tcp_roundtrip(LISTEN_HOST, front_port, payload)
	finally:
		await backend_server.close()

		for path in socks_to_cleanup:
			unlink_if_exists(path)

async def tcp_read_once(host: str, port: int) -> bytes:
	reader, writer = await asyncio.open_connection(host, port)

	try:
		return await asyncio.wait_for(reader.read(65536), timeout=3.0)
	finally:
		await close_writer(writer)

async def test_unix_stream_to_unix_stream() -> None:
	payload = b"hello unix to unix\n"

	conf_text = (
		f"listen tcp {LISTEN_HOST}:{FRONT_PORT} "
		f"unix {UNIX_TO_UNIX_LISTEN_SOCK}\n"
		f"listen unix {UNIX_TO_UNIX_LISTEN_SOCK} "
		f"unix {UNIX_TO_UNIX_BACKEND_SOCK}\n"
		f"listen unix {UNIX_TO_UNIX_BACKEND_SOCK} "
		f"tcp {LISTEN_HOST}:{BACKEND_PORT}\n"
	)

	got = await run_stream_chain_roundtrip(
		conf_text=conf_text,
		front_port=FRONT_PORT,
		payload=payload,
		socks_to_cleanup=[
			UNIX_TO_UNIX_LISTEN_SOCK,
			UNIX_TO_UNIX_BACKEND_SOCK,
		],
	)

	assert got == payload, (
		f"unix-to-unix roundtrip mismatch: "
		f"got={got!r} expected={payload!r}"
	)

async def test_unix_stream_to_tcp() -> None:
	payload = b"hello over unix stream\n"

	conf_text = (
		f"listen tcp {LISTEN_HOST}:{FRONT_PORT} "
		f"unix {UNIX_STREAM_SOCK}\n"
		f"listen unix {UNIX_STREAM_SOCK} "
		f"tcp {LISTEN_HOST}:{BACKEND_PORT}\n"
	)

	got = await run_stream_chain_roundtrip(
		conf_text=conf_text,
		front_port=FRONT_PORT,
		payload=payload,
		socks_to_cleanup=[UNIX_STREAM_SOCK],
	)

	assert got == payload, (
		f"unix stream roundtrip mismatch: got={got!r} expected={payload!r}"
	)

async def test_unix_stream_to_builtin_client_addr() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	unlink_if_exists(UNIX_BUILTIN_SOCK)

	conf_text = (
		f"listen tcp {LISTEN_HOST}:{FRONT_PORT} "
		f"unix {UNIX_BUILTIN_SOCK}\n"
		f"listen unix {UNIX_BUILTIN_SOCK} "
		f"builtin client_addr\n"
	)

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			proto="tcp",
			listen_port=FRONT_PORT,
		):
			got = await tcp_read_once(LISTEN_HOST, FRONT_PORT)

			assert got, "expected builtin client_addr response"
			assert b"unix" in got.lower() or b"unknown" in got.lower() or got.strip(), (
				f"unexpected builtin client_addr response: {got!r}"
			)
	finally:
		unlink_if_exists(UNIX_BUILTIN_SOCK)

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
	if sys.platform.startswith("win"):
		raise SkipTest("Unix socks tests are skipped on Windows")
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
