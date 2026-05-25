#!/usr/bin/env python3

import asyncio
import os
import resource
import signal
import subprocess
import sys
import time
import tempfile

LISTEN_HOST = "127.0.0.1"
PROXY_PORT = 31232
BACKEND_PORT = 41232

def raise_fd_limit(wanted: int) -> None:
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


async def echo_handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
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


async def proxy_roundtrip(payload: bytes, timeout: float = 10.0) -> bytes:
	reader, writer = await asyncio.open_connection(LISTEN_HOST, PROXY_PORT)

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


async def test_small_roundtrip() -> None:
	payload = b"hello through proxy\n"
	got = await proxy_roundtrip(payload)
	assert got == payload, f"small roundtrip mismatch: {got!r}"


async def test_large_roundtrip() -> None:
	payload = b"0123456789abcdef" * 131072  # 2 MiB
	got = await proxy_roundtrip(payload, timeout=30.0)
	assert got == payload, f"large roundtrip mismatch: got {len(got)} bytes"


async def test_many_sequential_connections(count: int) -> None:
	for i in range(count):
		payload = f"message-{i}\n".encode()
		got = await proxy_roundtrip(payload)
		assert got == payload, f"sequential connection {i} failed"


async def run_one_concurrent(i: int, payload_size: int, sem: asyncio.Semaphore) -> None:
	async with sem:
		prefix = f"worker-{i}-".encode()
		payload = (prefix * ((payload_size // len(prefix)) + 1))[:payload_size]

		got = await proxy_roundtrip(payload, timeout=30.0)

		if got != payload:
			raise AssertionError(
				f"worker {i} mismatch: expected {len(payload)} bytes, got {len(got)}"
			)


async def test_concurrent_connections(
	total: int,
	concurrency: int,
	payload_size: int,
) -> None:
	sem = asyncio.Semaphore(concurrency)

	tasks = [
		asyncio.create_task(run_one_concurrent(i, payload_size, sem))
		for i in range(total)
	]

	done = 0

	for task in asyncio.as_completed(tasks):
		await task
		done += 1

		if done % 1000 == 0:
			print(f"  completed {done}/{total}")


async def main_async(proxy_bin: str) -> int:
	raise_fd_limit(65535)

	backend_server = await asyncio.start_server(
		echo_handler,
		LISTEN_HOST,
		BACKEND_PORT,
		backlog=4096,
	)

	with tempfile.NamedTemporaryFile("w", suffix=".conf") as f:
		f.write(f"{LISTEN_HOST}:{PROXY_PORT} {LISTEN_HOST}:{BACKEND_PORT} tcp\n")
		f.flush()

		proxy = subprocess.Popen(
			[proxy_bin, "-c", f.name],
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			text=True,
		)

		await asyncio.sleep(0.2)

		if proxy.poll() is not None:
			stderr = proxy.stderr.read() if proxy.stderr else ""
			raise RuntimeError(
				f"proxy exited early with code {proxy.returncode}\n{stderr}"
			)

		try:
			await wait_for_port(LISTEN_HOST, BACKEND_PORT)
			await wait_for_port(LISTEN_HOST, PROXY_PORT)

			print("running test_small_roundtrip...")
			await test_small_roundtrip()
			print("ok test_small_roundtrip")

			print("running test_large_roundtrip...")
			await test_large_roundtrip()
			print("ok test_large_roundtrip")

			print("running test_many_sequential_connections 1000...")
			await test_many_sequential_connections(1000)
			print("ok test_many_sequential_connections")

			total = int(os.environ.get("TOTAL", "10000"))
			concurrency = int(os.environ.get("CONCURRENCY", "10000"))
			payload_size = int(os.environ.get("PAYLOAD_SIZE", "1024"))

			print(
				f"running test_concurrent_connections "
				f"total={total} concurrency={concurrency} payload_size={payload_size}..."
			)

			await test_concurrent_connections(
				total=total,
				concurrency=concurrency,
				payload_size=payload_size,
			)

			print("ok test_concurrent_connections")
			print("all tests passed")
			return 0

		finally:
			proxy.terminate()

			try:
				proxy.wait(timeout=2.0)
			except subprocess.TimeoutExpired:
				proxy.kill()
				proxy.wait(timeout=2.0)

			backend_server.close()
			await backend_server.wait_closed()

			stderr = proxy.stderr.read() if proxy.stderr else ""
			if stderr:
				print("\nproxy stderr:")
				print(stderr)


def main() -> int:
	if len(sys.argv) != 2:
		print(f"usage: {sys.argv[0]} tinyproxy", file=sys.stderr)
		return 2

	return asyncio.run(main_async(sys.argv[1]))


if __name__ == "__main__":
	raise SystemExit(main())
