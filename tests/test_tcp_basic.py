from .support import proxy_roundtrip


async def test_small_roundtrip() -> None:
	payload = b"hello through proxy\n"
	got = await proxy_roundtrip(payload)

	assert got == payload, f"small roundtrip mismatch: {got!r}"


async def test_large_roundtrip() -> None:
	payload = b"0123456789abcdef" * 131072  # 2 MiB
	got = await proxy_roundtrip(payload, timeout=30.0)

	assert got == payload, f"large roundtrip mismatch: got {len(got)} bytes"


async def test_many_sequential_connections(count: int = 1000) -> None:
	for i in range(count):
		payload = f"message-{i}\n".encode()
		got = await proxy_roundtrip(payload)

		assert got == payload, f"sequential connection {i} failed"


TESTS = [
	("test_small_roundtrip", test_small_roundtrip),
	("test_large_roundtrip", test_large_roundtrip),
	("test_many_sequential_connections", test_many_sequential_connections),
]
