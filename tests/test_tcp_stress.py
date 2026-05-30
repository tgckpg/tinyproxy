import asyncio
import time
import os

from .support import (
	LISTEN_HOST,
	PROXY_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
)

PROXY_PORT = PROXY_PORT + 1

async def close_writer(writer: asyncio.StreamWriter) -> None:
	try:
		writer.close()
		await writer.wait_closed()
	except ConnectionResetError:
		pass


async def run_one_concurrent(
	i: int,
	sem: asyncio.Semaphore,
) -> None:
	async with sem:
		reader, writer = await asyncio.open_connection(LISTEN_HOST, PROXY_PORT)

		try:
			got = await asyncio.wait_for(reader.read(4096), timeout=10.0)

			if not got:
				raise AssertionError(f"worker {i} got empty response")

			# Keep this weak on purpose. Other tests cover exact builtin output.
			if b":" not in got:
				raise AssertionError(f"worker {i} got invalid client_addr response: {got!r}")
		finally:
			await close_writer(writer)


async def test_concurrent_connections() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	total = int(os.environ.get("TOTAL", "10000"))
	concurrency = int(os.environ.get("CONCURRENCY", "10000"))

	conf_text = (
		f"listen"
		f" tcp {LISTEN_HOST}:{PROXY_PORT}"
		f" builtin client_addr\n"
	)

	started_at = time.monotonic()
	async with run_tinyproxy_with_conf(
		proxy_bin=proxy_bin,
		conf_text=conf_text,
		args=["-w", "0"],
		listen_host=LISTEN_HOST,
		listen_port=PROXY_PORT,
		proto="tcp",
	):
		sem = asyncio.Semaphore(concurrency)

		tasks = [
			asyncio.create_task(run_one_concurrent(i, sem))
			for i in range(total)
		]

		done = 0

		for task in asyncio.as_completed(tasks):
			await task
			done += 1

			if done % 1000 == 0:
				elapsed = time.monotonic() - started_at
				print(f"  {elapsed:.1f}s completed ({done}/{total})")


TESTS = [
	("test_concurrent_connections", test_concurrent_connections),
]
