import asyncio
import os

from .support import (
	LISTEN_HOST,
	BACKEND_PORT,
	PROXY_PORT,
	SkipTest,
	run_tinyproxy_with_conf,
)


class UdpEchoProtocol(asyncio.DatagramProtocol):
	def __init__(self) -> None:
		self.received: list[bytes] = []

	def datagram_received(self, data: bytes, addr) -> None:
		self.received.append(data)
		transport = self.transport
		transport.sendto(data, addr)

	def connection_made(self, transport) -> None:
		self.transport = transport


class UdpClientProtocol(asyncio.DatagramProtocol):
	def __init__(self) -> None:
		self.queue: asyncio.Queue[bytes] = asyncio.Queue()

	def datagram_received(self, data: bytes, addr) -> None:
		self.queue.put_nowait(data)


async def udp_roundtrip_same_socket(
	transport: asyncio.DatagramTransport,
	protocol: UdpClientProtocol,
	payload: bytes,
	timeout: float = 3.0,
) -> bytes:
	transport.sendto(payload)
	return await asyncio.wait_for(protocol.queue.get(), timeout=timeout)


async def test_udp_idle_timeout_expires_client_but_route_still_works() -> None:
	proxy_bin = os.environ.get("TINYPROXY_BIN")
	if not proxy_bin:
		raise SkipTest("TINYPROXY_BIN is not set")

	conf_text = (
		f"listen"
		f" udp {LISTEN_HOST}:{PROXY_PORT} "
		f" udp {LISTEN_HOST}:{BACKEND_PORT} "
		f" idle_timeout=1\n"
	)

	loop = asyncio.get_running_loop()

	backend_transport, backend_protocol = await loop.create_datagram_endpoint(
		lambda: UdpEchoProtocol(),
		local_addr=(LISTEN_HOST, BACKEND_PORT),
	)

	client_transport = None

	try:
		async with run_tinyproxy_with_conf(
			proxy_bin=proxy_bin,
			conf_text=conf_text,
			listen_host=LISTEN_HOST,
			listen_port=PROXY_PORT,
			proto="udp",
		) as proxy:
			client_transport, client_protocol = await loop.create_datagram_endpoint(
				lambda: UdpClientProtocol(),
				remote_addr=(LISTEN_HOST, PROXY_PORT),
			)

			first = b"before udp idle timeout\n"
			got = await udp_roundtrip_same_socket(
				client_transport,
				client_protocol,
				first,
			)
			assert got == first, (
				f"first udp roundtrip mismatch: got={got!r} expected={first!r}"
			)

			await asyncio.sleep(2.0)

			second = b"after udp idle timeout\n"
			got = await udp_roundtrip_same_socket(
				client_transport,
				client_protocol,
				second,
			)
			assert got == second, (
				f"second udp roundtrip mismatch after idle expiry: "
				f"got={got!r} expected={second!r}"
			)

			assert backend_protocol.received == [first, second], (
				f"backend received unexpected packets: "
				f"{backend_protocol.received!r}"
			)

	finally:
		if client_transport is not None:
			client_transport.close()

		backend_transport.close()


TESTS = [
	(
		"test_udp_idle_timeout_expires_client_but_route_still_works",
		test_udp_idle_timeout_expires_client_but_route_still_works,
	),
]
