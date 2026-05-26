import asyncio
import os

from .support import (
	LISTEN_HOST,
	PROXY_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
	run_udp_proxy_v2_echo_backend,
	udp_proxy_roundtrip,
)

UDP_PROXY_V2_BACKEND_PORT = 41234


async def test_tinyproxy_sends_proxy_v2_for_udp() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"{LISTEN_HOST}:{PROXY_PORT} "
		f"{LISTEN_HOST}:{UDP_PROXY_V2_BACKEND_PORT} "
		f"udp proxy_v2\n"
	)

	async with run_udp_proxy_v2_echo_backend(
		LISTEN_HOST,
		UDP_PROXY_V2_BACKEND_PORT,
	) as backend:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			listen_host=LISTEN_HOST,
			listen_port=PROXY_PORT,
			proto="udp",
		):
			payload = b"hello udp proxy v2\n"

			try:
				got = await udp_proxy_roundtrip(payload, timeout=3.0)
			except TimeoutError as exc:
				raw = backend.last_raw.hex(" ") if backend.last_raw else "<none>"
				raise AssertionError(
					"timed out waiting for UDP proxy-v2 echo; "
					f"backend_error={backend.error!r}; "
					f"backend_last_src={backend.last_src!r}; "
					f"backend_last_dst={backend.last_dst!r}; "
					f"backend_raw={raw}"
				) from exc

			raw = backend.last_raw.hex(" ") if backend.last_raw else "<none>"

			assert backend.error is None, (
				f"backend failed to parse PROXY v2: {backend.error!r}; raw={raw}"
			)
			assert got == payload, (
				f"udp proxy-v2 roundtrip mismatch: got={got!r} expected={payload!r}; "
				f"backend_raw={raw}"
			)
			assert backend.last_src is not None, (
				f"backend did not record PROXY v2 source; raw={raw}"
			)
			assert backend.last_dst is not None, (
				f"backend did not record PROXY v2 destination; raw={raw}"
			)

			assert backend.last_src[0] == LISTEN_HOST, (
				f"bad proxy-v2 src addr: got={backend.last_src!r}; raw={raw}"
			)
			assert backend.last_dst[0] == LISTEN_HOST, (
				f"bad proxy-v2 dst addr: got={backend.last_dst!r}; raw={raw}"
			)
			assert backend.last_dst[1] == PROXY_PORT, (
				f"bad proxy-v2 dst port: got={backend.last_dst!r}; "
				f"expected_port={PROXY_PORT}; raw={raw}"
			)


TESTS = [
	("test_tinyproxy_sends_proxy_v2_for_udp", test_tinyproxy_sends_proxy_v2_for_udp),
]