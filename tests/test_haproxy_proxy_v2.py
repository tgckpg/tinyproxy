import os
import sys
import subprocess

from .support import (
	BACKEND_PORT,
	LISTEN_HOST,
	PROXY_PORT,
	SkipTest,
	find_program,
	print_process_output,
	proxy_roundtrip,
	run_echo_backend,
	run_tinyproxy_with_conf,
	terminate_process,
	wait_for_port,
	write_temp_file,
)


HAPROXY_FRONTEND_PORT = 41233


def require_haproxy() -> str:
	if sys.platform.startswith("win"):
		raise SkipTest("HAProxy proxy-v2 tests are skipped on Windows")

	haproxy_bin = find_program("HAPROXY_BIN", "haproxy")
	if haproxy_bin is None:
		raise SkipTest("HAProxy not found; set HAPROXY_BIN to enable this test")

	return haproxy_bin


async def test_tinyproxy_sends_proxy_v2_to_haproxy() -> None:
	haproxy_bin = require_haproxy()

	haproxy_conf = f"""
global
	maxconn 4096

defaults
	mode tcp
	timeout connect 5s
	timeout client 30s
	timeout server 30s

frontend proxy_v2_in
	bind {LISTEN_HOST}:{HAPROXY_FRONTEND_PORT} accept-proxy
	default_backend echo_backend

backend echo_backend
	server echo1 {LISTEN_HOST}:{BACKEND_PORT}
"""

	haproxy_conf_path = None
	haproxy = None

	# Adjust this if your config parser uses a different spelling.
	#
	# Current expected route shape:
	#
	#   listen upstream tcp proxy_v2
	#
	proxy_v2_option = os.environ.get("TINYPROXY_PROXY_V2_OPTION", "proxy_v2")

	tinyproxy_conf = (
		f"{LISTEN_HOST}:{PROXY_PORT} "
		f"{LISTEN_HOST}:{HAPROXY_FRONTEND_PORT} "
		f"tcp {proxy_v2_option}\n"
	)

	try:
		haproxy_conf_path = write_temp_file(haproxy_conf, ".haproxy.cfg")

		haproxy = subprocess.Popen(
			[haproxy_bin, "-f", haproxy_conf_path, "-db"],
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			text=True,
		)

		async with run_echo_backend(LISTEN_HOST, BACKEND_PORT):
			await wait_for_port(LISTEN_HOST, HAPROXY_FRONTEND_PORT)

			if haproxy.poll() is not None:
				stderr = haproxy.stderr.read() if haproxy.stderr else ""
				raise RuntimeError(
					f"haproxy exited early with code {haproxy.returncode}\n{stderr}"
				)

			async with run_tinyproxy_with_conf(
				proxy_bin=os.environ["TINYPROXY_BIN"],
				conf_text=tinyproxy_conf,
				listen_host=LISTEN_HOST,
				listen_port=PROXY_PORT,
			):
				payload = b"hello through proxy v2 via haproxy\n"
				got = await proxy_roundtrip(payload, port=PROXY_PORT)

				assert got == payload, f"proxy-v2 haproxy roundtrip mismatch: {got!r}"

	finally:
		terminate_process("haproxy", haproxy)
		print_process_output("haproxy", haproxy)

		if haproxy_conf_path is not None:
			try:
				os.unlink(haproxy_conf_path)
			except FileNotFoundError:
				pass


TESTS = [
	("test_tinyproxy_sends_proxy_v2_to_haproxy", test_tinyproxy_sends_proxy_v2_to_haproxy),
]
