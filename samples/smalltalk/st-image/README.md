# Smalltalk source image

This directory is the mandatory AOT language image. It contains ordinary
Smalltalk source files, not a serialized heap, bytecode image, generated C, or
pre-linked machine code.

The compiler always processes the image before application sources:

```text
manifest -> image sources -> application sources -> class graph/sema
         -> Anvil IR -> assembly -> object files -> system linker + RT
```

Manifest order is deterministic and participates in bootstrap semantics. The
loader parses every image unit before it parses any application unit. Class
construction then declares the complete image, declares the application on
top of it, resolves inheritance and layouts, and only afterward analyzes and
lowers method bodies. Forward superclass references are therefore resolved
without making method-body meaning depend on file order; an extension still
must follow the class it extends. Application sources may add subclasses and
explicit extensions, but cannot silently replace an image class or duplicate
a method.

The source image contains classes covering the object/class
kernel, booleans, exceptions, the immediate/arbitrary-precision numeric roots,
indexed collections, associations, ordered collections, sets, dictionaries,
strings/symbols, closures, Unicode read/write streams, cooperative fibers,
sockets and the HTTP sample protocol. Primitive pragmas are
required AOT link contracts, not optional no-ops. The compiler currently
resolves primitive uses to typed intrinsics or real RT entries and rejects a build
if any implementation or required fallback is absent. Whole-image lowering
still has explicit capability gates for unsupported source constructs.

See the [HTTP sample](../examples/web-http-server/README.md) for connection
lifetime, platform adapters and compiled Windows/Linux validation commands.

Source loading is transactional and bounded. Defaults cap the manifest at
4 MiB, each source at 64 MiB, the combined source text at 512 MiB and the file
count at 65,536; embedders can supply smaller explicit limits. The reader
checks both `fstat` and actual bytes read, so a file that grows after opening
cannot escape its budget.

The runtime (`RT`) implements only representation, allocation/GC, primitive
operations, dispatch miss handling, exceptions/unwind and process/OS bridges.
Language behavior such as `Object`, `UndefinedObject`, `Boolean`, `True` and
`False` lives here and is compiled through the same Anvil pipeline as the
application.

Block evaluation crosses into RT only to enter already compiled closures. The
five `BlockValue*` runtime symbols verify closure identity and exact arity,
while `BlockWhileTruePrimitive` performs the iterative condition/body loop
without recursive C calls. They construct rooted child frames and propagate
rather than consume pending non-local returns. `valueWithArguments:`
additionally requires the image-configured exact Array shape; every Smalltalk
fallback signals an error instead of fabricating a result.

`BoxedFloat64` preserves raw IEEE-754 binary64 bits. Float equality is
deliberately Float-only and treats signed zeros as equal; `Float>>hash`
therefore normalizes `-0.0` to the same deterministic, address-independent
hash as `+0.0`. Ordered comparisons and arithmetic accept Integers through
explicit `asFloat` coercion in the image. Rounded conversions are continued
by the authenticated numeric sidecar and publish canonical SmallInteger or
LargeInteger results; allocation failure never retries the primitive.

Exact division constructs canonical `Fraction` values. Their denominator is
strictly positive and nonzero, numerator and denominator are reduced by their
greatest common divisor, zero is the immediate Integer zero, and a denominator
of one demotes to its canonical Integer. Mixed Fraction/Integer arithmetic and
ordering remain exact through arbitrary-precision Integer operations; a mixed
Float operation performs the explicit `asFloat` conversion requested by that
operation. Float equality stays Float-only, preserving the equality/hash
contract instead of silently equating a rounded binary64 value with a rational.

`Transcript` is intentionally an external bootstrap global, not a magic class
or a sample-specific primitive. The image runtime allocates one authenticated
`ExternalStream`, stores descriptor `1` as a SmallInteger and publishes that
object as a GC root before application entry. String-literal materialization,
global loading, runtime-symbol lowering and the real `StreamWritePrimitive`
are covered by generated x86-64 execution and ARM64 assembly gates. The
remaining application tool must orchestrate those existing contracts and the
system linker; it must not replace them with host-side simulated output.

There is no JIT or runtime machine-code generation. Offline profiles may guide
the AOT compiler, but the resulting code is assembled and linked before the
program starts.
