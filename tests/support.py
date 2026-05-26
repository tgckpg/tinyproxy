import asyncio
import os
import shutil
import subprocess
import sys
import tempfile
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass

try:
	import resource
except ImportError:
	resource = None


LISTEN_HOST = "127.0.0.1"
PROXY_PORT = 31232
BACKEND_PORT = 41232


class SkipTest(Exception):
	pass


def raise_fd_limit(wanted: int) -> None:
	if resource is None:
		return

	soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)

	if soft >= wanted:
		return

	new_soft = min(wanted, hard)

	try:
		resource.setrlimit(resource.RLIMIT_NOFILE, (new_soft, hard))
		print(f"raised RLIMIT_NOFILE soft limit: {soft} -> {new_soft}")
	except OSError as exc:
		print(f"warning: failed to raise fd limit: {exc}", file=sys.stderr)
		print(f"current RLIMIT_NOFILE soft={soft} hard={hard}", file=sys.stderr)


def find_program(env_name: str, fallback_name: str) -> str | None:
	from_env = os.environ.get(env_name)
	if from_env:
		return from_env

	return shutil.which(fallback_name)


async def wait_for_port(host: str, port: int, timeout: float = 5.0) -> None:
	deadline = time.monotonic() + timeout

	while time.monotonic() < deadline:
		try:
			reader, writer = await asyncio.open_connection(host, port)
			writer.close()
			await writer.wait_closed()
			return
		except OSError:
			await asyncio.sleep(0.05)

	raise RuntimeError(f"timed out waiting for {host}:{port}")


async def echo_handler(
	reader: asyncio.StreamReader,
	writer: asyncio.StreamWriter,
) -> None:
	try:
		while True:
			data = await reader.read(65536)
			if not data:
				return

			writer.write(data)
			await writer.drain()

	except (ConnectionResetError, BrokenPipeError):
		return

	finally:
		writer.close()

		try:
			await writer.wait_closed()
		except OSError:
			pass


async def recv_exact(reader: asyncio.StreamReader, size: int) -> bytes:
	chunks = []
	remaining = size

	while remaining > 0:
		chunk = await reader.read(min(65536, remaining))
		if not chunk:
			break

		chunks.append(chunk)
		remaining -= len(chunk)

	return b"".join(chunks)


async def proxy_roundtrip(
	payload: bytes,
	host: str = LISTEN_HOST,
	port: int = PROXY_PORT,
	timeout: float = 10.0,
) -> bytes:
	reader, writer = await asyncio.open_connection(host, port)

	try:
		writer.write(payload)
		await writer.drain()

		return await asyncio.wait_for(
			recv_exact(reader, len(payload)),
			timeout=timeout,
		)

	finally:
		writer.close()

		try:
			await writer.wait_closed()
		except OSError:
			pass


def write_temp_file(content: str, suffix: str) -> str:
	fd, path = tempfile.mkstemp(suffix=suffix, text=True)

	with os.fdopen(fd, "w") as f:
		f.write(content)

	return path


def terminate_process(name: str, proc: subprocess.Popen | None) -> None:
	if proc is None:
		return

	if proc.poll() is not None:
		return

	proc.terminate()

	try:
		proc.wait(timeout=2.0)
	except subprocess.TimeoutExpired:
		proc.kill()
		proc.wait(timeout=2.0)


def print_process_output(name: str, proc: subprocess.Popen | None) -> None:
	if proc is None:
		return

	stdout = proc.stdout.read() if proc.stdout else ""
	stderr = proc.stderr.read() if proc.stderr else ""

	if stdout:
		print(f"\n{name} stdout:")
		print(stdout)

	if stderr:
		print(f"\n{name} stderr:")
		print(stderr)


@dataclass
class TinyproxyFixture:
	proxy: subprocess.Popen
	conf_path: str
	listen_host: str
	listen_port: int


@asynccontextmanager
async def run_echo_backend(
	host: str = LISTEN_HOST,
	port: int = BACKEND_PORT,
):
	server = await asyncio.start_server(
		echo_handler,
		host,
		port,
		backlog=4096,
	)

	try:
		await wait_for_port(host, port)
		yield server
	finally:
		server.close()
		await server.wait_closed()


@asynccontextmanager
async def run_tinyproxy_with_conf(
	proxy_bin: str,
	conf_text: str,
	listen_host: str = LISTEN_HOST,
	listen_port: int = PROXY_PORT,
):
	conf_path = None
	proxy = None

	try:
		conf_path = write_temp_file(conf_text, ".conf")

		proxy = subprocess.Popen(
			[proxy_bin, "-c", conf_path],
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			text=True,
		)

		await wait_for_port(listen_host, listen_port)

		if proxy.poll() is not None:
			stderr = proxy.stderr.read() if proxy.stderr else ""
			raise RuntimeError(
				f"tinyproxy exited early with code {proxy.returncode}\n{stderr}"
			)

		yield TinyproxyFixture(
			proxy=proxy,
			conf_path=conf_path,
			listen_host=listen_host,
			listen_port=listen_port,
		)

	finally:
		terminate_process("tinyproxy", proxy)
		print_process_output("tinyproxy", proxy)

		if conf_path is not None:
			try:
				os.unlink(conf_path)
			except FileNotFoundError:
				pass


@asynccontextmanager
async def run_default_tinyproxy(proxy_bin: str):
	conf_text = (
		f"{LISTEN_HOST}:{PROXY_PORT} "
		f"{LISTEN_HOST}:{BACKEND_PORT} "
		f"tcp\n"
	)

	async with run_echo_backend(LISTEN_HOST, BACKEND_PORT):
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			listen_host=LISTEN_HOST,
			listen_port=PROXY_PORT,
		) as fixture:
			yield fixture
