#!/usr/bin/env python3
import argparse
import asyncio
import resource
import socket
import time


REQ = b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"


async def one_client(i: int, host: str, port: int, hold: float, read_response: bool) -> None:
	reader, writer = await asyncio.open_connection(host, port)

	writer.write(REQ)
	await writer.drain()

	if read_response:
		# Read until server half-closes its write side.
		# Important: we do NOT close our side afterward.
		try:
			await asyncio.wait_for(reader.read(-1), timeout=10.0)
		except asyncio.TimeoutError:
			pass

	if i % 1000 == 0:
		print(f"opened/holding {i}")

	# Hold socket open. No writer.close(), no EOF, no RST.
	await asyncio.sleep(hold)


async def main() -> None:
	ap = argparse.ArgumentParser()
	ap.add_argument("--host", default="127.0.0.1")
	ap.add_argument("--port", type=int, default=12800)
	ap.add_argument("-n", "--connections", type=int, default=10000)
	ap.add_argument("-c", "--concurrency", type=int, default=1000)
	ap.add_argument("--hold", type=float, default=300.0)
	ap.add_argument("--no-read-response", action="store_true")
	args = ap.parse_args()

	soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
	print(f"nofile soft={soft} hard={hard}")
	print(
		f"target={args.host}:{args.port} "
		f"connections={args.connections} concurrency={args.concurrency} hold={args.hold}s"
	)

	sem = asyncio.Semaphore(args.concurrency)
	started = time.monotonic()
	created = 0

	async def runner(i: int) -> None:
		nonlocal created
		async with sem:
			created += 1
			await one_client(
				i,
				args.host,
				args.port,
				args.hold,
				read_response=not args.no_read_response,
			)

	tasks = [asyncio.create_task(runner(i)) for i in range(1, args.connections + 1)]

	while True:
		done = sum(1 for t in tasks if t.done())
		elapsed = time.monotonic() - started
		print(f"{elapsed:.1f}s created={created} done={done} holding~={created - done}")
		if done == len(tasks):
			break
		await asyncio.sleep(1.0)

	await asyncio.gather(*tasks)


if __name__ == "__main__":
	asyncio.run(main())
