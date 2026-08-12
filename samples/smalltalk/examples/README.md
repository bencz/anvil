# Smalltalk AOT examples

These directories contain real Smalltalk source inputs, not checked-in output.
Every application is compiled conceptually as one ordered program:

```text
st-image/manifest.txt sources, then application.manifest sources
```

`application.manifest` currently uses the same deliberately small source-list
shape as the image manifest: one relative source path per non-empty,
non-comment line, in compilation order.  The frontend regression consumes the
manifests and proves that image units precede application units in the class
graph and semantic pass.

`make smalltalk-aotc` builds the AOT application compiler. It consumes both
manifests and atomically publishes an organized assembly matrix:

```text
build/samples/smalltalk/st-aotc \
    samples/smalltalk/st-image \
    samples/smalltalk/examples/hello \
    hello HelloApplication run build/smalltalk
```

The result is stored under `build/smalltalk/hello/`. Five tagged-64 profiles
contain real assembly (`x86_64`, `arm64`, `ppc64`, `ppc64le` and `zarch`). The
matrix manifest records the five narrow targets as explicitly unsupported by
the still-unimplemented tagged-32 object ABI; it does not create fake output.
Generated assembly, objects and executables are build products and are not
checked into the source directory.

To compile, link and run `hello` in one workflow from the repository root, use
a fresh output directory:

```sh
make smalltalk-app \
    ST_APP_DIR=samples/smalltalk/examples/hello \
    ST_APP_NAME=hello \
    ST_ENTRY_CLASS=HelloApplication \
    ST_ENTRY_SELECTOR=run \
    ST_APP_OUTPUT_DIR="$PWD/build/smalltalk-run"

./build/smalltalk-run/hello/native/host/hello
```

See the parent [README](../README.md#creating-compiling-and-running-an-application)
for the complete application layout, manifest rules, generated profile tree,
entrypoint contract and a from-scratch source example.

- `hello` uses the reference-compatible protocol exactly:
  `Transcript nextPutAll: 'Hello from Anvil Smalltalk'; lf`.  The frontend gate
  validates both sends. The end-to-end regression compiles the complete image
  and application, materializes all five assembly profiles, assembles and links
  the x86-64 profile with the product runtime, executes it, and requires exit
  status zero, empty stderr and the exact bytes
  `Hello from Anvil Smalltalk\n` on stdout. `Transcript` is the authenticated
  descriptor-1 `ExternalStream`; output uses the real `StreamWritePrimitive`.
- `closures` exercises an escaping value capture, a self capture and a
  non-local return. Its end-to-end gate uses the same matrix and native
  toolchain as `hello`; the executable must produce no output and return exit
  status `42`. The `run` entrypoint executes all three closure paths before
  returning the result of the compiled escaping adder.
