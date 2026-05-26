from .support import udp_proxy_roundtrip, udp_proxy_client


async def test_udp_small_roundtrip() -> None:
	payload = b"hello through udp proxy\n"
	got = await udp_proxy_roundtrip(payload)

	assert got == payload, f"udp small roundtrip mismatch: {got!r}"


async def test_udp_1k_roundtrip() -> None:
	payload = b"0123456789abcdef" * 64  # 1 KiB
	got = await udp_proxy_roundtrip(payload, timeout=5.0)

	assert got == payload, f"udp 1k roundtrip mismatch: got {len(got)} bytes"


async def test_udp_4k_roundtrip() -> None:
	payload = b"0123456789abcdef" * 256  # 4 KiB
	got = await udp_proxy_roundtrip(payload, timeout=5.0)

	assert got == payload, f"udp 4k roundtrip mismatch: got {len(got)} bytes"


async def test_udp_many_sequential_datagrams(count: int = 1000) -> None:
	async with udp_proxy_client() as client:
		for i in range(count):
			payload = f"udp-message-{i}\n".encode()
			got = await client.roundtrip(payload)

			assert got == payload, f"udp sequential datagram {i} failed"


TESTS = [
	("test_udp_small_roundtrip", test_udp_small_roundtrip),
	("test_udp_1k_roundtrip", test_udp_1k_roundtrip),
	("test_udp_4k_roundtrip", test_udp_4k_roundtrip),
	("test_udp_many_sequential_datagrams", test_udp_many_sequential_datagrams),
]
