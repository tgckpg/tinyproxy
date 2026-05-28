import asyncio
import os

from .support import (
	LISTEN_HOST,
	BACKEND_PORT,
	PROXY_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
)


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


async def read_one_or_eof(reader: asyncio.StreamReader, timeout: float = 3.0) -> bytes:
	try:
		return await asyncio.wait_for(reader.read(1), timeout=timeout)
	except ConnectionResetError:
		return b""


async def test_tcp_idle_timeout_closes_idle_connection() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{PROXY_PORT}"
		f" tcp {LISTEN_HOST}:{BACKEND_PORT}"
		f" idle_timeout=1\n"
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
			listen_port=PROXY_PORT,
			proto="tcp",
		):
			reader, writer = await asyncio.open_connection(LISTEN_HOST, PROXY_PORT)

			try:
				payload = b"before idle timeout\n"

				writer.write(payload)
				await writer.drain()

				got = await asyncio.wait_for(
					reader.readexactly(len(payload)),
					timeout=3.0,
				)
				assert got == payload, (
					f"initial roundtrip mismatch: got={got!r} expected={payload!r}"
				)

				await asyncio.sleep(2.0)

				got = await read_one_or_eof(reader, timeout=3.0)
				assert got == b"", (
					f"expected idle connection to close, got={got!r}"
				)
			finally:
				await close_writer(writer)
	finally:
		backend_server.close()
		await backend_server.wait_closed()


async def test_tcp_idle_timeout_keeps_active_connection_open() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{PROXY_PORT}"
		f" tcp {LISTEN_HOST}:{BACKEND_PORT}"
		f" idle_timeout=2\n"
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
			listen_port=PROXY_PORT,
			proto="tcp",
		):
			reader, writer = await asyncio.open_connection(LISTEN_HOST, PROXY_PORT)

			try:
				for i in range(3):
					payload = f"still-active-{i}\n".encode()

					writer.write(payload)
					await writer.drain()

					got = await asyncio.wait_for(
						reader.readexactly(len(payload)),
						timeout=3.0,
					)
					assert got == payload, (
						f"active roundtrip {i} mismatch: "
						f"got={got!r} expected={payload!r}"
					)

					await asyncio.sleep(1.0)
			finally:
				await close_writer(writer)
	finally:
		backend_server.close()
		await backend_server.wait_closed()


TESTS = [
	("test_tcp_idle_timeout_closes_idle_connection", test_tcp_idle_timeout_closes_idle_connection),
	("test_tcp_idle_timeout_keeps_active_connection_open", test_tcp_idle_timeout_keeps_active_connection_open),
]
