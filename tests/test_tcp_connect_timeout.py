import asyncio
import os
import time

from .support import (
	LISTEN_HOST,
	PROXY_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
)


CONNECT_TIMEOUT_PROXY_PORT = PROXY_PORT + 20
BLACKHOLE_HOST = "192.0.2.1"
BLACKHOLE_PORT = 65000


async def close_writer(writer: asyncio.StreamWriter) -> None:
	try:
		writer.close()
		await writer.wait_closed()
	except ConnectionResetError:
		pass


async def read_one_or_eof(reader: asyncio.StreamReader, timeout: float = 5.0) -> bytes:
	try:
		return await asyncio.wait_for(reader.read(1), timeout=timeout)
	except ConnectionResetError:
		return b""


async def test_tcp_connect_timeout_closes_client_connection() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{CONNECT_TIMEOUT_PROXY_PORT}"
		f" tcp {BLACKHOLE_HOST}:{BLACKHOLE_PORT}"
		f" connect_timeout=1,idle_timeout=30\n"
	)

	async with run_tinyproxy_with_conf(
		proxy_bin=proxy_bin,
		conf_text=conf_text,
		listen_host=LISTEN_HOST,
		listen_port=CONNECT_TIMEOUT_PROXY_PORT,
		proto="tcp",
	):
		reader, writer = await asyncio.open_connection(LISTEN_HOST, CONNECT_TIMEOUT_PROXY_PORT)

		try:
			start = time.monotonic()

			writer.write(b"hello before upstream connect\n")
			await writer.drain()

			got = await read_one_or_eof(reader, timeout=5.0)
			elapsed = time.monotonic() - start

			assert got == b"", (
				f"expected proxy to close client after connect timeout, got={got!r}"
			)

			assert elapsed >= 0.8, (
				f"connect timeout fired too early: elapsed={elapsed:.3f}s"
			)

			assert elapsed < 4.0, (
				f"connect timeout took too long: elapsed={elapsed:.3f}s"
			)
		finally:
			await close_writer(writer)


async def test_tcp_connect_timeout_does_not_replace_idle_timeout_after_connect() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

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

	from .support import BACKEND_PORT

	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{CONNECT_TIMEOUT_PROXY_PORT}"
		f" tcp {LISTEN_HOST}:{BACKEND_PORT}"
		f" connect_timeout=1,idle_timeout=2\n"
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
			listen_host=LISTEN_HOST,
			listen_port=CONNECT_TIMEOUT_PROXY_PORT,
			proto="tcp",
		):
			reader, writer = await asyncio.open_connection(LISTEN_HOST, CONNECT_TIMEOUT_PROXY_PORT)

			try:
				payload = b"connected path still works\n"

				writer.write(payload)
				await writer.drain()

				got = await asyncio.wait_for(
					reader.readexactly(len(payload)),
					timeout=3.0,
				)
				assert got == payload, (
					f"roundtrip mismatch: got={got!r} expected={payload!r}"
				)

				await asyncio.sleep(1.2)

				payload = b"still alive after connect timeout duration\n"

				writer.write(payload)
				await writer.drain()

				got = await asyncio.wait_for(
					reader.readexactly(len(payload)),
					timeout=3.0,
				)
				assert got == payload, (
					f"connection died as if connect_timeout remained active: "
					f"got={got!r} expected={payload!r}"
				)

				await asyncio.sleep(2.5)

				got = await read_one_or_eof(reader, timeout=3.0)
				assert got == b"", (
					f"expected idle timeout to close established connection, got={got!r}"
				)
			finally:
				await close_writer(writer)
	finally:
		backend_server.close()
		await backend_server.wait_closed()


TESTS = [
	(
		"test_tcp_connect_timeout_closes_client_connection",
		test_tcp_connect_timeout_closes_client_connection,
	),
	(
		"test_tcp_connect_timeout_does_not_replace_idle_timeout_after_connect",
		test_tcp_connect_timeout_does_not_replace_idle_timeout_after_connect,
	),
]
