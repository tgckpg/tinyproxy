import asyncio
import os

from .support import (
	LISTEN_HOST,
	BACKEND_PORT,
	PROXY_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
)

BACKPRESSURE_MAX_BYTES = 32 * 1024 * 1024
BACKPRESSURE_CHUNK_SIZE = 64 * 1024
BACKPRESSURE_DRAIN_TIMEOUT = 0.5


async def close_writer(writer: asyncio.StreamWriter) -> None:
	try:
		writer.close()
		await writer.wait_closed()
	except ConnectionResetError:
		pass

async def abort_writer(writer: asyncio.StreamWriter) -> None:
	transport = writer.transport
	transport.abort()

	# Let the event loop process connection_lost().
	await asyncio.sleep(0)

async def blackhole_handler(
	reader: asyncio.StreamReader,
	writer: asyncio.StreamWriter,
) -> None:
	try:
		# Intentionally do not read.
		#
		# This simulates an upstream that accepted the TCP connection but
		# stopped draining. Without proxy-side backpressure, tinyproxy may keep
		# reading from the client and buffer data until memory grows without
		# bound.
		await asyncio.sleep(3600)
	finally:
		await close_writer(writer)

async def push_until_drain_blocks(
	writer: asyncio.StreamWriter,
	max_bytes: int = BACKPRESSURE_MAX_BYTES,
	chunk_size: int = BACKPRESSURE_CHUNK_SIZE,
	drain_timeout: float = BACKPRESSURE_DRAIN_TIMEOUT,
) -> int:
	chunk = b"x" * chunk_size
	written = 0

	while written < max_bytes:
		writer.write(chunk)
		written += len(chunk)

		try:
			await asyncio.wait_for(writer.drain(), timeout=drain_timeout)
		except asyncio.TimeoutError:
			return written
		except ConnectionError:
			return written
		except BrokenPipeError:
			return written

		await asyncio.sleep(0)

	return written

async def test_tcp_backpressure_when_upstream_does_not_read() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{PROXY_PORT}"
		f" tcp {LISTEN_HOST}:{BACKEND_PORT}"
		f" idle_timeout=30\n"
	)

	stop_backend = asyncio.Event()

	async def blackhole_handler(
		reader: asyncio.StreamReader,
		writer: asyncio.StreamWriter,
	) -> None:
		try:
			# Intentionally do not read from reader.
			await stop_backend.wait()
		finally:
			await abort_writer(writer)

	backend_server = await asyncio.start_server(
		blackhole_handler,
		LISTEN_HOST,
		BACKEND_PORT,
		backlog=128,
	)

	writer: asyncio.StreamWriter | None = None

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			listen_host=LISTEN_HOST,
			listen_port=PROXY_PORT,
			proto="tcp",
		):
			reader, writer = await asyncio.open_connection(LISTEN_HOST, PROXY_PORT)

			written = await asyncio.wait_for(
				push_until_drain_blocks(writer),
				timeout=10.0,
			)

			assert written < BACKPRESSURE_MAX_BYTES, (
				"proxy accepted too much client data while upstream was not reading; "
				"this likely means TCP backpressure/watermarks are missing. "
				f"wrote={written} max={BACKPRESSURE_MAX_BYTES}"
			)

			_ = reader
	finally:
		if writer is not None:
			await abort_writer(writer)

		stop_backend.set()

		backend_server.close()
		await asyncio.wait_for(backend_server.wait_closed(), timeout=3.0)

async def test_tcp_backpressure_when_client_does_not_read() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{PROXY_PORT}"
		f" tcp {LISTEN_HOST}:{BACKEND_PORT}"
		f" idle_timeout=30\n"
	)

	backend_result: asyncio.Future[tuple[str, int]] = (
		asyncio.get_running_loop().create_future()
	)

	async def pushing_backend_handler(
		reader: asyncio.StreamReader,
		writer: asyncio.StreamWriter,
	) -> None:
		try:
			# Intentionally do not wait for a client trigger.
			#
			# This test is about upstream -> proxy -> non-reading client.
			# Waiting for a trigger byte makes the test depend on the
			# client->upstream direction, which is unrelated and flaky here.
			_ = reader

			written = await push_until_drain_blocks(writer)

			if not backend_result.done():
				if written < BACKPRESSURE_MAX_BYTES:
					backend_result.set_result(("blocked", written))
				else:
					backend_result.set_result(("unbounded", written))
		except ConnectionError:
			if not backend_result.done():
				backend_result.set_result(("blocked", 0))
		except BrokenPipeError:
			if not backend_result.done():
				backend_result.set_result(("blocked", 0))
		except Exception as e:
			if not backend_result.done():
				backend_result.set_exception(e)
		finally:
			await abort_writer(writer)

	backend_server = await asyncio.start_server(
		pushing_backend_handler,
		LISTEN_HOST,
		BACKEND_PORT,
		backlog=128,
	)

	writer: asyncio.StreamWriter | None = None

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			listen_host=LISTEN_HOST,
			listen_port=PROXY_PORT,
			proto="tcp",
		):
			reader, writer = await asyncio.open_connection(LISTEN_HOST, PROXY_PORT)

			# Intentionally do not read from reader.
			_ = reader

			status, written = await asyncio.wait_for(
				backend_result,
				timeout=10.0,
			)

			assert status == "blocked", (
				"proxy accepted too much upstream data while client was not reading; "
				"this likely means reverse-direction TCP backpressure/watermarks "
				"are missing. "
				f"status={status} wrote={written} max={BACKPRESSURE_MAX_BYTES}"
			)
	finally:
		if writer is not None:
			await abort_writer(writer)

		backend_server.close()
		await asyncio.wait_for(backend_server.wait_closed(), timeout=3.0)


TESTS = [
	(
		"test_tcp_backpressure_when_upstream_does_not_read",
		test_tcp_backpressure_when_upstream_does_not_read,
	),
	(
		"test_tcp_backpressure_when_client_does_not_read",
		test_tcp_backpressure_when_client_does_not_read,
	),
]
