#!/usr/bin/env python3

import asyncio
import os
import sys

from .support import (
	SkipTest,
	raise_fd_limit,
	run_default_tcp_tinyproxy,
	run_default_udp_tinyproxy,
)
from . import test_tcp_basic
from . import test_tcp_idle_timeout
from . import test_tcp_connect_timeout
from . import test_tcp_keep_alive
from . import test_tcp_stress
from . import test_tcp_backpressure
from . import test_udp_basic
from . import test_udp_idle_timeout
from . import test_haproxy_proxy_v2
from . import test_udp_proxy_v2
from . import test_unix_listeners


TCP_PROXY_TEST_MODULES = [
	test_tcp_basic,
]

UDP_PROXY_TEST_MODULES = [
	test_udp_basic,
]

STANDALONE_TEST_MODULES = [
	test_tcp_stress,
	test_unix_listeners,
	test_haproxy_proxy_v2,
	test_udp_proxy_v2,
	test_tcp_backpressure,
	test_tcp_idle_timeout,
	test_tcp_keep_alive,
	test_tcp_connect_timeout,
	test_udp_idle_timeout,
]


async def run_module_tests(module) -> tuple[int, int]:
	passed = 0
	skipped = 0

	for name, test_func in module.TESTS:
		print(f"running {name}...")

		try:
			await test_func()
		except SkipTest as exc:
			skipped += 1
			print(f"skipped {name}: {exc}")
			continue

		passed += 1
		print(f"ok {name}")

	return passed, skipped


async def main_async(proxy_bin: str) -> int:
	os.environ["TINYPROXY_BIN"] = proxy_bin

	fd_limit = int(os.environ.get("FD_LIMIT", "65535"))
	raise_fd_limit(fd_limit)

	passed = 0
	skipped = 0

	async with run_default_tcp_tinyproxy(proxy_bin):
		for module in TCP_PROXY_TEST_MODULES:
			p, s = await run_module_tests(module)
			passed += p
			skipped += s

	async with run_default_udp_tinyproxy(proxy_bin):
		for module in UDP_PROXY_TEST_MODULES:
			p, s = await run_module_tests(module)
			passed += p
			skipped += s

	for module in STANDALONE_TEST_MODULES:
		p, s = await run_module_tests(module)
		passed += p
		skipped += s

	print(f"all tests passed: passed={passed} skipped={skipped}")
	return 0


def main() -> int:
	if len(sys.argv) != 2:
		print(f"usage: {sys.argv[0]} tinyproxy", file=sys.stderr)
		return 2

	return asyncio.run(main_async(sys.argv[1]))


if __name__ == "__main__":
	raise SystemExit(main())
