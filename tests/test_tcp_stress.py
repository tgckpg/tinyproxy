import asyncio
import os

from .support import proxy_roundtrip


async def run_one_concurrent(
	i: int,
	payload_size: int,
	sem: asyncio.Semaphore,
) -> None:
	async with sem:
		prefix = f"worker-{i}-".encode()
		payload = (prefix * ((payload_size // len(prefix)) + 1))[:payload_size]

		got = await proxy_roundtrip(payload, timeout=30.0)

		if got != payload:
			raise AssertionError(
				f"worker {i} mismatch: expected {len(payload)} bytes, got {len(got)}"
			)


async def test_concurrent_connections() -> None:
	total = int(os.environ.get("TOTAL", "10000"))
	concurrency = int(os.environ.get("CONCURRENCY", "10000"))
	payload_size = int(os.environ.get("PAYLOAD_SIZE", "1024"))

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


TESTS = [
	("test_concurrent_connections", test_concurrent_connections),
]
