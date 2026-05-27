import asyncio
import os
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path

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


async def wait_for_tcp_listen(host: str, port: int, timeout: float = 5.0) -> None:
	deadline = asyncio.get_running_loop().time() + timeout
	last_error = None

	while asyncio.get_running_loop().time() < deadline:
		try:
			reader, writer = await asyncio.open_connection(host, port)
			writer.close()
			await writer.wait_closed()
			return
		except OSError as e:
			last_error = e
			await asyncio.sleep(0.05)

	raise RuntimeError(f"timed out waiting for {host}:{port}: {last_error}")


async def test_tcp_keep_alive_roundtrip() -> None:

	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"{LISTEN_HOST}:{PROXY_PORT} "
		f"{LISTEN_HOST}:{BACKEND_PORT} "
		f"tcp keep_alive,idle_timeout=5\n"
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
				payload = b"keep-alive smoke test\n"

				writer.write(payload)
				await writer.drain()

				got = await asyncio.wait_for(
					reader.readexactly(len(payload)),
					timeout=3.0,
				)

				assert got == payload, (
					f"roundtrip mismatch: got={got!r} expected={payload!r}"
				)
			finally:
				await close_writer(writer)
	finally:
		backend_server.close()
		await backend_server.wait_closed()


async def test_tcp_keep_alive_sets_socket_option() -> None:
	if platform.system() != "Linux":
		raise SkipTest("strace keep_alive test only runs on Linux")

	strace_bin = shutil.which("strace")
	if not strace_bin:
		raise SkipTest("strace is not installed")

	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"{LISTEN_HOST}:{PROXY_PORT} "
		f"{LISTEN_HOST}:{BACKEND_PORT} "
		f"tcp keep_alive,idle_timeout=5\n"
	)

	backend_server = await asyncio.start_server(
		echo_handler,
		LISTEN_HOST,
		BACKEND_PORT,
		backlog=128,
	)

	with tempfile.TemporaryDirectory() as td:
		td_path = Path(td)
		conf_path = td_path / "tinyproxy.conf"
		trace_path = td_path / "strace.log"

		conf_path.write_text(conf_text)

		proxy = subprocess.Popen(
			[
				strace_bin,
				"-f",
				"-e",
				"trace=setsockopt",
				"-o",
				str(trace_path),
				proxy_bin,
				"-c",
				str(conf_path),
			],
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			text=True,
		)

		try:
			await wait_for_tcp_listen(LISTEN_HOST, PROXY_PORT)

			reader, writer = await asyncio.open_connection(LISTEN_HOST, PROXY_PORT)

			try:
				payload = b"trigger upstream connect\n"

				writer.write(payload)
				await writer.drain()

				got = await asyncio.wait_for(
					reader.readexactly(len(payload)),
					timeout=3.0,
				)

				assert got == payload, (
					f"roundtrip mismatch: got={got!r} expected={payload!r}"
				)
			finally:
				await close_writer(writer)
		finally:
			proxy.terminate()

			try:
				proxy.wait(timeout=3.0)
			except subprocess.TimeoutExpired:
				proxy.kill()
				proxy.wait(timeout=3.0)

			backend_server.close()
			await backend_server.wait_closed()

		trace_text = trace_path.read_text(errors="replace")

		keepalive_count = trace_text.count("SO_KEEPALIVE")

		assert keepalive_count >= 2, (
			"expected SO_KEEPALIVE to be enabled on both client and upstream sockets; "
			f"found {keepalive_count} calls\n\nstrace output:\n{trace_text}"
		)


TESTS = [
	("test_tcp_keep_alive_roundtrip", test_tcp_keep_alive_roundtrip),
	("test_tcp_keep_alive_sets_socket_option", test_tcp_keep_alive_sets_socket_option),
]
