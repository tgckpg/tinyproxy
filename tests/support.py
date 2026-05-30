import asyncio
import os
import shutil
import subprocess
import sys
import tempfile
import time
import struct
import ipaddress
from contextlib import asynccontextmanager
from dataclasses import dataclass

try:
	import resource
except ImportError:
	resource = None


LISTEN_HOST = "127.0.0.1"
PROXY_PORT = 31232
BACKEND_PORT = 41232

PROXY_V2_SIG = b"\r\n\r\n\x00\r\nQUIT\n"


class SkipTest(Exception):
	pass


class UDPClientProtocol(asyncio.DatagramProtocol):
	def __init__(self, payload: bytes, future: asyncio.Future[bytes]) -> None:
		self.payload = payload
		self.future = future
		self.transport: asyncio.DatagramTransport | None = None

	def connection_made(self, transport: asyncio.BaseTransport) -> None:
		self.transport = transport  # type: ignore[assignment]
		self.transport.sendto(self.payload)

	def datagram_received(self, data: bytes, addr) -> None:
		if not self.future.done():
			self.future.set_result(data)

	def error_received(self, exc: Exception) -> None:
		if not self.future.done():
			self.future.set_exception(exc)

class UDPProxyV2EchoServerProtocol(asyncio.DatagramProtocol):
	def __init__(self) -> None:
		self.transport: asyncio.DatagramTransport | None = None
		self.error: Exception | None = None
		self.last_src: tuple[str, int] | None = None
		self.last_dst: tuple[str, int] | None = None
		self.last_raw: bytes | None = None

	def connection_made(self, transport: asyncio.BaseTransport) -> None:
		self.transport = transport  # type: ignore[assignment]

	def datagram_received(self, data: bytes, addr) -> None:
		self.last_raw = data

		try:
			src, dst, payload = parse_proxy_v2_udp4_packet(data)
		except Exception as exc:
			self.error = exc
			return

		self.last_src = src
		self.last_dst = dst

		if self.transport is not None:
			self.transport.sendto(payload, addr)

class UDPRoundtripClientProtocol(asyncio.DatagramProtocol):
	def __init__(self) -> None:
		self.transport: asyncio.DatagramTransport | None = None
		self.pending: asyncio.Future[bytes] | None = None

	def connection_made(self, transport: asyncio.BaseTransport) -> None:
		self.transport = transport  # type: ignore[assignment]

	def datagram_received(self, data: bytes, addr) -> None:
		if self.pending is not None and not self.pending.done():
			self.pending.set_result(data)

	def error_received(self, exc: Exception) -> None:
		if self.pending is not None and not self.pending.done():
			self.pending.set_exception(exc)

	async def roundtrip(self, payload: bytes, timeout: float = 3.0) -> bytes:
		if self.transport is None:
			raise RuntimeError("UDP transport is not ready")

		if self.pending is not None and not self.pending.done():
			raise RuntimeError("UDP roundtrip already in progress")

		loop = asyncio.get_running_loop()
		self.pending = loop.create_future()

		self.transport.sendto(payload)

		try:
			return await asyncio.wait_for(self.pending, timeout=timeout)
		finally:
			self.pending = None

@asynccontextmanager
async def udp_proxy_client():
	loop = asyncio.get_running_loop()

	transport, protocol = await loop.create_datagram_endpoint(
		UDPRoundtripClientProtocol,
		remote_addr=(LISTEN_HOST, PROXY_PORT),
	)

	try:
		yield protocol
	finally:
		transport.close()

def parse_proxy_v2_udp4_packet(data: bytes) -> tuple[tuple[str, int], tuple[str, int], bytes]:
	if len(data) < 28:
		raise ValueError(f"packet too short for PROXY v2 UDP4 header: {len(data)} bytes")

	if data[:12] != PROXY_V2_SIG:
		raise ValueError("bad PROXY v2 signature")

	ver_cmd = data[12]
	if ver_cmd != 0x21:
		raise ValueError(f"bad PROXY v2 version/cmd: 0x{ver_cmd:02x}")

	fam_proto = data[13]
	if fam_proto != 0x12:
		raise ValueError(f"bad PROXY v2 family/proto: 0x{fam_proto:02x}")

	addr_len = struct.unpack("!H", data[14:16])[0]
	if addr_len != 12:
		raise ValueError(f"bad PROXY v2 UDP4 addr_len: {addr_len}")

	src_ip = str(ipaddress.IPv4Address(data[16:20]))
	dst_ip = str(ipaddress.IPv4Address(data[20:24]))
	src_port, dst_port = struct.unpack("!HH", data[24:28])

	payload = data[28:]

	return (src_ip, src_port), (dst_ip, dst_port), payload

async def udp_proxy_roundtrip(payload: bytes, timeout: float = 3.0) -> bytes:
	loop = asyncio.get_running_loop()
	future: asyncio.Future[bytes] = loop.create_future()

	transport, _protocol = await loop.create_datagram_endpoint(
		lambda: UDPClientProtocol(payload, future),
		remote_addr=(LISTEN_HOST, PROXY_PORT),
	)

	try:
		return await asyncio.wait_for(future, timeout=timeout)
	finally:
		transport.close()

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
class TrackedStreamServer:
	server: asyncio.AbstractServer
	tasks: set[asyncio.Task]

	async def close(self, timeout: float = 3.0) -> None:
		self.server.close()
		await self.server.wait_closed()

		if not self.tasks:
			return

		done, pending = await asyncio.wait(self.tasks, timeout=timeout)

		for task in pending:
			task.cancel()

		if pending:
			await asyncio.gather(*pending, return_exceptions=True)

		for task in done:
			try:
				task.result()
			except asyncio.CancelledError:
				pass


async def start_tracked_stream_server(
	handler,
	host: str,
	port: int,
	backlog: int = 128,
) -> TrackedStreamServer:
	tasks: set[asyncio.Task] = set()

	async def tracked_handler(
		reader: asyncio.StreamReader,
		writer: asyncio.StreamWriter,
	) -> None:
		task = asyncio.current_task()
		if task is not None:
			tasks.add(task)

		try:
			await handler(reader, writer)
		finally:
			if task is not None:
				tasks.discard(task)

	server = await asyncio.start_server(
		tracked_handler,
		host,
		port,
		backlog=backlog,
	)

	return TrackedStreamServer(server=server, tasks=tasks)


@asynccontextmanager
async def run_tracked_stream_server(
	handler,
	host: str,
	port: int,
	backlog: int = 128,
	close_timeout: float = 3.0,
):
	tracked = await start_tracked_stream_server(
		handler,
		host,
		port,
		backlog=backlog,
	)

	try:
		yield tracked.server
	finally:
		await tracked.close(timeout=close_timeout)


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
	async with run_tracked_stream_server(
		echo_handler,
		host,
		port,
		backlog=4096,
	) as server:
		await wait_for_port(host, port)
		yield server

@asynccontextmanager
async def run_tinyproxy_with_conf(
	proxy_bin: str,
	conf_text: str | None = None,
	args: list[str] | None = None,
	listen_host: str = LISTEN_HOST,
	listen_port: int = PROXY_PORT,
	proto: str = "tcp",
):
	conf_path = None
	proxy = None

	try:
		cmd = [proxy_bin]

		if conf_text is not None:
			conf_path = write_temp_file(conf_text, ".conf")
			cmd += ["-c", conf_path]

		if args:
			cmd += args

		if conf_text is None and not args:
			raise ValueError("run_tinyproxy_with_conf requires conf_text or args")

		proxy = subprocess.Popen(
			cmd,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			text=True,
		)

		if proto == "tcp":
			await wait_for_port(listen_host, listen_port)
		else:
			await asyncio.sleep(0.1)

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
async def run_default_tcp_tinyproxy(proxy_bin: str):
	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{PROXY_PORT}"
		f" tcp {LISTEN_HOST}:{BACKEND_PORT}\n"
	)

	async with run_echo_backend(LISTEN_HOST, BACKEND_PORT):
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			args=["-w", "2"],
			listen_host=LISTEN_HOST,
			listen_port=PROXY_PORT,
			proto="tcp",
		) as fixture:
			yield fixture

@asynccontextmanager
async def run_default_udp_tinyproxy(proxy_bin: str):
	conf_text = (
		f"listen"
		f" udp {LISTEN_HOST}:{PROXY_PORT}"
		f" udp {LISTEN_HOST}:{BACKEND_PORT}\n"
	)

	async with run_udp_echo_backend(LISTEN_HOST, BACKEND_PORT):
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			listen_host=LISTEN_HOST,
			listen_port=PROXY_PORT,
			proto="udp",
		) as fixture:
			yield fixture

class UDPEchoServerProtocol(asyncio.DatagramProtocol):
	def connection_made(self, transport: asyncio.BaseTransport) -> None:
		self.transport = transport  # type: ignore[assignment]

	def datagram_received(self, data: bytes, addr) -> None:
		self.transport.sendto(data, addr)


@asynccontextmanager
async def run_udp_echo_backend(host: str, port: int):
	loop = asyncio.get_running_loop()

	transport, _protocol = await loop.create_datagram_endpoint(
		UDPEchoServerProtocol,
		local_addr=(host, port),
	)

	try:
		yield
	finally:
		transport.close()


@asynccontextmanager
async def run_udp_proxy_v2_echo_backend(host: str, port: int):
	loop = asyncio.get_running_loop()

	transport, protocol = await loop.create_datagram_endpoint(
		UDPProxyV2EchoServerProtocol,
		local_addr=(host, port),
	)

	try:
		yield protocol
	finally:
		transport.close()
