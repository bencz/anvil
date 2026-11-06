# ANVIL

A C library for compiler code generation: a portable SSA intermediate
representation, a target-independent optimizer, a shared MachineIR layer with
register allocation, and assembly printers for x86, x86-64, ARM64, PowerPC and
the IBM mainframe family.

ANVIL's goal is code as good as clang/LLVM's at `-O2`, through generic IR and
MachineIR optimizations that every target shares. Target policy lives in
descriptors and backend vtables, never in the passes. The
[benchmarks](#benchmarks-anvil-vs-llvm) compare the two kernel by kernel.

## Features

* **Portable source IR**: modules, functions, basic blocks, SSA values, typed
  constants, globals, aggregates, atomics, varargs and structured control flow.
* **Verifier**: `anvil_module_codegen()` rejects invalid IR before
  optimization or backend lowering; the pass manager re-verifies after every
  pass.
* **Optimizer**: SROA, mem2reg, SCCP with known bits, GVN/CSE, strength
  reduction, memory optimizations, CFG simplification, LICM, tail recursion,
  inlining, complete and runtime loop unrolling, loop strength reduction,
  compile-time loop tables, loop and SLP vectorization, and idiom recognizers
  (range tests, byte compares, counter loops). Each idiom is mapped to each
  ISA's best form.
* **MachineIR**: target-independent virtual registers, fixed ABI registers,
  frame and spill slots, copy coalescing, linear-scan allocation with lifetime
  holes and copy hints, rematerialization, spill splitting, shrink-wrapping
  (`anvil_mir_split_early_return`), block layout and constant hoisting.
* **Backends**: x86, x86-64, ARM64, PowerPC 32/64/64LE, S/370, S/370-XA,
  S/390 and z/Architecture. All lower through MachineIR, with ABI descriptors
  and backend vtables.
* **CPU models**: instruction and SIMD choices follow the selected CPU's
  features, for example SSE2/AVX on x86, LOCGR/TMLL/LLGC on z/Architecture,
  and `isel` on PowerPC.
* **Assembly output**: GAS for x86, x86-64, ARM64 and PowerPC (ELF, Mach-O,
  COFF), HLASM for the mainframes.

## Supported Architectures

| Architecture | Bits | Endianness | Stack | FP Format | ABI | Syntax |
|--------------|------|------------|-------|-----------|-----|--------|
| x86 | 32 | Little | Down | IEEE 754 | System V / Darwin / Windows | GAS |
| x86-64 | 64 | Little | Down | IEEE 754 | System V / Darwin / Windows x64 | GAS |
| S/370 | 24 | Big | Up | HFP | MVS | HLASM |
| S/370-XA | 31 | Big | Up | HFP | MVS | HLASM |
| S/390 | 31 | Big | Up | HFP | MVS | HLASM |
| z/Architecture | 64 | Big | Up | HFP+IEEE | MVS | HLASM |
| PowerPC 32 | 32 | Big | Down | IEEE 754 | System V | GAS |
| PowerPC 64 | 64 | Big | Down | IEEE 754 | ELFv1 | GAS |
| PowerPC 64 LE | 64 | Little | Down | IEEE 754 | ELFv2 | GAS |
| ARM64 (Linux) | 64 | Little | Down | IEEE 754 | AAPCS64 | GAS |
| ARM64 (macOS) | 64 | Little | Down | IEEE 754 | Darwin | GAS |

**Floating-point formats:**
- **IEEE 754**: standard binary floating point.
- **HFP**: IBM hexadecimal floating point (base-16 exponent), used on S/370
  and S/390.
- **HFP+IEEE**: z/Architecture supports both.

**OS ABI variants:**
- **System V**: the standard Unix/Linux ABI.
- **Darwin**: the macOS ABI (underscore prefix, Mach-O).
- **MVS**: ANVIL's MVS-oriented arena linkage; native interoperability is
  incomplete (see [SystemZ notes](#systemz-family-notes)).

## Building

```bash
make                         # library and examples
make lib                     # library only
make tests                   # core, optimizer, MachineIR/regalloc, backend and Smalltalk regressions
make test-examples           # generate, assemble, link and run the runtime example
make test-examples-advanced  # fp_math_lib, dynamic_array, base64_lib
make test-aggregate-abi      # aggregate ABI against the C compiler, every target
make bench-ir                # build the IR-level benchmark generator (bench/ir/run.sh runs it)

make -C samples/mcc          # the MCC C compiler sample
make -C samples/mcc test-all # MCC unit, syntax, negative, codegen, cross and layout tests
make -C samples/mcc test-exec
make -C samples/smalltalk test

make clean
sudo make install
```

### Native Windows validation

Use Python 3, Clang/LLVM, and the MSVC libraries plus Windows SDK. The validated
toolchain uses Clang's integrated assembler to produce COFF objects and native
PE executables; MASM, NASM, FASM, MinGW and Wine are not needed for this path.

```powershell
py -3 tests/run_windows.py --cc C:/llvm/bin/clang.exe --scope compiler
py -3 tests/run_windows_mcc.py --cc C:/llvm/bin/clang.exe
py -3 tests/run_windows_sanitize.py --cc C:/llvm/bin/clang.exe
py -3 tests/run_windows_abi.py --cc C:/llvm/bin/clang.exe --family aggregate
py -3 tests/run_windows_abi.py --cc C:/llvm/bin/clang.exe --family variadic
py -3 tests/fuzz_windows_optimizer.py --cc C:/llvm/bin/clang.exe --cases 128

# Full audit, including Smalltalk's currently unsupported POSIX services.
# This command returns nonzero while those failures remain.
py -3 tests/run_windows.py --cc C:/llvm/bin/clang.exe --scope all
```

The compiler and logs are kept in `build/windows`. MCC accepts
`-arch=x86_64_windows` (alias `win64`); it uses LLP64 and the Windows calling
conventions. For a generated program using the sample stdio headers:

```powershell
./build/windows/mcc.exe -arch=win64 -std=c99 -Isamples/mcc/includes -o build/windows/hello.s input.c
C:/llvm/bin/clang.exe build/windows/hello.s -Xlinker legacy_stdio_definitions.lib -o build/windows/hello.exe
./build/windows/hello.exe
```

Host services use platform vtables selected by the build (`HOST_PLATFORM=posix`
or `windows` in Makefiles). Target ABI selection is independent of the host.
See [the Windows source review](doc/WINDOWS_REVIEW.md) for tested coverage and
known defects.

## Basic Usage

```c
#include <anvil/anvil.h>
#include <anvil/anvil_opt.h>

int main(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);   /* O2 */
    anvil_module_t *mod = anvil_module_create(ctx, "my_module");

    /* int add(int a, int b) */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *func = anvil_func_create(mod, "add", func_type, ANVIL_LINK_EXTERNAL);

    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_value_t *a = anvil_func_get_param(func, 0);
    anvil_value_t *b = anvil_func_get_param(func, 1);
    anvil_build_ret(ctx, anvil_build_add(ctx, a, b, "result"));

    /* Verifies, optimizes at the context's level, lowers and prints. */
    char *output = NULL;
    size_t len = 0;
    if (anvil_module_codegen(mod, &output, &len) != ANVIL_OK) {
        fprintf(stderr, "codegen failed: %s\n", anvil_ctx_get_error(ctx));
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    printf("%s", output);

    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    return 0;
}
```

## Supported IR Operations

* **Arithmetic**: `add`, `sub`, `mul`, `sdiv`/`udiv`, `smod`/`umod`, `neg`;
  `anvil_set_no_wrap` marks exact results.
* **Bitwise**: `and`, `or`, `xor`, `not`, `shl`, `shr` (logical), `sar`
  (arithmetic).
* **Comparison**: `cmp_eq`/`cmp_ne`, `cmp_lt`/`le`/`gt`/`ge` and unsigned
  `_ult`/`_ule`/`_ugt`/`_uge`. An ordered compare's signedness must match its
  operand type. `fcmp` takes a predicate.
* **Memory**: `alloca`, `alloca_dyn`, `load`, `store` (with volatile and
  atomic forms), `gep`, `struct_gep`, globals (`anvil_module_add_global`).
* **Control flow**: `br`, `br_cond`, `switch`, `call` (direct and indirect),
  `ret`/`ret_void`.
* **Conversions**: `trunc`, `zext`, `sext`, `bitcast`, `ptrtoint`/`inttoptr`,
  `fptrunc`, `fpext`, `fptosi`/`fptoui`, `sitofp`/`uitofp`.
* **Floating point**: `fadd`, `fsub`, `fmul`, `fdiv`, `fneg`, `fabs`, and
  `fma` through contraction (`anvil_func_set_fp_contraction`).
* **Other**: `phi` (with `anvil_phi_add_incoming`), `select`, vectors, atomics
  (8–64 bits: compare/exchange, read-modify-write and fences), `va_arg` /
  `va_copy`.

## Supported Types

* Boolean: `i1` (one semantic bit, one-byte storage)
* Integers: `i8`, `i16`, `i32`, `i64` (signed) and `u8`, `u16`, `u32`, `u64`
  (unsigned)
* Floating point: `f32`, `f64`
* Pointers: `anvil_type_ptr(ctx, pointee_type)`
* Arrays: `anvil_type_array(ctx, elem_type, count)`
* Structs and unions: `anvil_type_struct(ctx, name, fields, num_fields)`
* Vectors: integer and floating-point lanes, as the target's cost hook accepts
* Functions: `anvil_type_func(ctx, ret_type, params, num_params, variadic)`;
  `anvil_type_func_cc` and `anvil_type_func_attrs` add a calling convention
  and parameter attributes (`BYVAL`, `SRET`).

### Function pointers

Definitions and declarations keep their `ANVIL_TYPE_FUNC` signature, but the
callable value from `anvil_func_get_value()` is a `ptr<func>`. It can be
stored, loaded and called indirectly, as in C:

```c
anvil_type_t *fn_ptr_type = anvil_type_ptr(ctx, fn_type);
anvil_value_t *slot = anvil_build_alloca(ctx, fn_ptr_type, "slot");
anvil_build_store(ctx, anvil_func_get_value(add_fn), slot);
anvil_value_t *loaded = anvil_build_load(ctx, fn_ptr_type, slot, "loaded_fn");
anvil_value_t *args[] = { anvil_const_i32(ctx, 3), anvil_const_i32(ctx, 4) };
anvil_value_t *result = NULL;
anvil_build_call_checked(ctx, loaded, args, 2, "result", &result);
```

`anvil_build_call_checked()` takes the signature and the effective calling
convention from that type. It returns `true` with a NULL result for a void
call.

## Calling Conventions

| Architecture | Convention | Description |
|--------------|------------|-------------|
| x86 | CDECL (default), STDCALL, FASTCALL | Parameters on the stack, caller cleanup; FASTCALL passes two integers in ECX/EDX |
| x86-64 | System V / Windows x64 | RDI, RSI, RDX, RCX, R8, R9 / RCX, RDX, R8, R9 + shadow space |
| S/370, S/390 | ANVIL MVS-oriented arena | R1 points to the parameter list |
| z/Architecture | ANVIL arena-64 | R1 points to the parameter list (64-bit) |
| PPC32 | System V | r3-r10 for arguments, r3 for the result |
| PPC64 BE | ELFv1 | r3-r10, function descriptors |
| PPC64 LE | ELFv2 | r3-r10, local entry points |
| ARM64 (Linux) | AAPCS64 | x0-x7 for arguments, x0 for the result |
| ARM64 (macOS) | Apple ARM64 | x0-x7, variadic arguments on the stack, underscore prefix |

An internal, non-variadic function whose every use is a direct call may take a
cheaper convention offered by its backend
(`anvil_backend_ops_t.local_call_convention`). On i386 that is FASTCALL, so a
`static` helper receives its first two integer arguments in registers, as with
clang's internal convention. Targets that already pass arguments in registers
offer none.

## Optimizer

Optimization is target-independent and runs before backend lowering. The pass
manager runs its passes in a fixed order and repeats them to a bounded
fixpoint. The bound defaults to 10 and is set with
`anvil_pass_manager_set_iteration_limit()`. A failing pass, invalid IR or
failure to converge is an error, never a silent success. Every level includes
the one below it:

| Level | Constant | Passes |
|-------|----------|--------|
| O0 | `ANVIL_OPT_NONE` | none (default) |
| Og | `ANVIL_OPT_DEBUG` | copy propagation, store-load propagation |
| O1 | `ANVIL_OPT_BASIC` | Og + mem2reg, constant folding, DCE; codegen preparation |
| O2 | `ANVIL_OPT_STANDARD` | O1 + SROA, SCCP, known bits, GVN, CSE, strength reduction, dead stores, load elimination, CFG simplification, switch tables, LICM with scalar promotion, closed forms of counting loops, tail recursion, inlining (small bodies, single call sites), complete unrolling, loop strength reduction, runtime unrolling, loop tables, loop vectorization |
| O3 | `ANVIL_OPT_AGGRESSIVE` | O2 + larger inlining budgets, SLP vectorization |

Highlights (details in [doc/OPTIMIZATION.md](doc/OPTIMIZATION.md)):

* **Strength reduction**: power-of-two arithmetic; division by any constant
  through a multiply-high where the target prices it; range tests
  `lo <= x && x <= hi` become one unsigned compare; compares of extended
  narrow values compare the narrow values (a C `char` promoted to `int` is
  compared as a byte).
* **Loop strength reduction**: pointer streams where an index costs a
  multiply or the target's accesses update their base (PowerPC `lbzu`, ARM64
  `[x, #k]!`). Lockstep pointers share one index where `base + index` is free
  (x86, mainframes). Counters run to zero on targets with counter branches
  (PowerPC CTR, SystemZ BRCT), and meeting indexes become a trip counter.
* **Runtime unrolling**: small counted loops get a copy unrolled U times where
  the target asks for it (`anvil_arch_info_t.small_loop_unroll`).
* **Loop tables**: a loop computing a constant table, such as CRC32's 256
  entries, runs at compile time into a read-only global.
* **Vectorization**: counted loops (with early exits), open searches (a scan
  for a terminator or delimiter, reading aligned vectors that cannot fault
  past the scalar loop's reads) and SLP groups become vectors only where the
  backend's `vector_operation_cost` hook accepts them for the CPU model.
  Floating-point lanes vectorize by default, with results identical to the
  scalar code; `anvil_func_set_fp_vectorization(func, false)` (MCC:
  `-fno-fp-vectorize`) keeps them scalar. Reductions also need
  `anvil_func_set_fp_reassociation`.
* **Codegen preparation (O1+)**: late loop strength rounds, address sinking,
  loads whose sign no reader observes, local calling conventions, and removal
  of internal functions nothing alive refers to.

Custom passes, per-pass enabling, statistics and a pass observer are
available through `anvil_opt.h`:

```c
anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(ctx);
anvil_pass_manager_enable(pm, ANVIL_PASS_CONST_FOLD);
anvil_pass_manager_disable(pm, ANVIL_PASS_DCE);
anvil_module_optimize(mod);
```

## MachineIR and Register Allocation

`include/anvil/anvil_machine.h` exposes the target-independent MachineIR that
every backend lowers to:

- typed virtual registers (GPR/FPR/flags/special), fixed ABI registers, frame,
  spill and dynamic-allocation slots;
- blocks, branches, direct and indirect calls, string literals;
- copy coalescing (including a loop back edge's PHI move into its source),
  local CSE, address-arithmetic folding, constant hoisting, test-block
  duplication, compare sinking, and shrink-wrapping of early returns;
- linear-scan allocation by register class with per-block live segments
  (lifetime holes), chained copy hints, register views of free extensions,
  spill splitting around clobbers, rematerialization and spill-slot reuse;
- a verifier for structural invariants and definite assignment, before and
  after allocation.

The printers share the isel facts (constants, known-zero widths, single
readers, registerless values) and block layout. Each one maps the shared
idioms to its ISA: `test`/`adc`/memory operands on x86, `tbz`/`cinc`/`ldp`/
writeback on ARM64, record forms/`isel`/update forms on PowerPC, and
`TMLL`/`LOCGR`/`BRCT` on SystemZ.

## Benchmarks: ANVIL vs LLVM

Two benchmark sets compare ANVIL with clang/LLVM at `-O2`, on the same
algorithms, and check that both programs print the same output.

* **`bench/ir`** isolates code generation. The kernels are written directly
  against the ANVIL API (`bench/ir/kernels`) and in C for clang
  (`bench/ir/ref`, built with `-fno-builtin`). Both are linked with the same
  driver, and neither is inlined into it. x86-64 and i386 run natively and
  report the best of N runs in milliseconds (`--runs`, 10 below). ARM64 and PowerPC run under
  qemu-user on reduced workloads and report dynamic instruction counts.
  Instruction counts are not time, but they are exact and reproducible.
* **`samples/mcc/tests/bench`** compiles the same C files with MCC (the C
  front end sample on ANVIL) and with clang. It includes front-end effects
  such as C integer promotions and the floating-point policy.

Machine: Intel Core i9-11900K (Linux 7.2, Fedora 44), clang 22.1.8,
qemu 10.2.2. Ratio = ANVIL / clang, so **below 1.00 means ANVIL is better**.

### Code generation (`bench/ir`, ANVIL API vs clang -O2)

Native targets, best of 10 runs, in milliseconds (± about 5% between runs on
kernels this short):

| kernel | what it stresses | x86-64 clang | x86-64 ANVIL | ratio | i386 clang | i386 ANVIL | ratio |
|--------|------------------|-------------:|-------------:|------:|-----------:|-----------:|------:|
| fib | recursive calls, prologues, argument passing | 23 | 22 | 0.96 | 19 | 21 | 1.11 |
| crc32 | table-driven bit loop, table built by a loop | 102 | 103 | 1.01 | 103 | 103 | 1.00 |
| sieve | byte stores, nested counted loops | 115 | 109 | 0.95 | 115 | 111 | 0.97 |
| matmul | integer multiply-accumulate, strided loads | 20 | 21 | 1.05 | 22 | 22 | 1.00 |
| strings | strlen / strcmp / strcpy / reverse byte loops | 76 | 51 | 0.67 | 77 | 46 | 0.60 |
| qsort | recursion, compares, swaps, branches | 174 | 176 | 1.01 | 169 | 178 | 1.05 |
| lexer | a 74-case switch classifying bytes | 30 | 36 | 1.20 | 28 | 36 | 1.29 |
| hex | hex encoding: signed division of promoted bytes, selects, two stores | 47 | 49 | 1.04 | 53 | 87 | 1.64 |
| bigprime | transform butterflies: 64-bit products modulo a prime | 163 | 181 | 1.11 | 329 | 490 | 1.49 |
| cells | a tape machine's drain loop (computed in closed form) | 2 | 3 | 1.50 | 2 | 2 | 1.00 |

i386 targets `pentium4` (SSE2), the CPU clang assumes for `-m32`.

Under qemu-user, dynamic instructions executed (exact, the same on every run):

| kernel | ARM64 clang | ARM64 ANVIL | ratio | ppc64le clang | ppc64le ANVIL | ratio |
|--------|------------:|------------:|------:|--------------:|--------------:|------:|
| fib | 201,459 | 193,099 | 0.96 | 322,936 | 280,749 | 0.87 |
| crc32 | 533,510 | 533,301 | 1.00 | 468,161 | 451,974 | 0.97 |
| sieve | 2,109,042 | 1,620,757 | 0.77 | 2,123,809 | 1,620,784 | 0.76 |
| matmul | 214,390 | 207,910 | 0.97 | 340,798 | 286,037 | 0.84 |
| strings | 253,432 | 196,936 | 0.78 | 292,630 | 242,296 | 0.83 |
| qsort | 800,784 | 826,725 | 1.03 | 1,138,113 | 994,093 | 0.87 |
| lexer | 835,870 | 966,950 | 1.16 | 1,014,229 | 981,481 | 0.97 |
| hex | 1,149,427 | 1,346,047 | 1.17 | 1,183,965 | 1,380,549 | 1.17 |
| bigprime | 722,178 | 940,628 | 1.30 | 931,585 | 1,028,757 | 1.10 |
| cells | 201,973 | 364,157 | 1.80 | 267,726 | 503,604 | 1.88 |

| kernel | ppc64 (BE) clang | ppc64 ANVIL | ratio | ppc32 clang | ppc32 ANVIL | ratio |
|--------|-----------------:|------------:|------:|------------:|------------:|------:|
| fib | 316,168 | 294,276 | 0.93 | 294,383 | 261,546 | 0.89 |
| crc32 | 534,069 | 484,989 | 0.91 | 534,616 | 485,157 | 0.91 |
| sieve | 2,014,004 | 1,620,785 | 0.80 | 2,800,582 | 1,620,889 | 0.58 |
| matmul | 388,710 | 387,247 | 1.00 | 390,554 | 389,090 | 1.00 |
| strings | 289,547 | 289,559 | 1.00 | 289,618 | 289,810 | 1.00 |
| qsort | 1,012,850 | 998,492 | 0.99 | 1,049,861 | 877,019 | 0.84 |
| lexer | 991,606 | 1,073,602 | 1.08 | 991,637 | 942,565 | 0.95 |
| hex | 1,386,276 | 1,574,924 | 1.14 | 1,386,524 | 1,575,180 | 1.14 |
| bigprime | 970,260 | 1,099,375 | 1.13 | 111,575,208 | 113,357,198 | 1.02 |
| cells | 271,080 | 498,796 | 1.84 | 271,311 | 499,015 | 1.84 |

ANVIL comes out ahead with counter branches and update-form streams on
PowerPC (`lbzu`/`stbu` + `bdnz`), compile-time tables (crc32), lockstep
pointers sharing one index on x86, in-place index updates, and vectorized open
searches (strings). Switches that only choose values become table lookups on
every target (lexer).

### C compiler sample (`samples/mcc/tests/bench`, MCC -O2 vs clang -O2)

Best of 10 runs, in milliseconds:

| kernel | x86-64 clang | x86-64 MCC | ratio | i386 clang | i386 MCC | ratio |
|--------|-------------:|-----------:|------:|-----------:|---------:|------:|
| andlut (AND/OR/XOR through a nibble table) | 232 | 426 | 1.84 | 241 | 451 | 1.87 |
| bigprime (number-theoretic transforms, 64-bit products modulo a prime) | 299 | 368 | 1.23 | 367 | 494 | 1.35 |
| calls (small static helpers, structs by value) | 97 | 87 | 0.90 | 97 | 167 | 1.72 |
| crc32 | 70 | 72 | 1.03 | 62 | 73 | 1.18 |
| fib | 22 | 23 | 1.05 | 18 | 24 | 1.33 |
| hashmap | 28 | 29 | 1.04 | 28 | 29 | 1.04 |
| hex (signed division of promoted bytes) | 46 | 47 | 1.02 | 46 | 87 | 1.89 |
| lexer (74-case switch) | 28 | 37 | 1.32 | 28 | 36 | 1.29 |
| matflat (double, run-time order, flat int indices) | 13 | 14 | 1.08 | 14 | 15 | 1.07 |
| matmul (double) | 14 | 15 | 1.07 | 15 | 15 | 1.00 |
| nbody (double) | 80 | 99 | 1.24 | 83 | 99 | 1.19 |
| pidigits (a Brainfuck program as C, 30 digits of pi) | 7 | 6 | 0.86 | 6 | 6 | 1.00 |
| qsort | 107 | 109 | 1.02 | 106 | 109 | 1.03 |
| sieve | 91 | 93 | 1.02 | 100 | 184 | 1.84 |
| strings | 18 | 18 | 1.00 | 21 | 22 | 1.05 |

How to reproduce:

```bash
bench/ir/run.sh --arch=x86_64 --runs=10   # also x86 (i386, --cpu=pentium4 by default), arm64, ppc64, ppc64le, ppc32
bench/ir/run.sh --arch=arm64 --asm=/tmp/asm   # keep both compilers' assembly side by side
samples/mcc/tests/bench/run_bench.sh --runs=10
```

## SystemZ Family Notes

The S/370, S/370-XA, S/390 and z/Architecture implementations live in
`src/backend/systemz/`, with ISA descriptors in `targets/`, linkage policies in
`abi/` and HLASM printing in `asm/`. Instruction choices follow the CPU model's
facilities: relative branches and immediates, extended immediates, load and
store on condition, IEEE floating point and 64-bit registers.

These backends have real MIR lowering, allocation and text emission. The
current linkage assumes caller-owned arena storage. It is not certified
against native z/OS, Language Environment or GCCMVS, and its 144-byte layout
is not the IBM F4SA layout. HLASM output cannot run here, so its correctness
rests on unit tests and review. [Implementation notes and IBM manual
references](doc/MAINFRAME.md) document the profiles, ABI discrepancies and
the validation still required.

## Source Organization

Backends are grouped by instruction-set family:

```
src/core/        IR, types, builder, verifier, context, CPU table
src/opt/         target-independent passes and codegen preparation
src/machine/     MachineIR, liveness, coalescing, register allocation, MIR verifier
src/backend/common/  shared lowering helpers, isel facts, block layout, vector ISA tables, GAS data
src/backend/x86/     i386 and x86-64: targets/, abi/, asm/ (GAS printers), lowering and legality
src/backend/arm64/   ARM64 (AAPCS64, Darwin)
src/backend/ppc/     PowerPC 32/64/64LE
src/backend/systemz/ S/370 .. z/Architecture (HLASM)
```

Target policy (word size, ABI, address forms, counter branches, unrolling,
local conventions) lives in `anvil_arch_info_t`, the ABI descriptors and
`anvil_backend_ops_t`, never in branches inside lowering or the printers. See
[the backend guide](doc/BACKENDS.md) and
[the architecture overview](doc/ARCHITECTURE.md).

## Adding New Backends

1. Create the backend under `src/backend/<arch>/`, lowering through MachineIR
   so allocation, spills, fixed ABI registers and frame slots stay shared.
2. Implement `anvil_backend_ops_t`:

```c
const anvil_backend_ops_t anvil_backend_myarch = {
    .name = "MyArch",
    .arch = ANVIL_ARCH_MYARCH,
    .init = myarch_init,
    .cleanup = myarch_cleanup,
    .reset = myarch_reset,                    /* clear cached IR pointers */
    .prepare_ir = myarch_prepare_ir,          /* optional IR legalization */
    .codegen_module = myarch_codegen_module,
    .codegen_func = myarch_codegen_func,
    .get_arch_info = myarch_get_arch_info,
    .vector_operation_cost = myarch_cost,     /* optional: what vectorizes */
    .local_call_convention = myarch_local_cc, /* optional */
};
```

3. Add the architecture to `anvil_arch_t` in `anvil.h`, its `anvil_arch_info_t`
   in `src/core/context.c`, and register the backend in `backend.c`.

## Samples

* **MCC** (`samples/mcc`): a C89–C23 compiler (with GNU extensions) on ANVIL,
  and the integration stress test for the IR, verifier, optimizer and every
  backend. Its 103 execution tests are checked against clang on x86-64
  (all opt levels, haswell with and without FP vectorization), i386 (i486 and
  pentium4) and, under qemu, ARM64, ppc64le, ppc64 and ppc32. Generated code
  is checked to be deterministic. See [samples/mcc/README.md](samples/mcc/README.md).
* **Smalltalk** (`samples/smalltalk`): an ahead-of-time Smalltalk built on
  ANVIL: lexer, parser, semantic analysis, a whole-image AOT compiler and a
  native runtime (dispatch, closures, non-local return, exceptions, fibers,
  GC). `make smalltalk-app` builds an application from its manifest and the
  `st-image` sources into a native executable. See
  [samples/smalltalk/README.md](samples/smalltalk/README.md).
* **Examples** (`examples/`): a runtime example and three libraries generated
  through the API and linked with C test programs (`fp_math_lib`,
  `dynamic_array`, `base64_lib`).

## IR Debug/Dump API

```c
anvil_print_module(mod);              /* to stdout */
anvil_dump_func(stderr, func);        /* also _module, _block, _instr, _value, _type */
char *text = anvil_module_to_string(mod);
free(text);
```

String constants are escaped in dumps (`\n`, `\t`, `\0`, `\xHH`). MCC writes
the IR before or after optimization with `-dump-ir` / `-dump-ir-opt`.

## Roadmap

* Source-level debug information through assembler directives (`.file`/`.loc`
  line tables and DWARF sections written as assembly, as `.cfi` unwinding
  already is)
* RISC-V
* Native z/OS linkage (F4SA, Language Environment) and validation on hardware
* Broader vectorization and idiom recognition

## Documentation

Contributors and agents follow [AGENTS.md](AGENTS.md). See [doc/INDEX.md](doc/INDEX.md) for the documentation map: API
([doc/API.md](doc/API.md)), IR ([doc/IR.md](doc/IR.md)), optimizer
([doc/OPTIMIZATION.md](doc/OPTIMIZATION.md)), backends
([doc/BACKENDS.md](doc/BACKENDS.md)) and the review log
([doc/REVIEW.md](doc/REVIEW.md)). `DOCUMENTATION.md` holds the API reference
with usage examples.

## License

Unlicense
