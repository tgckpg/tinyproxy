#!/usr/bin/env python3
import argparse
import socket
import threading
import time


def worker(sock: socket.socket, host: str, port: int, count: int, payload: bytes) -> None:
    for _ in range(count):
        sock.sendto(payload, (host, port))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=24801)
    ap.add_argument("--threads", type=int, default=32)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--payload", default="x")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))

    print(f"client={sock.getsockname()} target={(args.host, args.port)}")
    print(f"sending {args.threads * args.count} packets")

    payload = args.payload.encode()
    threads = []

    start = time.time()

    for _ in range(args.threads):
        t = threading.Thread(
            target=worker,
            args=(sock, args.host, args.port, args.count, payload),
        )
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    elapsed = time.time() - start
    print(f"done in {elapsed:.3f}s")

    sock.close()


if __name__ == "__main__":
    main()