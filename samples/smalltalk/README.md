# Anvil Smalltalk

This directory contains the new Smalltalk implementation built on Anvil.  The
project in `../smalltalk-jit` is an executable language and test reference; it
is not linked into this implementation and its private IR, register allocator,
assembler, object handles, and x64 register ABI are not dependencies here.

The port is intentionally bottom-up:

1. a heap-independent lexer and parser with source spans;
2. an arena-owned AST that does not allocate runtime Smalltalk objects;
3. semantic analysis, capture analysis, and class construction;
4. a precisely specified tagged-value/object ABI and runtime;
5. direct lowering to verified Anvil IR, including indirect calls through
   typed method tables;
6. GC safepoints, stack maps, barriers, exceptions, closures, and later
   AOT profile-guided inline caches and devirtualization.

Every promoted layer has executable tests.  Code that has not reached its layer
is absent instead of being represented by a successful no-op or placeholder.

## Creating, compiling, and running an application

The Smalltalk implementation is AOT-only. Running an application does not
start a JIT: the compiler first parses the checked `st-image`, then parses the
application sources, generates assembly, invokes the native assembler and
linker, and links the result with the Smalltalk runtime.

Run the following commands from the Anvil repository root. This example creates
an application named `my_hello`:

```sh
mkdir -p samples/smalltalk/examples/my_hello
```

Create `samples/smalltalk/examples/my_hello/application.manifest`:

```text
# Paths are relative to this application directory.
MyHelloApplication.st
```

Create `samples/smalltalk/examples/my_hello/MyHelloApplication.st`:

```smalltalk
MyHelloApplication := Object [

    run [
        Transcript nextPutAll: 'Hello from my application'; lf.
        ^0
    ]
]
```

The manifest is authoritative. Every application source must appear exactly
once, in load order. Absolute paths, `..`, symlink traversal, duplicate paths,
and files outside the application directory are rejected. All image sources
from `st-image/manifest.txt` are parsed before the first application source.

Compile the complete image and application, materialize the multiarchitecture
assembly matrix, assemble the native x86-64 profile, and link the executable:

```sh
make smalltalk-app \
    ST_APP_DIR=samples/smalltalk/examples/my_hello \
    ST_APP_NAME=my_hello \
    ST_ENTRY_CLASS=MyHelloApplication \
    ST_ENTRY_SELECTOR=run \
    ST_APP_OUTPUT_DIR="$PWD/build/smalltalk-apps"
```

`ST_APP_NAME` must be a portable identifier: it starts with an ASCII letter
and contains only ASCII letters, digits, or `_`. The current process entrypoint
must be a zero-argument instance method. Its result must be a SmallInteger in
the range 0 through 255; that value becomes the native process exit status.

Run the linked program:

```sh
./build/smalltalk-apps/my_hello/native/host/my_hello
echo $?
```

The expected output is:

```text
Hello from my application
0
```

The publication is intentionally no-replace. Reusing the same application
name and output directory fails instead of silently overwriting a previous
build. Use a fresh output directory, or deliberately remove the old generated
application directory before rebuilding it.

### Inspecting the generated assembly

The command above publishes this layout:

```text
build/smalltalk-apps/my_hello/
├── matrix.manifest
├── x86_64-sysv-gas-O2/
├── arm64-sysv-gas-O2/
├── ppc64-sysv-gas-O2/
├── ppc64le-sysv-gas-O2/
├── zarch-mvs-hlasm-O2/
└── native/host/my_hello
```

Each tagged-64 profile contains one assembly file per compiled method, the
image metadata, the launch descriptor, and an authenticated `bundle.manifest`.
GNU targets use `.s`; z/Architecture uses `.asm` HLASM. The top-level
`matrix.manifest` also records `x86`, `s370`, `s370_xa`, `s390`, and `ppc32` as
`tagged32-abi-unimplemented`; no empty or fake assembly directories are made.

To generate only the assembly matrix, without native assembly or linking:

```sh
make smalltalk-aotc

./build/samples/smalltalk/st-aotc \
    samples/smalltalk/st-image \
    samples/smalltalk/examples/my_hello \
    my_hello MyHelloApplication run \
    "$PWD/build/smalltalk-assembly"
```

The checked-in `examples/hello` and `examples/closures` applications use this
same product pipeline. The end-to-end regression compiles both five-profile
matrices, cross-assembles the complete ARM64 Hello profile, links and executes
the x86-64 programs, checks Hello's stdout byte-for-byte, and requires the
closures application to return 42.

## Current gates

The lexer implements the reference lexical grammar without depending on the
VM heap. It handles memory and file input, bounded lookahead, exact byte spans,
long tokens, escaped quotes, comments, radix/floating/scaled-decimal spelling,
canonical shortest-form UTF-8 characters, terminal/idempotent EOF, and
recoverable allocation/I/O errors. Closed comments are skipped without copying
their bodies into token buffers; explicit `reinit` entry points release storage
owned by an already-live lexer/parser.

The AST layer is also heap-independent. Nodes and their growing child lists are
owned by a checked arena, strings retain exact lengths (including embedded NUL
bytes), source spans live directly on nodes, and numeric literals retain exact
spelling for later arbitrary-precision semantic conversion. No runtime object
or target-sized host value is created while parsing.

The runtime ABI now has a checked 64-bit tagged value representation: aligned
object pointers, signed 61-bit SmallIntegers, Unicode Characters and distinct
`nil`/`false`/`true` immediates use three low tag bits. Its one-word atomic
object header packs dense class/shape IDs and GC state with explicit masks.
Selector interning uses stable dense IDs and a power-of-two Robin Hood table;
growth is transactional under allocator fault injection.

The first executable backend kernel defines the uniform 64-bit method ABI
`uint64_t method(StFrame *)`. Its 72-byte C frame and recursive Anvil struct
agree field-for-field (`thread`, `caller`, AOT `method`, activation `home`,
`receiver`, `argv`, shadow `roots`, `argc`, `root_count`, `safepoint_id`,
`flags`). Two real methods live in a typed relocatable VTable. The
dispatcher bounds-checks the slot, performs a checked indirect call on a hit,
and calls the typed `st_dispatch_miss` runtime hook on a miss. The root test
links generated x86-64 assembly to a C harness and executes both hit and miss
paths; ARM64 output is also cross-assembled.
This VTable is a relocation/calling-convention conformance kernel, not the
final language dispatch design. Real sends progress through selector lookup,
monomorphic/PIC data caches, epochs, guarded direct calls and AOT
devirtualization. The runtime never emits or patches machine code.

Inherited lookup and the data-cache layer are now concrete runtime code.
Method slots are searched by selector ID, `super` starts at the lexical
owner's superclass, immutable versioned bindings are published through stable
`MethodEntry` records, and a four-way PIC validates receiver class, descendant
epoch, entry and binding as one atomic snapshot. Cache replacement uses a
power-of-two mask; the hit/miss machine-code paths remain fixed AOT code.

The parser ports the reference grammar onto that AST: class and namespace
declarations, extensions, unary/binary/keyword methods, pragmas, lexical
blocks, temporaries, assignments, message precedence, cascades, exact numeric
literals, Unicode character literals, and nested literal arrays. Errors are
structured and sticky; a failed production cannot be mistaken for a partial
successful tree. Every recursive delimiter/class production consumes a shared,
configurable nesting budget (default `ST_PARSER_MAX_NESTING`), and adjacent
top-level forms require whitespace, a comment, or an explicit period boundary.
Cascade messages retain the original receiver and mark every semicolon boundary
with `st_ast_message_t::starts_cascade`.

The semantic pass builds dense scopes/bindings and node side tables, resolves
lexicals and explicitly catalogued ivar/class/global bindings, rejects implicit
forward globals, and computes transitive closure captures. Read-only captures
remain values; any binding captured and assigned anywhere in its home method
is promoted to a shared cell. Nested caret returns are annotated as home-method
returns and force an explicit context. Allocation is injectable and every OOM
point is regression-tested transactionally.

The class graph keeps image/application globals in shared namespace layers
instead of copying a flat global catalog into every class.  Method lookup is
indexed by a power-of-two Robin Hood table, while semantic analysis builds an
owned minimal view containing only the external names referenced by that
method.  A 10,000-class regression guards both linear storage and lookup probe
counts.

The source-image loader consumes the checked `st-image/manifest.txt`, parses
all 50 image units before application units, and builds one dense
class/metaclass graph with inherited slot layouts. A real integration gate
adds `HelloApplication`, semantically analyzes every image/application method,
and proves deterministic IDs and lexical inheritance. Independent budgets
bound manifest bytes, per-file bytes, total source bytes and file count.

Metadata emission interns selectors and strings in power-of-two hash tables
and emits immutable entity, method, selector and slot records with typed Anvil
relocations. Generated x86-64 assembly is assembled, linked to a C consumer
and executed; ARM64 is cross-assembled, and all ten Anvil targets generate the
same logical image metadata. Requesting method code from this metadata-only
phase fails with `ST_IMAGE_EMIT_ERR_METHOD_CODE_UNAVAILABLE`: no fake entry
point is manufactured.

The method lowerer emits the exact `StValue (StFrame *)` ABI
for `self`, arguments, temporaries, assignments, immediate literals, returns
and proven-inline Boolean control blocks.  Ordinary message sends use frozen
selector IDs, one data-only PIC per AOT call site, an authenticated receiver
class, exported precise root metadata and a typed indirect call after lookup.
The caller's temporary receiver/argument roots are cleared after the merge so
later collections cannot retain dead objects.  Real image/application methods
and `true & false` are emitted as assembly, linked and executed on x86-64;
AArch64 output is cross-assembled. Escaping blocks support arities zero through
three, multiple and nested block artifacts, VALUE/SELF captures and shared CELL
captures for mutable lexical bindings. Cascades preserve and root their original
receiver; external globals and immutable String literals load through
authenticated dense image-runtime tables; and resolved runtime-symbol
primitives use the uniform checked bridge ABI. Unsupported context operations
still fail with a source-span diagnostic before publication.

Methods that may send or perform a non-local return now use one generated
control epilogue.  The AOT code enters an authenticated activation scope,
publishes a caret return against the stable home token, propagates the exact
pending value after calls, roots that value across the epilogue safepoint and
leaves the scope on every non-fatal return path.  x86-64 execution covers a
three-frame return/catch and `ensure` replacement of a pending value; the NLR
method is also cross-assembled for AArch64. Escaping closure allocation is
connected to the managed heap, authenticated block descriptors and post-sweep
HomeToken reclamation. Cooperative exception matching, nested handlers,
`ensure:`, unwinding and unhandled-exception propagation use the same rooted
pending-control protocol; unsupported retry/resignal operations are absent.

Resolved primitive bindings enter the same lowering transaction. Generated
methods call the concrete implementation with independent status/result
channels: success returns the value, a permitted failure executes the
Smalltalk fallback body, and violation of an infallible contract is fatal. No
pragma is rediscovered by spelling inside the lowerer.

The runtime descriptor layer validates dense class/shape tables, exact
object extents, pointer bitmaps, root maps and unwind regions. Stable
`MethodEntry` records publish immutable bindings atomically; shape transitions
use CAS and preserve concurrent GC bits. Allocation currently accepts only
implemented immutable/pinned policies. Caller-requested weak, finalizable and
remembered-set allocation flags are rejected; the write barrier and collector
alone own the remembered bit. Primitive pragmas resolve only when executable
semantics and the declared failure policy both exist. All 69 primitive pragma
uses in the current image bind to executable contracts without diagnostics.
The families cover identity and tagged integers,
heap access/allocation and barriers, arbitrary-precision Integer operations,
boxed binary64 including deterministic rounding and hashing, Unicode String
comparison/concatenation/interning, stream writes, closure invocation and
iteration, cooperative exceptions and reflective method lookup. Runtime-symbol
operations use real link symbols and never consume fake intrinsic IDs.

The complete current image lowers in the whole-program AOT gate. Managed class
and selector bootstrap, canonical Symbols, eager reflective method mirrors,
authenticated DNU and ordinary image protocols participate in the linked
application runtime. A future image addition is accepted only after its
compiler/runtime capability and failure behavior are implemented and gated.

The first collector is a precise, stop-the-world, non-moving mark/sweep heap.
Every object dereference is authorized through an exact-base extent registry;
marking is iterative and uses descriptor pointer maps plus the generated
frame-root bitmaps.  Interior, foreign, dangling or corrupt roots and every OOM
path fail before sweep and leave the live heap unchanged. Weak references,
finalizers and moving/generational collection remain unsupported. For scoped
frames the collector authenticates the AOT thread/control sidecar and visits
the pending NLR value, the transient `scope_leave` value and explicit root
vectors attached to armed or running `ensure` callbacks.

The control sidecar implements non-local return without `longjmp`: stable
refcounted home tokens, anti-ABA activation IDs, pending NLR propagation and
LIFO `ensure` callbacks executed exactly once. Calling a block after its home
returned is detected. Generated method epilogues are now connected to this
protocol and export `CAN_UNWIND`/`HAS_NON_LOCAL_RETURN` metadata. The collector
now enumerates that sidecar before marking. Reclamation observers use a
prepare/commit protocol: every fallible check completes before sweep, and
external releases occur exactly once only after the heap commit. The closure
runtime ABI uses authenticated immutable block descriptors, a power-of-two
Robin Hood exact-object registry, precise indexed capture scanning and
exactly-once HomeToken release after sweep. Generated escaping closures cover
VALUE, SELF and CELL captures, nested blocks, NLR and detection of a returned
home. Exception handlers use the same precise control-sidecar roots instead of
falling back to host `setjmp`/`longjmp` or a silent result.

The whole-program AOT driver owns one Anvil context and one transactional
lowering result per method, then feeds exact method/block flags, captures and
canonical root-map bitmaps into metadata ABI v5. The emitted metadata owns the
runtime method descriptors, block descriptors, real method bindings/entries,
sorted class method slots, class/shape descriptors and their relocation
registries. The x86-64 gate validates the emitted runtime descriptor set,
allocates a class object from the emitted metaclass shape, and uses only these
emitted objects. Complete method and
image assembly is produced for the tagged-64 targets x86-64, AArch64, PPC64,
PPC64LE and z/Architecture. The five narrower Anvil targets reject Smalltalk
method lowering before publication; a distinct 32-bit tagged-value ABI is
required rather than truncating the 64-bit model.

`st_artifact_bundle_materialize` is the deliberately external filesystem
boundary for those in-memory artifacts.  It publishes one canonical
`target-abi-syntax-opt/` profile containing every method assembly file,
`metadata.s`/`metadata.asm`, and the byte-exact `bundle.manifest`.  Files are
created relative to a private same-filesystem staging directory with
`openat`/`O_NOFOLLOW`/`O_EXCL`, synchronized, and exposed by one atomic
no-replace rename.  Traversal, symlink destinations, duplicate names,
collisions, short writes and failed writes are rejected or rolled back; this
stage intentionally does not invoke an assembler or linker.

Run it with:

```sh
make -C samples/smalltalk test
```

Frontend-ready application inputs live under `examples/`.  Their
`application.manifest` sources are loaded strictly after the real `st-image`
and are gated through parsing, class-graph construction, semantic analysis,
whole-image AOT compilation and product linking. `hello` executes the real
`Transcript nextPutAll: ...; lf` protocol through the authenticated descriptor-1
stream, and `closures` executes escaping captures and a non-local return. The
application startup uses metadata ABI v5 for the exact global, literal, class,
shape, selector, method, block and root-map contracts. No checked-in assembly,
captured output, placeholder entrypoint, or JIT path is involved. See
[`examples/README.md`](examples/README.md) for the manifest boundary.

The Anvil-linked dispatch kernel is part of the repository-wide `make tests`
gate because it links against the current root `libanvil` build.

When the supplied reference tree is present, all package, test, sample,
benchmark, and typed-prototype sources are gated separately (407 files in the
current reference, with no known-failure list):

```sh
make -C samples/smalltalk test-reference-corpus
```

See [docs/architecture.md](docs/architecture.md) for the port boundary and ABI
direction. The image is developed by semantic layer rather than by copying the
reference file or primitive count; see
[docs/image-roadmap.md](docs/image-roadmap.md) for those boundaries and gates.
