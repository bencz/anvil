# Compiled HTTP server

`WebServerApplication` starts `HttpServer new serveOn: 8080`. The server
continues accepting connections until `stop` is sent to that server instance.
There is no `maximumConnections:` setting or fixed array of active connections.

The runtime allocates socket entries and fiber stacks as connections arrive.
Closed socket slots are reused with generation checks, completed native stacks
are released, and `ObjectMemory collect` reclaims consumed fiber records and
managed objects. Readiness uses Linux `epoll` and Windows IOCP with `AcceptEx`,
overlapped reads/writes and cancellation draining. Timers use a growing min-heap.
Platform implementations are selected through vtables in `src/platform`.

This is a cooperative scheduler on one OS thread. A CPU-bound handler must
yield to allow other fibers to run. Capacity depends on available memory,
socket resources and OS queue settings. The Winsock adapter requests the TCP
provider's largest supported pending-accept queue using `SOMAXCONN_HINT`;
this is separate from the number of active connections. See Microsoft's
[listen contract](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-listen).

The sample currently binds IPv4 loopback, serves bodyless GET/HEAD requests,
and closes each connection after its response. Routes are `/` and `/health`.
It validates CRLF framing, Host, header names and duplicate framing headers.
Request bodies and transfer encoding are explicitly rejected. This sample
does not yet implement persistent HTTP connections, TLS or the complete HTTP
server protocol. Header byte/line limits and I/O deadlines bound individual
requests; they do not cap concurrent connections.

## Compile and validate

From the repository root in Linux or WSL, build the compiler and publish the
test application to a new output directory on the Linux filesystem:

```sh
make -j4 HOST_PLATFORM=posix BUILD_DIR=build/wsl smalltalk-aotc smalltalk-runtime
mkdir -p /tmp/anvil-http-output
build/wsl/samples/smalltalk/st-aotc samples/smalltalk/st-image samples/smalltalk/examples/http-regression http HttpRegressionApplication run /tmp/anvil-http-output
python3 samples/smalltalk/tests/run_runtime.py --cc clang --build-dir build/http-linux --http-profile /tmp/anvil-http-output/http/x86_64-sysv-gas-O2 --clients 1024
```

Publication refuses to overwrite an existing application. Use a fresh output
directory for another compilation. The regression application chooses an
ephemeral port and adds `/shutdown` only for its external test driver.

For native Windows execution, copy the generated `x86_64-win64-gas-O2`
directory into the Windows workspace, then run in PowerShell:

```powershell
python samples/smalltalk/tests/run_runtime.py --cc C:/llvm/bin/clang.exe --build-dir build/http-windows --http-profile build/http-profile/x86_64-win64-gas-O2 --clients 1024
```

Clang assembles the generated GAS text into COFF objects and links the Windows
runtime with `Ws2_32`. MASM/NASM are not needed for this profile. The compiler's
source-loading and publication tools still use the Linux/WSL path in these
instructions; native Windows execution of the generated program is tested
separately.

The driver first checks native fiber/socket contracts, then runs HTTP waves of
17, the requested client count twice, and 17 again in one server process. It
checks response bytes, Content-Length, HEAD, errors and explicit shutdown.
`--clients` belongs to the workload generator. It imposes no server policy.
To exercise an already linked test application:

```powershell
python samples/smalltalk/tests/http_test.py build/http-windows/http.exe --clients 2048 --waves 3
```

The `fibers` example demonstrates cooperative interleaving and joining results.
The `collections` example exercises dictionary growth/removal, constructor
cascades and Unicode read/write streams; its expected exit status is 42.
