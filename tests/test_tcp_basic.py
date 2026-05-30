import os

from .support import proxy_roundtrip


async def test_small_roundtrip() -> None:
	payload = b"hello through proxy\n"
	got = await proxy_roundtrip(payload)

	assert got == payload, f"small roundtrip mismatch: {got!r}"


async def test_large_roundtrip() -> None:
	payload_size = int(os.environ.get("LARGE_ROUNDTRIP_SIZE", str(256 * 1024)))
	timeout = float(os.environ.get("LARGE_ROUNDTRIP_TIMEOUT", "60.0"))

	chunk = b"0123456789abcdef"
	payload = (chunk * ((payload_size // len(chunk)) + 1))[:payload_size]

	got = await proxy_roundtrip(payload, timeout=timeout)

	assert got == payload, (
		f"large roundtrip mismatch: expected {len(payload)} bytes, "
		f"got {len(got)} bytes"
	)


async def test_many_sequential_connections() -> None:
	count = int(os.environ.get("SEQUENTIAL_CONNECTIONS", "1000"))

	for i in range(count):
		payload = f"message-{i}\n".encode()
		got = await proxy_roundtrip(payload)

		assert got == payload, f"sequential connection {i} failed"


TESTS = [
	("test_small_roundtrip", test_small_roundtrip),
	("test_large_roundtrip", test_large_roundtrip),
	("test_many_sequential_connections", test_many_sequential_connections),
]
