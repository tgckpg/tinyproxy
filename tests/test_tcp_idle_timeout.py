import asyncio
import os

from .support import (
	LISTEN_HOST,
	BACKEND_PORT,
	PROXY_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
	start_tracked_stream_server,
)

async def delayed_response_upload_handler(
	reader: asyncio.StreamReader,
	writer: asyncio.StreamWriter,
) -> None:
	try:
		try:
			header = await reader.readuntil(b"\r\n\r\n")
		except asyncio.IncompleteReadError as e:
			# The test harness/proxy readiness check may open a TCP
			# connection and close it without sending any request bytes.
			# Ignore that. A partial HTTP request is still a real failure.
			if not e.partial:
				return
			raise AssertionError(f"incomplete request headers: {e.partial!r}") from e

		content_length = None
		for line in header.decode("latin1", errors="replace").split("\r\n"):
			if line.lower().startswith("content-length:"):
				content_length = int(line.split(":", 1)[1].strip())
				break

		assert content_length is not None, "missing Content-Length"

		received = 0
		while received < content_length:
			chunk = await reader.read(min(65536, content_length - received))
			if not chunk:
				break
			received += len(chunk)

		assert received == content_length, (
			f"upload body truncated: received={received} expected={content_length}"
		)

		body = b"ok\n"
		writer.write(
			b"HTTP/1.1 200 OK\r\n"
			+ f"Content-Length: {len(body)}\r\n".encode("ascii")
			+ b"Connection: close\r\n"
			+ b"\r\n"
			+ body
		)
		await writer.drain()

	finally:
		writer.close()
		await writer.wait_closed()

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
		await backend_server.close()


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
		await backend_server.close()

async def test_tcp_idle_timeout_keeps_long_one_way_upload_open() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{PROXY_PORT}"
		f" tcp {LISTEN_HOST}:{BACKEND_PORT}"
		f" idle_timeout=2\n"
	)

	backend_server = await start_tracked_stream_server(
		delayed_response_upload_handler,
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
				body_size = 6 * 1024 * 1024
				chunk_size = 64 * 1024
				delay = 0.05

				req = (
					b"PUT /v2/test/blobs/uploads/fake?digest=sha256:deadbeef HTTP/1.1\r\n"
					b"Host: fake-registry\r\n"
					b"User-Agent: test-buildkit/repro\r\n"
					b"Content-Type: application/octet-stream\r\n"
					+ f"Content-Length: {body_size}\r\n".encode("ascii")
					+ b"Connection: close\r\n"
					+ b"\r\n"
				)

				writer.write(req)
				await writer.drain()

				chunk = b"x" * chunk_size
				sent = 0

				while sent < body_size:
					n = min(chunk_size, body_size - sent)
					writer.write(chunk[:n])
					await writer.drain()
					sent += n
					await asyncio.sleep(delay)

				resp = await asyncio.wait_for(reader.read(), timeout=5.0)

				assert b"HTTP/1.1 200 OK" in resp, resp
				assert resp.endswith(b"\r\n\r\nok\n"), resp
			finally:
				await close_writer(writer)
	finally:
		await backend_server.close()

TESTS = [
	("test_tcp_idle_timeout_closes_idle_connection", test_tcp_idle_timeout_closes_idle_connection),
	("test_tcp_idle_timeout_keeps_active_connection_open", test_tcp_idle_timeout_keeps_active_connection_open),
	("test_tcp_idle_timeout_keeps_long_one_way_upload_open", test_tcp_idle_timeout_keeps_long_one_way_upload_open),
]
