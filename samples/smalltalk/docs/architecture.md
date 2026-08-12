# Smalltalk-on-Anvil architecture

## Reference boundary

The tokenizer, parser grammar, AST behavior, class lookup rules, bootstrap
packages, and tests in `samples/smalltalk-jit` are specifications and oracles.
The following implementation details are deliberately replaced:

- its heap-object AST and parser-time allocation into a moving GC;
- its register bytecode, private SSA/LIR, register allocator, and assemblers;
- its x64-only method ABI with fixed VM registers;
- its target-specific stack maps and implicit runtime-code-generation
  contracts.

The new frontend remains usable before the runtime is initialized. Parsing
produces arena-owned C data. Runtime objects are created only after parsing and
semantic validation succeed.

## Source layout

Public C headers remain flat in `include/`, while implementations are grouped
by responsibility under `src/`: `frontend/` owns source ingestion and semantic
structure; `compiler/` owns lowering, image/object emission, and artifact
materialization; `runtime/` owns values, heap, lookup, and execution bridges;
`runtime/control/` owns unwind and non-local control transfer; and
`runtime/primitives/` owns primitive implementations and their AOT bridges.
This is an ownership boundary only: it does not introduce private duplicate
headers or change the public ABI.

## Compilation path

```text
source -> lexer -> parser -> arena AST -> semantic/capture analysis
       -> typed Smalltalk lowering -> Anvil IR -> Anvil MIR/backend
       -> assembly -> system assembler -> linker -> executable/library
```

This compiler is strictly ahead-of-time. The runtime neither emits nor patches
machine code, and there is no interpreter-shaped fallback hidden in the path.
Unsupported source constructs are diagnosed with spans until their complete
lowering and runtime support exist.

The mandatory source image lives in `st-image`. Its checked manifest is loaded
without globbing, traversal or symlink following. All image sources are parsed
first and all explicitly supplied application sources second. A declaration
pass constructs one dense class/metaclass graph and flattened slot layout;
semantic analysis and lowering run only after that graph is complete. Thus an
application can extend the language image deliberately, but cannot replace an
image class or method by load-order accident. The image is source input, not a
serialized heap: its descriptors, method tables, symbols and literals become
ordinary relocatable data in the final AOT objects.
The loader also enforces independent manifest, per-file, total-byte and
file-count budgets while reading—not only the initial file size—and tears down
the entire partially parsed bundle on any I/O, parse, allocation or limit
failure.

Before lowering, `st_image_layout_build` compiles class pragmas and inherited
slots into the single authoritative runtime layout plan. Graph IDs remain
source identities and may include namespaces; runtime class IDs are a separate
dense sequence in which namespace entries map to zero. Lowering consumes that
same map for lexical `super` send sites, while metadata publishes the map for
bootstrap and tooling. No runtime component derives a class or shape from its
name.

The declarative shape recipes are `fixedPointers`, indexed VALUES/UINT8/
UINT16/UINT32, `boxedFloat64`, `closure`, and `largeInteger`. A class may own
multiple deterministic shapes but exactly one default. String and Symbol each
own distinct 8-, 16-, and 32-bit shapes; inheritance copies recipes into new
shape IDs owned by the subclass. Pointer bitmaps cover every inherited source
slot exactly. Raw recipes reject source slots. LargeInteger is frozen as one
raw fixed metadata word plus little-endian UINT32 indexed limbs; Block is four
raw fixed words plus indexed managed captures.

`<abstract: true>` sets `ST_CLASS_ABSTRACT` on that class descriptor, while
`<abstract: false>` records an explicit concrete declaration. The pragma is
validated for a single Boolean argument and may appear at most once per class.
Abstractness is intentionally not inherited: a concrete subclass of an
abstract protocol remains allocatable unless it declares itself abstract.

One explicit `<classObjectLayout: true>` role identifies the pointer layout of
managed class objects. Every generated metaclass entity receives that precise
layout, so Behavior/Class methods can access their declared fields. The root
metaclass superclass is the `Class` role, which continues through
`ClassDescription` and `Behavior`; consequently all class objects inherit the
normal allocation and reflection protocol. The finite runtime still closes
the class-of-a-metaclass identity with a self-reference because the source
graph does not represent an infinite metaclass tower. That identity knot does
not truncate the superclass chain.

Parser recursion is bounded by one shared budget across parenthesized
expressions, lexical/method blocks, literal arrays, classes, and namespaces.
The default is `ST_PARSER_MAX_NESTING`; embedders may lower it before parsing,
and exhaustion produces `ST_PARSE_ERR_NESTING_LIMIT` at the opening token. A
cascade is represented without losing evaluation order: its expression keeps
the lexical receiver, ordinary chain messages precede the cascade, and the
first message after every semicolon has `starts_cascade` set. Lowering must
restart at the original receiver exactly at those marked boundaries. Explicit
parentheses remain an AST grouping boundary (`parenthesized`) so a cascade on
`(x foo) bar; baz` restarts from the result of `x foo`, not from `x`.

## Baseline method ABI

The portable baseline uses a uniform typed signature conceptually equivalent to:

```c
StValue method(StFrame *frame);
```

The initial 64-bit frame layout is exact: `thread*` at byte 0, recursive
`caller*` at 8, immutable AOT method descriptor at 16, stable activation/home
token at 24, `receiver` at 32, `argv*` at 40, the shadow-root vector at 48,
`argc` at 56, `root_count` at 60, `safepoint_id` at 64 and flags at 68; size is
72 and alignment is 8. The C and
Anvil definitions are regression-checked together. `StThread` owns allocation, safepoint, and
exception/unwind state. `StValue` is the fully specified tagged value. A single
pointer argument makes every method-table entry exactly type-compatible, keeps
dispatch independent of host register names, and provides a precise shadow
stack until Anvil gains native statepoints/stack maps. Fixed-arity entry thunks
may be added only as a measured, ABI-tested optimization; they cannot change
language semantics.

`StMethodDescriptor` ABI v2 preserves the 96-byte tagged64 layout. It is
immutable, carries owner/selector/arity, source spans, safepoint root maps and
unwind metadata, and may exist before linking with `code_size == 0` when it
has no PC-relative unwind regions. Cooperative non-local return is represented
by `ST_METHOD_HAS_NON_LOCAL_RETURN` and does not manufacture a PC region.
Descriptors with unwind regions require a nonzero code size and exact in-range
PC offsets; a published `StMethodBinding` separately supplies the non-null
executable entry point.
`StHomeToken` is stable runtime data shared by the home activation and its
closures; it records whether that activation is still live and is the only
authority for a non-local return. Neither requires runtime code generation.
Generated code updates `safepoint_id` before operations that may allocate,
dispatch or unwind.

The first runtime target is 64-bit because the reference value/header encoding
is 64-bit. A 32-bit object model is a separate ABI design, not an accidental
truncation of the 64-bit representation.

## Values and object headers

The portable 64-bit representation uses the low three alignment bits directly:

```text
...000  non-null, eight-byte-aligned object pointer
...001  signed 61-bit SmallInteger
...010  Unicode scalar Character
...011  nil/false/true special immediate
```

Zero is invalid/uninitialized and is never confused with Smalltalk `nil`.
Reserved tags and unknown special payloads are invalid rather than silently
treated as heap pointers. SmallInteger encode/decode and arithmetic operate as
unsigned bit vectors plus explicit range checks, avoiding signed-overflow UB.
The checked fast path falls back to the future arbitrary-precision Integer
primitive when a 61-bit result does not fit.

Tag validity is not heap-membership validity. FFI, image loading,
deserialization, debugger and native-extension boundaries must validate an
object word against allocator extents before the explicitly named
`st_value_to_object_unchecked` conversion may authorize dereference. Values
created inside the trusted runtime retain that provenance on hot paths.

Every heap object starts with one atomic 64-bit header. It packs 24-bit dense
class and shape IDs, a four-bit survivor age, two-bit GC color and generation,
and flags for the remembered set, immutability, pinning, finalization, weak
references. Three high flag bits remain reserved and are rejected by the
current API; forwarding and identity hashing will not be advertised before
their complete forwarding-slot and side-table protocols exist. Field
transitions use masked compare/exchange or atomic bit operations, so GC state
changes cannot overwrite class/shape identity. Object size and the precise
pointer bitmap belong to the immutable class/shape descriptor indexed by these
IDs; the heap walker never guesses them from the header. The baseline
intentionally does not use NaN boxing: a target-specific
NaN-boxed representation may be introduced only with equivalent NaN, pointer,
GC, ABI and differential tests.

## Dispatch progression

Selectors are interned before method installation into a runtime-owned Robin
Hood table. IDs are dense 32-bit integers, zero is invalid, capacity is always
a power of two, and probes use `hash & (capacity - 1)`. A selector descriptor
retains exact bytes, kind and arity outside the managed heap, so dispatch never
depends on a raw pointer into a moving object space. Growth is transactional:
an allocation failure leaves every existing ID and lookup intact. Runtime
mutation is explicitly serialized; immutable published snapshots are the only
lock-free read mode of this low-level table.

1. Bounds-checked slot lookup, a typed indirect call, and an explicit typed
   runtime miss hook (implemented and executable in the current kernel).
2. Stable, non-moving `MethodEntry` records referenced by class slot vectors.
3. Monomorphic and polymorphic per-send-site data caches with atomic
   publication and epoch validation; their executable hit/miss paths are AOT
   compiled.
4. Closed-world/class-hierarchy/profile-guided devirtualization and inlining
   performed before assembly/linking, with a semantically complete generic
   branch retained when the proof is not static.

Each stage retains the general lookup path and is tested against method
replacement, `super`, inheritance, and does-not-understand behavior.

Profiles, when supplied, are offline compiler inputs. They influence layout,
inlining and cache-way ordering but never trigger runtime recompilation.

Stages 2 and 3 are implemented:
lookup uses sorted selector slots, lexical `super`, stable entries, lateral
class epochs and a four-way data-only PIC published with C11 atomics. General
AST sends lower to the fixed AOT resolve/miss/typed-call path with immutable
selector IDs, explicit child frames and compiler-owned root maps. Implementing
the cache never grants permission to patch executable pages.

The metadata emitter is intentionally separate from method lowering. It
materializes the source/class/selector/slot graph as typed relocatable data and
verifies code generation on every supported architecture. Metadata ABI v5 may
consume a complete validated set of real AOT method and block artifacts and
then emits typed function relocations, runtime method descriptors, immutable
block descriptors, capture tables and exact root maps. It also emits immutable
`StMethodBinding`/`StMethodEntry`/sorted method-slot chains, class and shape
descriptors, bitmap storage, dense descriptor-pointer arrays, and the final
target-native `st_runtime_descriptors_t`. Metadata-only emission retains zero
method slots rather than inventing addresses; AOT emission is directly
consumable by lookup and heap initialization.

The whole-program AOT driver already performs the complete transaction for
the implemented subset: load image then application sources, build the graph,
resolve primitives, build a minimal semantic view per method, lower each method
into its own verified Anvil module, and emit metadata ABI v5 with direct typed
function/descriptor relocations. It preserves root bitmaps, capture descriptors
and method/block control flags exactly. Method code is currently valid only for
the five 64-bit tagged-value targets (x86-64, AArch64, PPC64, PPC64LE and
z/Architecture); narrow targets fail before any artifact is published.

The filesystem materializer remains outside libanvil and accepts only a
validated, fully-owned in-memory artifact bundle.  It derives the profile name
from canonical provenance, writes assembly plus `bundle.manifest` through
directory-relative no-follow descriptors, fsyncs every file and the staging
directory, and atomically publishes with no-replace semantics.  Consequently
an I/O failure cannot expose a partial final profile and an existing build is
never silently overwritten.  Assembly and linking are later stages, not side
effects of materialization.

Primitive registration uses a dense vector plus a Robin Hood hash table. A
registration names either a non-zero implemented intrinsic or a real RT link
symbol and records arity, receiver side and failure policy. Resolution checks
AST pragmas against those contracts. The current catalog contains 65 contracts
and resolves all 69 image uses (64 distinct pragma names) without diagnostics.
It includes value, heap, arbitrary-precision Integer, boxed Float, String,
Symbol, Stream, Block, exception and reflection families. Float operations use
15 distinct runtime symbols and complete Smalltalk fallbacks; finite integral
results outside the tagged range continue once into the arbitrary-precision
Integer runtime. Block invocation covers zero through three arguments,
`valueWithArguments:` and `whileTrue:` with authenticated child roots and
cooperative control propagation. Reflection returns managed, rooted
`CompiledMethod` objects with stable identity for the currently published
MethodEntry binding. Adding a name to a wish list still never makes it callable:
each contract has a concrete handler, ABI and failure-path test.

Catalog closure is narrower than whole-image executability. The latter also
requires general nested-block lowering, mutable capture cells, transactional
class bootstrap, DNU and complete ordinary Smalltalk protocols; those remain
explicit milestones rather than being hidden behind primitive success.

The image-side output protocol is `Stream`/`ExternalStream`, with `lf` routed
through `nextPut:` and `nextPutAll:` routed through that primitive. `Transcript`
is an external bootstrap root published as an `ExternalStream` over descriptor
1 through explicit class/shape/ivar metadata. The image runtime owns dense
authenticated global/literal tables. Lowering uses authenticated frame-indexed
loads and the generic six-argument runtime primitive ABI; merely returning a
String is still not claimed to be console output.

## Managed-memory requirements

Moving GC requires explicit Anvil support for safepoints, live-root stack maps,
relocation of managed references, and write barriers. Until those contracts are
implemented and tested, generated code must not claim moving-GC safety. The
first complete collector is precise, non-moving mark/sweep with explicit frame
roots. Generational/moving collection is promoted only after statepoints,
barriers, relocation, and concurrent safepoint tests pass.

The implemented collector is stop-the-world, precise and non-moving.  Its heap
registry authenticates exact object bases and extents before dereference;
marking is iterative and scans descriptor pointer maps, indexed values and
generated frame root maps.  Collection errors and allocation failures are
transactional and cannot publish a partial sweep. Unwind/non-local-return
frames are accepted only when their authenticated control scope exactly
matches the frame chain; pending, leave and declared `ensure` roots are then
visited without allocation. Weak/finalizable objects and moving roots remain
rejected; header bits do not become behavior merely because they fit in the
representation.

Method entries, class descriptors, and the selector table remain stable and
outside the managed heap. A class slot vector inherits entries from its
superclass. Method replacement increments class/subclass epochs; inline-cache
readers validate `(class_id, epoch, entry)` as one published state. A `super`
send starts at the lexical owner's superclass, never at the dynamic receiver's
superclass.

The AOT control sidecar represents non-local return as explicit pending thread
state. Home tokens are heap-stable, refcounted and carry saturating
thread/activation identities; `ensure` records unwind LIFO exactly once while
allowing nested calls. Methods that can unwind now have a unique generated
epilogue: all local returns converge there, caret returns publish against the
home token, post-call pending checks propagate the exact rooted value, and
`scope_leave` decides catch versus propagation. The runtime closure bridge
authenticates immutable AOT block descriptors and exact closure objects,
stores captures in descriptor-scanned indexed payloads, and releases retained
HomeTokens through the collector's post-commit observer. Its object registry
uses a power-of-two Robin Hood table instead of a linear scan. The lowering and
metadata emitter now support one non-nested escaping block with arity zero or
one and VALUE/SELF captures, including cooperative NLR. CELL captures, nested
escaping blocks and exception matching remain subsequent gated work.
