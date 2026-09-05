"""Exercise a compiled Smalltalk HTTP application with an external, variable workload."""

import argparse
import asyncio
from pathlib import Path


async def exchange(port, request, head=False):
    reader, writer = await asyncio.open_connection("127.0.0.1", port)

    try:
        writer.write(request[:7])
        await writer.drain()
        await asyncio.sleep(0)
        writer.write(request[7:])
        await writer.drain()

        header = await reader.readuntil(b"\r\n\r\n")
        lines = header[:-4].split(b"\r\n")
        status = int(lines[0].split(b" ")[1])
        headers = dict(line.lower().split(b": ", 1) for line in lines[1:])
        body = await reader.read()

        if headers.get(b"connection") != b"close":
            raise AssertionError("Response must describe its connection lifetime")

        expected_length = 0 if head else int(headers[b"content-length"])

        if len(body) != expected_length:
            raise AssertionError("Response body does not match Content-Length")

        return status, body
    finally:
        writer.close()

        try:
            await writer.wait_closed()
        except ConnectionError:
            pass


async def run(executable, clients, waves):
    process = await asyncio.create_subprocess_exec(str(executable), stdout=asyncio.subprocess.PIPE)

    try:
        announcement = await asyncio.wait_for(process.stdout.readline(), 15)
        prefix = b"HTTP listening on 127.0.0.1:"

        if not announcement.startswith(prefix):
            raise AssertionError("Server did not announce its port: " + repr(announcement))

        port = int(announcement[len(prefix):])
        request = b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"

        for count in [17] + [clients] * waves + [17]:
            results = await asyncio.wait_for(asyncio.gather(*[exchange(port, request) for _ in range(count)]), 120)

            if any(result != (200, b"ok") for result in results):
                raise AssertionError("Concurrent health request failed")

            print("HTTP wave passed:", count, "clients", flush=True)

        cases = [
            (b"HEAD /health HTTP/1.1\r\nHost: localhost\r\n\r\n", 200, True),
            (b"GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n", 404, False),
            (b"POST / HTTP/1.1\r\nHost: localhost\r\n\r\n", 405, False),
            (b"GET / HTTP/1.1\r\n\r\n", 400, False),
            (b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, False),
            (b"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n\r\nx", 413, False),
        ]

        for request, expected, head in cases:
            status, _ = await asyncio.wait_for(exchange(port, request, head), 15)

            if status != expected:
                raise AssertionError("Unexpected HTTP status: " + str(status))

        result = await asyncio.wait_for(exchange(port, b"GET /shutdown HTTP/1.1\r\nHost: localhost\r\n\r\n"), 15)

        if result != (200, b"stopped"):
            raise AssertionError("Explicit server shutdown failed")

        await asyncio.wait_for(process.wait(), 15)

        if process.returncode != 0:
            raise AssertionError("Server cleanup failed: " + str(process.returncode))

        print("Compiled Smalltalk HTTP: PASS (elastic workload, framing, errors, explicit shutdown)")
    except BaseException:
        print("HTTP server exit status at failure:", process.returncode, flush=True)
        raise
    finally:
        if process.returncode is None:
            process.kill()
            await process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--clients", type=int, default=1024)
    parser.add_argument("--waves", type=int, default=2)
    args = parser.parse_args()

    if args.clients < 1 or args.waves < 1:
        parser.error("--clients and --waves must be positive")

    asyncio.run(run(args.executable.resolve(), args.clients, args.waves))


if __name__ == "__main__":
    main()
