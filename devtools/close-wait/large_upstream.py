#!/usr/bin/env python3
import socket
import threading

HOST = "127.0.0.1"
PORT = 12700
BODY_SIZE = 4 * 1024 * 1024

BODY = b"x" * BODY_SIZE
RESP = (
	b"HTTP/1.1 200 OK\r\n"
	+ f"Content-Length: {BODY_SIZE}\r\n".encode()
	+ b"Connection: close\r\n"
	+ b"\r\n"
	+ BODY
)

def handle(c):
	try:
		c.recv(4096)
		c.sendall(RESP)
	finally:
		c.close()

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((HOST, PORT))
s.listen(4096)

print(f"serving {BODY_SIZE} bytes on {HOST}:{PORT}")

while True:
	c, _ = s.accept()
	threading.Thread(target=handle, args=(c,), daemon=True).start()
