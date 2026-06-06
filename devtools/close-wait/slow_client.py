#!/usr/bin/env python3
import socket
import time

HOST = "127.0.0.1"
PORT = 12800

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

# Make receive buffer reasonably large so the kernel can queue data
# while the application is sleeping.
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)

s.connect((HOST, PORT))
s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")

print("request sent; not reading for 5s")
time.sleep(5)

total = 0
chunks = []

try:
	while True:
		b = s.recv(65536)
		if not b:
			break
		chunks.append(b)
		total += len(b)
except ConnectionResetError as e:
	print(f"RESET after {total} bytes: {e}")
	raise

print(f"read total={total} bytes")
data = b"".join(chunks)

header, _, body = data.partition(b"\r\n\r\n")
print(header.decode(errors="replace"))
print(f"body bytes={len(body)}")
