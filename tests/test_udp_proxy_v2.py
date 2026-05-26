import asyncio
import os

from .support import (
	BACKEND_PORT,
	LISTEN_HOST,
	PROXY_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
	run_udp_proxy_v2_echo_backend,
	udp_proxy_roundtrip,
)

async def test_tinyproxy_sends_proxy_v2_for_udp() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"{LISTEN_HOST}:{PROXY_PORT} "
		f"{LISTEN_HOST}:{BACKEND_PORT} "
		f"udp proxy_v2\n"
	)

	async with run_udp_proxy_v2_echo_backend(LISTEN_HOST, BACKEND_PORT) as backend:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			listen_host=LISTEN_HOST,
			listen_port=PROXY_PORT,
			proto="udp",
		):
			payload = b"hello udp proxy v2\n"

			echo_task = asyncio.create_task(udp_proxy_roundtrip(payload, timeout=3.0))

			try:
				got = await echo_task
			except TimeoutError:
				if backend.error is not None:
					raw = backend.last_raw.hex(" ") if backend.last_raw else "<none>"
					raise AssertionError(
						f"backend failed to parse UDP PROXY v2: {backend.error}; raw={raw}"
					) from backend.error

				raw = backend.last_raw.hex(" ") if backend.last_raw else "<none>"
				raise AssertionError(
					f"timed out waiting for UDP proxy v2 echo; backend raw={raw}"
				)

			assert got == payload, f"udp proxy v2 roundtrip mismatch: {got!r}"

			assert backend.error is None, f"backend failed to parse PROXY v2: {backend.error}"
			assert backend.last_src is not None, "backend did not record PROXY v2 source"
			assert backend.last_dst is not None, "backend did not record PROXY v2 destination"

			assert backend.last_src[0] == LISTEN_HOST
			assert backend.last_dst[0] == LISTEN_HOST
			assert backend.last_dst[1] == PROXY_PORT

TESTS = [
	("test_tinyproxy_sends_proxy_v2_for_udp", test_tinyproxy_sends_proxy_v2_for_udp),
]
