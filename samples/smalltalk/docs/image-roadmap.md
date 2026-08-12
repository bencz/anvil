# AOT image design and capability roadmap

The Anvil Smalltalk image is a language library compiled together with the
application. It is not a serialized VM snapshot, a bytecode image, or a copy of
the reference implementation. The reference tree is useful as a grammar and
behavior oracle, but it does not define our object ABI, compiler pipeline, or
runtime boundaries.

## Design rules

The image follows these rules:

1. Observable language behavior is written in Smalltalk whenever it can be
   expressed without violating the runtime ABI.
2. A primitive is introduced only for an operation that must cross into the
   runtime: object allocation and authenticated raw storage, GC barriers,
   operating-system I/O, atomic publication, symbol interning, exact numeric
   representation, stack/control transfer, or reflective metadata.
3. Every primitive has a stable ID, a checked receiver/arity contract, separate
   status and result channels, and an executable implementation. A registered
   name without an implementation is forbidden.
4. A fallible primitive must have a semantically correct Smalltalk fallback.
   Otherwise primitive resolution fails at compile time.
5. The compiler is strictly AOT. Inline caches are data, profiles are offline
   compiler input, and the runtime never emits or patches machine code.
6. Unsupported behavior is diagnosed before publishing an object, assembly
   bundle, or executable. Returning `nil`, silently skipping work, or creating
   an unresolved helper symbol is not an implementation.

## Image layers

The image is developed in dependency order. A layer is promoted only when its
protocol, lowering, runtime contracts, and executable tests are all present.

### Bootstrap kernel

- `Object`, `UndefinedObject`, `Boolean`, `True`, and `False`
- `Behavior`, `ClassDescription`, `Class`, and `MetaClass`
- dense class, shape, selector, method, and global identities
- `nil`, `false`, `true`, SmallInteger, and Character immediates
- authenticated allocation, indexed storage, instance slots, class lookup,
  identity, and identity hash

The tagged-64 kernel and its non-moving precise heap exist today. The separate
tagged-32 ABI is intentionally not inferred by truncating tagged-64 values.

### Core control and dispatch

- inherited and lexical-`super` lookup
- stable method entries, class epochs, and data-only polymorphic inline caches
- closures with explicit captures and home activations
- non-local return and `ensure:`
- exception signal/search/unwind/resume protocol
- `doesNotUnderstand:` with a real `Message` object

Lookup, PICs, VALUE/SELF closures, home tokens and non-local return are
implemented. Exception signaling now uses the same cooperative control
sidecar: nested class/subclass handlers, unhandled propagation, precise roots
and `ensure:` execution are covered by generated x86-64 execution and ARM64
cross-assembly, without `setjmp`/`longjmp`. Retry and resignal semantics,
general CELL captures and nested escaping closures remain explicit future
protocols, not hidden fallbacks.

### Numbers and characters

- exact SmallInteger arithmetic with promotion to LargeInteger
- arbitrary-precision signed Integer arithmetic, shifts, comparison, parsing,
  and printing
- Fraction normalization and mixed numeric coercion
- boxed binary64 Float operations with deterministic NaN, signed-zero, and
  floating-environment behavior
- Unicode-scalar Character construction and comparison

SmallInteger, Character and boxed binary64 runtime operations exist.
LargeInteger now uses an authenticated immutable sign/magnitude layout with
little-order base-2^32 limbs. Promotion/demotion, mixed Small/Large add,
subtract, schoolbook multiply, floor division/modulo, comparison, arithmetic
shifts, deterministic value hashing and exact binary64 conversion are covered
by differential oracles, generated x86-64 execution and ARM64 cross-assembly.
Parsing/printing arbitrary-precision integers remains separate language-library
work. Canonical Fraction construction, exact mixed Integer/Fraction arithmetic,
comparison and rounding now live in the source image; mixed Float operations
cross the explicit `asFloat` boundary. The boxed Float protocol is
now wired through honest runtime symbols rather than unsupported compiler
intrinsics. Its arithmetic, ordered comparisons, NaN propagation, signed-zero
hash/equality contract, strict floating environment and four Float-to-Integer
rounding modes have native x86-64 execution and ARM64 cross-assembly coverage.
Finite results outside the tagged range are promoted directly through the
shared numeric context.

### Collections and text

- `Collection`, `SequenceableCollection`, and `ArrayedCollection` iteration and
  bounds contracts
- Array, ByteArray, String, Symbol, OrderedCollection, Association, Dictionary,
  and Set
- Unicode-scalar String comparison, equality, hashing, concatenation, slicing,
  replacement, and conversion
- immutable, interned Symbols with identity stable for the image lifetime

String equality/hash/comparison/concatenation are implemented across the
configured 8-, 16-, and 32-bit element shapes. Symbol interning is also
implemented as an image-owned Robin Hood table: it validates exact live
String/Symbol class and shape metadata, canonicalizes Unicode scalars across
all widths, publishes immutable stable identities transactionally, and exposes
its entries as precise GC roots. The remaining collection algorithms belong
primarily in Smalltalk and must not become a large C primitive catalog.

### Streams and platform services

- abstract Stream protocol
- ExternalStream descriptor ownership and error reporting
- `Transcript` as an authenticated bootstrap global over stdout
- complete writes across interruption and short writes
- filesystem and process services in optional platform packages

The byte-write runtime primitive and the `Transcript nextPutAll: ...; lf`
protocol exist. Metadata ABI v5 carries exact literal bytes, dense external
global mappings, dense runtime class IDs, exact shape recipes/bitmaps and a
target-native runtime descriptor set; the image runtime materializes immutable Strings and the
authenticated stdout `ExternalStream`, and generated x86-64 code exercises
both loads and the Stream bridge. What remains is the product-facing driver
that writes the already deterministic artifacts, invokes the assembler and
system linker, and selects the application entry point.

### Reflection, contexts, and tooling

- method/class dictionaries and selector lookup
- `perform:` and `doesNotUnderstand:`
- materialized contexts with source/safepoint information
- stack traces and debugger-safe inspection
- immutable AOT compiled-method/block descriptors

Reflection must use authenticated descriptors and stable IDs. It cannot expose
backend-private pointers or assume writable machine code.

## Completion gates

Each promoted protocol needs all of the following:

- parser and semantic tests for its source surface;
- primitive binding tests proving that only implemented handlers resolve;
- runtime tests for malformed values, foreign/interior pointers, bounds,
  arithmetic boundaries, OOM rollback, and GC interaction;
- lowering tests that verify IR and execute generated x86-64 code;
- cross-assembly or backend code-generation tests for AArch64, PPC64, PPC64LE,
  and z/Architecture;
- deterministic artifact manifests and a real assembler/linker integration;
- differential behavior tests against a language-level oracle where one is
  available.

The number of reference files or primitive names is useful for discovery, but
is never a completion criterion. The completion criterion is a closed,
executable semantic path from image source through the final linked program.
