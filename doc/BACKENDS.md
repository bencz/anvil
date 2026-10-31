# ANVIL Backend Development Guide

This document explains how backends work and how to implement new ones.

## Backend Architecture

### Overview

A backend translates ANVIL IR to target-specific assembly code. Each backend is a self-contained module that implements the `anvil_backend_ops_t` interface.

### CPU Model System

Backends can access CPU model information to generate optimized code for specific processors. The context provides:

```c
// Check if a CPU feature is available
if (anvil_ctx_has_feature(ctx, ANVIL_FEATURE_PPC_VSX)) {
    // Emit VSX instructions
} else {
    // Emit fallback code
}

// Get current CPU model
anvil_cpu_model_t cpu = anvil_ctx_get_cpu(ctx);
```

Every instruction choice that depends on the processor generation must come
from these features, never from the architecture alone: the target
descriptor states what the *architecture* defines (register width, byte
order), the CPU model what the *implementation* adds.  Examples:

| Decision | Feature | Fallback |
|---|---|---|
| PowerPC `fcfid`/`fctidz` | `ANVIL_FEATURE_PPC_64BIT` (970, POWER4+) | magic-number sequence or run-time helper |
| PowerPC `fcfids`/`fcfidu`/`fctiwuz`/`fctiduz` | `ANVIL_FEATURE_PPC_FPCVT` (POWER7+) | `fcfid`+`frsp`, helpers |
| i386 floating point in SSE2 registers (`asm/gas32_sse2.c`) | `ANVIL_FEATURE_X86_SSE2` (Pentium 4+) | x87 FPU (`asm/gas32_x87.c`): FPR assignments become frame home slots, `fld`/op/`fstp` per instruction |
| i386 x87 compare with `fucomip` | `ANVIL_FEATURE_X86_CMOV` (P6 family) | `fucompp` + `fnstsw %ax` + `sahf` |
| SystemZ IEEE (BFP) arithmetic and constants | `ANVIL_FEATURE_ZARCH_BFP` (S/390 G5+, every z) | HFP only; IEEE formats are rejected |
| SystemZ HFP <-> fixed conversions (`CDFR`, `CFDR`...) | `ANVIL_FEATURE_ZARCH_HFP_EXT` (G5+) | classic unnormalized-add sequence (S/370, G1-G4) |
| SystemZ `LGFI` / `MSGFI` | `ANVIL_FEATURE_ZARCH_EIMM` (z9) / `ANVIL_FEATURE_ZARCH_GIE` (z10) | `LGF` / `MSG` from a literal |
| SystemZ `AHI`/`CHI`/`MHI` immediates | `ANVIL_FEATURE_ZARCH_IMMREL` (S/390 G1+, every z) | register forms or `C`/`CL` from a literal (S/370) |
| SystemZ `NILF`/`OILF`/`XILF` | `ANVIL_FEATURE_ZARCH_EIMM` (z9) | `N`/`O`/`X` from a literal |
| Scalar operations priced by table (`ANVIL_MIR_OP_SCALAR`: FMA, FSQRT, UMULH/SMULH) | the same tables, `ANVIL_VECTOR_SHAPE_SCALAR` rows: x86-64 FMA3 `vfmadd231sd`, `sqrtsd`, `mul`/`imul` into RDX:RAX; i386 `sqrtsd` with `ANVIL_FEATURE_X86_SSE2`, else x87 `fsqrt`, `mull`/`imull`; ARM64 `fmadd`, `fsqrt`, `umulh`/`smulh` (`umull`/`smull` + `lsr #32` for words); PowerPC `fmadd`, `fsqrt` with `ANVIL_FEATURE_PPC_FSQRT` (970, POWER), `mulhwu`/`mulhw`, `mulhdu`/`mulhd` in 64-bit mode | priced 0: front ends keep their library call (G3/G4, SystemZ); division by constants stays a division |
| 128-bit f32/f64 vectors (SLP, loop vectorizer, `splat`) | `vector_operation_cost`: x86-64 `ANVIL_FEATURE_X86_SSE2` (`addps`, `shufps`/`unpcklpd`), ARM64 `ANVIL_FEATURE_ARM64_NEON` (`fadd v.4s`, `dup`), PowerPC `ANVIL_FEATURE_PPC_VSX` (POWER7+: `lxvd2x`/`stxvd2x`, `xvaddsp`, `xxspltw`, `xxpermdi`) | scalar code |

`anvil_ctx_set_cpu()` accepts a model from the same family when it runs the
context's architecture (x86-64 CPUs for i386, 64-bit PowerPC CPUs for ppc32 and
ppc64le, z/Architecture CPUs for ESA/390); `anvil_cpu_model_lookup(arch, name)`
finds a model by name among those that run the architecture (names such as
"g4" exist in both the PowerPC and S/390 families; e.g. MCC's `-mcpu=power7`).

**Instruction selection facts.** `src/backend/common/mir_isel.c` gives every
MachineIR printer the same facts: which GPR vregs hold a known constant (their
only definition is a `MOV` of an immediate), how often each vreg is used, and
which constant `MOV`s are still needed.  Each printer supplies one callback
saying which operands it encodes as immediates (x86-64 imm32, ARM64 12-bit
arithmetic and bitmask immediates, PowerPC 16-bit D-forms, SystemZ by CPU
facility), so a folded constant never occupies a register.  The printers also
branch on the condition flags when a compare's only use is the following
`BR_COND`, turn multiplication by a power of two into a shift, and fall through
to the next block instead of jumping to it.  None of this changes the MIR
contract; it is selection inside each backend's printer.

**64-bit integers on 32-bit targets.** `src/backend/common/wide_mir.c` legalizes
64-bit operations into pairs of 32-bit virtual registers with target-independent
MachineIR (carries through unsigned compares, constant shifts inline).  A target
supplies one hook, `anvil_wide_target_t.call_helper`, to call the libgcc/compiler-rt
helpers (`__muldi3`, `__divdi3`, variable shifts, floating conversions) in its
calling convention; x86-32, PPC32 and the 31-bit SystemZ targets use it.

All actively maintained backends now follow the same MachineIR pipeline that
ARM64 established as the reference path: **lower source IR to MachineIR ->
verify target legality -> coalesce copies -> linear-scan register allocation ->
materialize spills -> emit assembly**. ARM64, x86, x86-64, and the PowerPC family
share this contract; the mainframe (S/370 family) backends use the same pipeline
through the `systemz/` family components. The linear-scan allocator and the
copy-coalescing / spill-materialization passes are target-independent and live in
`src/machine/` (`regalloc.c`, `machine_ir.c`).

**PowerPC and SystemZ domain organization**

Each family owns lowering, MIR legality, allocation and emission. Target
identity, linkage policy and assembler syntax have explicit owners:

```text
src/backend/ppc/
    ppc_lower.c, ppc_legal.c, ppc_emit.c, ppc_codegen.c
    ppc_target.c, ppc_internal.h
    targets/ppc32.c, targets/ppc64.c, targets/ppc64le.c
    abi/elf32.c, abi/elfv1.c, abi/elfv2.c

src/backend/systemz/
    systemz_lower.c, systemz_legal.c, systemz_codegen.c
    systemz_target.c, systemz_internal.h
    targets/s370.c, targets/s370_xa.c, targets/s390.c, targets/zarch.c
    abi/mvs_arena_31.c, abi/mvs_arena_64.c
    asm/hlasm.c, asm/hlasm_data.c, asm/hlasm_names.c, asm/hlasm_dispatch.c
```

PPC ABI vtables select ELF32, ELFv1 descriptor entries/calls, or ELFv2 global and
local entry conventions. PPC currently emits GAS. The descriptor still supplies
word width, endianness, frame offsets and register pools to common lowering.

SystemZ descriptors select a linkage vtable and a printer vtable. The family
name groups the S/370 lineage; it does not collapse four ISAs into one target.
The current MVS-oriented arena implementation is not a certified native z/OS
ABI. [SystemZ implementation notes](MAINFRAME.md) distinguish implemented
behavior, IBM manual requirements and outstanding incompatibilities.

To extend these families, put architecture capabilities in `targets/`, linkage
entry/call behavior in `abi/`, and syntax-specific printing in the assembler
component. Wire descriptors and vtables rather than adding architecture tests
to unrelated components or using preprocessor branches. An ABI and printer may
only be paired when their emission contracts are implemented.

The reorganization preserves public backend registration and MIR entry points.
Empty modules and declarations also validate their requested configuration.

**x86 family organization**

`src/backend/x86/` owns both 32-bit x86 and x86-64. The family name does not
imply a pointer width. Each target keeps its own lowering, legality checks and
instruction emission because their register sets and ABI requirements differ.

```text
src/backend/x86/
    x86_32_internal.h, x86_64_internal.h   Internal mode-specific contracts
    x86_32_helpers.c, x86_64_helpers.c     Register names and type helpers
    x86_32_lower.c, x86_64_lower.c         Source IR to MIR
    x86_32_legal.c, x86_64_legal.c         Legality and allocation bridge
    x86_32_codegen.c, x86_64_codegen.c     Function/module coordination and globals
    targets/
        x86_32.c                         Identity, lifecycle and backend vtable
        x86_64.c                         Identity, lifecycle and capability callbacks
    abi/
        x86_32_abi.c                     cdecl/stdcall/fastcall and symbol decoration
        x86_64_abi.c                     SysV/Darwin/Win64 descriptors and aggregate classification
        x86_64_varargs.c                 Native va_list operations
    asm/
        gas32.c                         Allocated MIR to 32-bit GAS/AT&T
        gas64.c                         Allocated MIR to 64-bit GAS/AT&T
```

The existing `anvil_backend_ops_t` vtables retain registration, codegen,
aggregate classification, varargs and capability dispatch. ABI descriptors
supply register pools, call clobbers, argument placement and frame properties.
Changing the family layout adds no architecture/platform preprocessor branches.

32-bit lowering still legalizes wide integers into pairs and implements
cdecl/stdcall/fastcall cleanup. The 64-bit path retains SysV/Darwin/Win64,
atomics, 128-bit floating-point vectors and native variadic cursors. This
organization does not make those 64-bit features available on the 32-bit path.

The public headers `include/anvil/anvil_x86_mir.h` and
`include/anvil/anvil_x86_64_mir.h`, architecture enums and function names are
unchanged. Internal component headers are not public API. The common allocator
remains in `src/machine/` rather than being duplicated into the family.

**Shared GAS data emission**

The GAS backends share data emission through `src/backend/common/gnu_data.h`
and `gnu_data.c`. The header declares the string pool and initializer interface;
the implementation handles integer/FP bit patterns, relocations, nested aggregate
layout, padding and string literals. Each backend chooses its sections and symbol
spelling through the existing callback. HLASM data emission remains in SystemZ.

**Example: ARM64 Backend Organization**

The ARM64 backend is the stable reference implementation for the MachineIR path:

```
src/backend/arm64/
├── arm64.c           # Backend lifecycle, module emission, va_* dispatch
├── arm64_internal.h  # Register constants, backend state, ABI descriptor type
├── arm64_helpers.c   # Type size/alignment and register names
├── arm64_mir.c       # Source IR -> MachineIR -> ARM64 assembly
└── abi/
    ├── aapcs64.c     # Rules both ABIs share; descriptor registry
    ├── linux.c       # AAPCS64 with ELF
    └── darwin.c      # Apple's AAPCS64 variant with Mach-O
```

**Key ARM64 Components:**
- **`arm64_mir.c`**: source IR lowering, PHI edge copies, switch lowering, MachineIR legality checks, register allocation bridge, spill materialization, and assembly emission. Argument placement, `va_start` and every object-format spelling come from the ABI descriptor.
- **`abi/`**: one `arm64_abi_desc_t` per ABI (`arm64_get_abi_desc()`), holding:
  - argument passing: register counts, the indirect result (x8) and indirect call (x16) registers, aggregate classification and `assign_scalar` / `assign_aggregate`;
  - variadic support: the MachineIR form of `va_start`, its prologue register save and emission, and IR-level `va_arg` / `va_copy`;
  - object format: symbol prefix, module header, string section, function header/footer, symbol addressing and data alignment.

  `aapcs64.c` holds what both share: HFA / small composite / by-reference classification, consecutive register allocation and the stack placement of aggregates.
- **Linux vs Darwin** (checked against clang by `tests/arm64_abi_regression.c`):

  | | Linux (`linux.c`) | Darwin (`darwin.c`) |
  |---|---|---|
  | Named stack arguments | doubleword slots | packed at natural size and alignment |
  | 8/16-bit integer arguments | callee extends | caller extends to 32 bits |
  | 16-byte aligned composites | even x register pair | next x registers |
  | Unnamed arguments | as named | doubleword stack slots only |
  | `va_list` | AAPCS64 cursor over register save areas | pointer into the stack slots |
  | Symbols / sections | ELF, `:lo12:` | `_` prefix, `@PAGE` / `@PAGEOFF`, `__TEXT` |

- **`arm64_internal.h`**: backend state, ARM64 register constants and the ABI descriptor type
- **`arm64_helpers.c`**: target type sizing/alignment helpers and register name tables
- **`arm64.c`**: `arm64_init()`, `arm64_cleanup()`, module/function codegen entry points, and `va_arg` / `va_copy` routed to the context's ABI

ARM64 deliberately has no target `prepare_ir` hook or dormant target peephole
directory. Source-IR transformations run through the generic pass manager;
target-specific work starts at verified MachineIR lowering so observable memory
operations cannot be erased by an untracked backend rewrite.

The x86 and x86-64 backends are no longer direct source-IR emitters: they were
rewritten onto the same MachineIR pipeline as ARM64 and PowerPC. New backend work
should follow ARM64's MachineIR structure.

### Supported Backends

| Backend | Arch enum | Pipeline | ABIs / conventions | Output | Maturity |
|---------|-----------|----------|--------------------|--------|----------|
| x86-64 | `ANVIL_ARCH_X86_64` | MachineIR | SysV (Linux/BSD), Darwin, Win64 | GAS/AT&T | Linux/SysV and Win64 execution-tested; Darwin emit-validated |
| x86 (32-bit) | `ANVIL_ARCH_X86` | MachineIR | cdecl, stdcall, fastcall; ELF/Mach-O/COFF decoration | GAS/AT&T | ELF execution-tested (`as --32` / `gcc -m32`); Mach-O/COFF emit-validated only |
| ARM64 | `ANVIL_ARCH_ARM64` | MachineIR (reference) | AAPCS64 (Linux SysV), Darwin | GAS | Linux execution-tested (qemu-user); Darwin ABI checked against clang and assembled, not executed |
| PPC32 | `ANVIL_ARCH_PPC32` | MachineIR | System V (ELF) | GAS | Lowering/emission regressions; native ABI coverage incomplete |
| PPC64 (ELFv1) | `ANVIL_ARCH_PPC64` | MachineIR | ELFv1 (BE) | GAS | Lowering/emission regressions; native ABI coverage incomplete |
| PPC64LE (ELFv2) | `ANVIL_ARCH_PPC64LE` | MachineIR | ELFv2 (LE) | GAS | Lowering/emission regressions; native ABI coverage incomplete |
| S/370 | `ANVIL_ARCH_S370` | MachineIR | ANVIL MVS-oriented arena | HLASM | Lowering/text regressions; native linkage incomplete |
| S/370-XA | `ANVIL_ARCH_S370_XA` | MachineIR | ANVIL MVS-oriented arena | HLASM | Lowering/text regressions; native linkage incomplete |
| S/390 | `ANVIL_ARCH_S390` | MachineIR | ANVIL MVS-oriented arena | HLASM | Lowering/text regressions; native linkage incomplete |
| z/Architecture | `ANVIL_ARCH_ZARCH` | MachineIR | ANVIL MVS-oriented arena | HLASM | Lowering/text regressions; native linkage incomplete |

> **Output syntax:** All non-mainframe backends currently emit **GAS / AT&T
> syntax only**. Intel/NASM/MASM output is not exposed until an emitter
> implements it completely. Mainframe backends emit IBM HLASM.

All five MachineIR emitter families implement `i1` as a normalized 0/1 value
with one-byte memory storage. Loads and stores mask the low bit, casts (including
`FPTOUI` to `i1`) normalize explicitly, and comparisons, branches, selects,
PHIs, parameters, and returns use the GPR8 boolean contract. `FCMP` carries one
of the 16 explicit ordered/unordered predicates through MachineIR. The native
x86-64 conformance gate executes every predicate over ordered values, NaNs,
infinities, and signed zero; ARM64 and PowerPC comparison sequences are
cross-assembled, while S/370 HFP and z/Architecture BFP mnemonic/condition-code
invariants are checked separately.

```
┌─────────────────────────────────────────────────────────────────┐
│                          ANVIL IR                                │
│  (Modules, Functions, Blocks, Instructions, Values, Types)      │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Backend Interface                           │
│                   (anvil_backend_ops_t)                         │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│ x86/x86-64 BE │    │ S/370 Backend │    │ Your Backend  │
│ ARM64 / PPC   │    │  (mainframe)  │    │               │
└───────────────┘    └───────────────┘    └───────────────┘
        │                     │                     │
        ▼ (MachineIR)         ▼ (MachineIR)         ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│  GAS (AT&T)   │    │    HLASM      │    │  Your ASM     │
│  Assembly     │    │   Assembly    │    │   Format      │
└───────────────┘    └───────────────┘    └───────────────┘
```

### MachineIR Pipeline (shared)

Every backend listed above lowers source IR into target MachineIR and then runs
the shared pipeline. The per-target driver (e.g. `anvil_x86_64_regalloc_mir`)
calls into the generic passes in this order (x86-64, i386, ARM64 and PowerPC
run all of them; SystemZ skips the optional optimizations):

```
verify-legal  ->  duplicate_branch_targets  ->  local_cse  ->
fold_address_arithmetic  ->  call clobbers  ->  coalesce_copies  ->
verify-legal  ->  printer scratch  ->  regalloc + materialize_spills  ->  verify-legal
```

- **Legality verification** (`anvil_<arch>_verify_mir_legal`): per-target rules,
  run before coalescing, after coalescing, and once more after spill insertion.
- **Branch-target duplication** (`anvil_mir_duplicate_branch_targets`): a jump
  to a small test block (pure instructions and a conditional branch) becomes a
  copy of it, keeping one incoming edge, so top-tested loops branch back from
  their latch.
- **Local value numbering** (`anvil_mir_local_cse`): a repeated pure integer
  computation in a block becomes a copy of the first.
- **Address folding** (`anvil_mir_fold_address_arithmetic`): `ADD(base,
  index * 1|2|4|8)` feeding loads/stores becomes an indexed access
  (`index_scale` on LOAD/STORE) when the target's hook accepts the form:
  x86 `disp(base,index,scale)` (both widths, `x86_address.h`), ARM64
  `[base, index, lsl #log2(size)]`, PowerPC X-form. Targets without the hook never see indexed accesses.
- **Call clobbers**: every CALL (and every instruction the printer expands into
  a helper call) carries the ABI's caller-saved set as a per-instruction
  clobber mask; the allocator keeps values that cross it out of those
  registers. The sets live in ABI data: `call_gpr_clobbers` /
  `call_fpr_clobbers` in the x86-64 ABI and i386 convention descriptors and
  the PowerPC ABI ops, the AAPCS64 register convention
  (`arm64_aapcs64_registers`).
- **Printer scratch** (`anvil_mir_set_instr_scratch`): registers the printer
  overwrites while printing an instruction, before it has read its operands
  (x86-64: CL for a variable shift or rotate count, RDX:RAX for division and
  the high half of a product, R11/RAX for conversions, floating constants and
  dynamic allocation; `x64_annotate_printer_scratch`, next to the emitters;
  i386: CL for a variable shift, `x86_annotate_printer_registers`, which also
  marks EDX:EAX of division and high products and EAX of dynamic allocation
  as clobbers, since those read their operands first). Unlike a clobber, a
  scratch register holds none of the instruction's operands or its result
  either; elsewhere it is an ordinary pool register.
- **Copy coalescing** (`anvil_mir_coalesce_copies`): removes copies between
  single-definition values within a block, and folds PHI moves (`t = op;
  x = COPY t`) by letting `op` write `x` when nothing in between touches it.
- **Register allocation** (`anvil_regalloc_linear_scan_classes`,
  `src/machine/regalloc.c`): linear scan over typed virtual registers with
  per-class pools (`anvil_regalloc_class_config_t`). Pools list caller-saved
  registers first, so values that do not cross a call need no save. Copy
  hints place a value copied to or from a fixed register (arguments, results)
  in that register, and the two sides of a plain copy in one register. A
  fixed-register interval that displaces an allocated value moves it to a free
  register before spilling it; spilled single-definition values are split at
  clobbers and allocation reruns. `narrow_bits`/`narrow_regs` keep small
  values in the registers that have a form of their width (i386: bytes and
  booleans in EAX-EDX); reloads follow the same rule.
- **Spill materialization** (`anvil_mir_materialize_spills`): inserts
  `SPILL_LOAD`/`SPILL_STORE` using the per-target scratch pool, checked per
  bundle (RET_VALUE_PART…RET, CALL…CALL_RESULT) against the scratch registers
  free there (and not printer scratch of the bundle) before anything is
  rewritten; in a bundle of several instructions the members' results are
  off limits too (they are written together, before the spill stores). A
  spilled result reuses its instruction's first reload register; a COPY
  between a register and a spill slot becomes the store or load itself. With
  `printer_declares_writes` on a scratch config (x86), a register keeps
  serving the value it reloaded or stored for later uses in its block until
  something declared writes it.
  `anvil_regalloc_allocate` runs both: with the spill scratch registers in the
  pool first, reloading where nothing occupies them; where a bundle's reloads
  do not fit, the values occupying scratch registers there are spilled
  (`anvil_mir_spill_for_scratch`, a few rounds); failing that, allocation
  reruns with the first 1, 2, ... scratch registers of each class out of the
  pool.
- **Spill choice**: when no register is free, a value recomputable from
  nothing (constant, symbol or frame address) is given up first: it costs a
  recomputation per use, no memory traffic.

### Backend Interface

```c
typedef struct {
    const char *name;              // Human-readable name
    anvil_arch_t arch;             // Architecture enum value
    
    // Initialize backend state
    anvil_error_t (*init)(anvil_backend_t *be, anvil_ctx_t *ctx);
    
    // Free backend resources
    void (*cleanup)(anvil_backend_t *be);
    
    // Clear cached IR pointers (called before module destruction)
    void (*reset)(anvil_backend_t *be);
    
    // Prepare/lower IR before code generation (optional)
    // Called automatically by anvil_module_codegen() before codegen_module()
    anvil_error_t (*prepare_ir)(anvil_backend_t *be, anvil_module_t *mod);
    
    // Generate code for entire module
    anvil_error_t (*codegen_module)(anvil_backend_t *be, anvil_module_t *mod,
                                     char **output, size_t *len);
    
    // Generate code for single function (optional)
    anvil_error_t (*codegen_func)(anvil_backend_t *be, anvil_func_t *func,
                                   char **output, size_t *len);
    
    // Return architecture information
    const anvil_arch_info_t *(*get_arch_info)(anvil_backend_t *be);
} anvil_backend_ops_t;
```

### IR Preparation Phase

The `prepare_ir` callback is called automatically before `codegen_module` to allow architecture-specific IR preparation:

```c
static anvil_error_t myarch_prepare_ir(anvil_backend_t *be, anvil_module_t *mod)
{
    myarch_backend_t *priv = be->priv;
    
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (!func->is_declaration) {
            // Analyze function (detect leaf functions, calculate stack layout)
            myarch_analyze_function(priv, func);
            
            // Lower unsupported operations
            // myarch_lower_unsupported_ops(priv, func);
            
            // Perform target-specific peephole optimizations
            // myarch_peephole_optimize(priv, func);
        }
    }
    
    return ANVIL_OK;
}
```

**Use cases for `prepare_ir`:**
- **Function analysis**: Detect leaf functions, calculate stack frame layout
- **IR lowering**: Convert unsupported operations to sequences of supported ones
- **Type legalization**: Split 64-bit operations on 32-bit targets
- **Peephole optimization**: Target-specific optimizations at IR level
- **Register pressure analysis**: Insert spill/reload hints

### Architecture-Specific Optimizations

Backends can implement their own optimization passes that run during `prepare_ir`. These are separate from the generic IR optimizations in `src/opt/` and can leverage architecture-specific knowledge.

**Example: ARM64 Optimization Structure**

```
src/backend/arm64/opt/
├── arm64_opt.h       # Interface declarations
├── arm64_opt.c       # Pass manager
├── arm64_peephole.c  # Peephole optimizations
├── arm64_dead_store.c # Dead store elimination
├── arm64_load_elim.c  # Redundant load elimination
├── arm64_branch.c     # Branch optimization
└── arm64_immediate.c  # Immediate optimization
```

**Pass Manager Example:**
```c
void arm64_opt_function(arm64_backend_t *be, anvil_func_t *func)
{
    /* Run optimization passes in order */
    arm64_opt_peephole(be, func);     // Local improvements
    arm64_opt_dead_store(be, func);   // Remove dead stores
    arm64_opt_load_elim(be, func);    // Eliminate redundant loads
    arm64_opt_branch(be, func);       // Optimize branches
    arm64_opt_immediate(be, func);    // Use immediate forms
}
```

**ARM64-Specific Optimizations:**
- **Peephole**: Redundant store elimination, load-store same address removal
- **Branch**: Combine `cmp`+`cset`+`cbnz` into `cmp`+`b.cond`, use `cbz`/`cbnz`/`tbz`/`tbnz`
- **Immediate**: Use `add`/`sub` immediate forms, efficient constant loading with `mov`/`movn`/`movz`

### Architecture Information

```c
typedef struct {
    anvil_arch_t arch;             // Architecture enum
    const char *name;              // Architecture name
    int ptr_size;                  // Pointer size in bytes
    int addr_bits;                 // Address bits (24, 31, 32, 64)
    int word_size;                 // Native word size
    int num_gpr;                   // General purpose registers
    int num_fpr;                   // Floating point registers
    anvil_endian_t endian;         // ANVIL_ENDIAN_LITTLE or ANVIL_ENDIAN_BIG
    anvil_stack_dir_t stack_dir;   // ANVIL_STACK_DOWN or ANVIL_STACK_UP
    bool has_condition_codes;      // Has condition code register
    bool has_delay_slots;          // Has branch delay slots
} anvil_arch_info_t;
```

## Implementing a New Backend

### Step 1: Create Backend File

Create `src/backend/<arch>/<arch>.c`:

```c
#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Register names for your architecture
static const char *myarch_reg_names[] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    // ...
};

// Register indices
#define MYARCH_R0   0
#define MYARCH_R1   1
// ...

// Backend private data
typedef struct {
    anvil_strbuf_t code;           // Output buffer
    anvil_strbuf_t data;           // Data section buffer
    int label_counter;             // For generating unique labels
    // Add your backend-specific state here
} myarch_backend_t;
```

### Step 2: Define Architecture Info

```c
static const anvil_arch_info_t myarch_arch_info = {
    .arch = ANVIL_ARCH_MYARCH,
    .name = "MyArch",
    .ptr_size = 4,                 // 4 bytes for 32-bit
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 16,
    .num_fpr = 8,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};
```

### Step 3: Implement Init and Cleanup

```c
static anvil_error_t myarch_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)ctx;  // May be unused
    
    myarch_backend_t *priv = calloc(1, sizeof(myarch_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void myarch_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    myarch_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv);
    be->priv = NULL;
}

static void myarch_reset(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    myarch_backend_t *priv = be->priv;
    
    // Clear cached pointers to anvil_value_t
    // This prevents dangling pointers when modules are destroyed
    priv->label_counter = 0;
    // Reset any stack_slots, strings, or other cached IR pointers
}

static const anvil_arch_info_t *myarch_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &myarch_arch_info;
}
```

### Step 4: Implement Code Emission Helpers

```c
// Emit file header
static void myarch_emit_header(myarch_backend_t *be, const char *module_name)
{
    anvil_strbuf_appendf(&be->code, "; Module: %s\n", module_name);
    anvil_strbuf_append(&be->code, "; Generated by ANVIL\n\n");
    anvil_strbuf_append(&be->code, ".text\n");
}

// Emit function prologue
static void myarch_emit_prologue(myarch_backend_t *be, anvil_func_t *func)
{
    anvil_strbuf_appendf(&be->code, ".global %s\n", func->name);
    anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
    
    // Save frame pointer, allocate stack, etc.
    anvil_strbuf_append(&be->code, "    push rbp\n");
    anvil_strbuf_append(&be->code, "    mov rbp, rsp\n");
    // ...
}

// Emit function epilogue
static void myarch_emit_epilogue(myarch_backend_t *be)
{
    anvil_strbuf_append(&be->code, "    mov rsp, rbp\n");
    anvil_strbuf_append(&be->code, "    pop rbp\n");
    anvil_strbuf_append(&be->code, "    ret\n");
}

// Load a value into a register
static void myarch_emit_load_value(myarch_backend_t *be, anvil_value_t *val, int reg)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            anvil_strbuf_appendf(&be->code, "    mov %s, %lld\n",
                myarch_reg_names[reg], (long long)val->data.i);
            break;
            
        case ANVIL_VAL_PARAM:
            // Load parameter based on calling convention
            // ...
            break;
            
        case ANVIL_VAL_INSTR:
            // Result of previous instruction (may already be in a register)
            // ...
            break;
            
        // Handle other value kinds...
    }
}
```

### Step 5: Implement Instruction Emission

```c
static void myarch_emit_instr(myarch_backend_t *be, anvil_instr_t *instr)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ADD:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);
            anvil_strbuf_append(&be->code, "    add R0, R1\n");
            // Result is in R0
            break;
            
        case ANVIL_OP_SUB:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);
            anvil_strbuf_append(&be->code, "    sub R0, R1\n");
            break;
            
        case ANVIL_OP_MUL:
            // Handle multiplication
            break;
            
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
            // Handle division
            break;
            
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_NOT:
            // Handle bitwise operations
            break;
            
        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
            // Handle shifts
            break;
            
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
            // Handle comparisons
            break;
            
        case ANVIL_OP_LOAD:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            anvil_strbuf_append(&be->code, "    load R0, [R0]\n");
            break;
            
        case ANVIL_OP_STORE:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);  // value
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);  // address
            anvil_strbuf_append(&be->code, "    store [R1], R0\n");
            break;
            
        case ANVIL_OP_BR:
            anvil_strbuf_appendf(&be->code, "    jmp %s\n",
                instr->true_block->name);
            break;
            
        case ANVIL_OP_BR_COND:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            anvil_strbuf_append(&be->code, "    test R0, R0\n");
            anvil_strbuf_appendf(&be->code, "    jnz %s\n",
                instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "    jmp %s\n",
                instr->false_block->name);
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
                // Move to return register if needed
            }
            myarch_emit_epilogue(be);
            break;
            
        case ANVIL_OP_CALL:
            // Handle function calls
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "    ; Unimplemented: op %d\n",
                instr->op);
            break;
    }
}
```

### Step 6: Implement Block and Function Emission

```c
static void myarch_emit_block(myarch_backend_t *be, anvil_block_t *block)
{
    if (!block) return;
    
    // Emit block label
    anvil_strbuf_appendf(&be->code, "%s:\n", block->name);
    
    // Emit all instructions
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        myarch_emit_instr(be, instr);
    }
}

static void myarch_emit_func(myarch_backend_t *be, anvil_func_t *func)
{
    if (!func) return;
    
    myarch_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        myarch_emit_block(be, block);
    }
    
    anvil_strbuf_append(&be->code, "\n");
}
```

### Step 7: Implement Module Code Generation

```c
static anvil_error_t myarch_codegen_module(anvil_backend_t *be,
                                            anvil_module_t *mod,
                                            char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    myarch_backend_t *priv = be->priv;
    
    // Reset buffers
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    
    // Emit header
    myarch_emit_header(priv, mod->name);
    
    // Emit all functions
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        myarch_emit_func(priv, func);
    }
    
    // Emit data section
    if (mod->num_globals > 0) {
        anvil_strbuf_append(&priv->code, "\n.data\n");
        for (anvil_global_t *g = mod->globals; g; g = g->next) {
            anvil_strbuf_appendf(&priv->code, "%s: .long 0\n",
                g->value->name);
        }
    }
    
    // Return output
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

static anvil_error_t myarch_codegen_func(anvil_backend_t *be,
                                          anvil_func_t *func,
                                          char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    myarch_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    myarch_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}
```

### Step 8: Export Backend Operations

```c
const anvil_backend_ops_t anvil_backend_myarch = {
    .name = "MyArch",
    .arch = ANVIL_ARCH_MYARCH,
    .init = myarch_init,
    .cleanup = myarch_cleanup,
    .reset = myarch_reset,
    .codegen_module = myarch_codegen_module,
    .codegen_func = myarch_codegen_func,
    .get_arch_info = myarch_get_arch_info
};
```

### Step 9: Register Backend

Add to `include/anvil/anvil.h`:

```c
typedef enum {
    ANVIL_ARCH_UNKNOWN = 0,
    ANVIL_ARCH_X86,
    ANVIL_ARCH_X86_64,
    ANVIL_ARCH_S370,
    ANVIL_ARCH_S370_XA,
    ANVIL_ARCH_S390,
    ANVIL_ARCH_ZARCH,
    ANVIL_ARCH_PPC32,
    ANVIL_ARCH_PPC64,
    ANVIL_ARCH_PPC64LE,
    ANVIL_ARCH_ARM64,
    ANVIL_ARCH_MYARCH,      // Add your architecture (before ANVIL_ARCH_COUNT)
    ANVIL_ARCH_COUNT
} anvil_arch_t;
```

Register it in `src/core/backend.c` (backends are registered at startup via
`anvil_register_backend()` in `anvil_init_backends()`):

```c
extern const anvil_backend_ops_t anvil_backend_myarch;

/* In anvil_init_backends(): */
anvil_register_backend(&anvil_backend_x86);
anvil_register_backend(&anvil_backend_x86_64);
anvil_register_backend(&anvil_backend_s370);
anvil_register_backend(&anvil_backend_s370_xa);
anvil_register_backend(&anvil_backend_s390);
anvil_register_backend(&anvil_backend_zarch);
anvil_register_backend(&anvil_backend_ppc32);
anvil_register_backend(&anvil_backend_ppc64);
anvil_register_backend(&anvil_backend_ppc64le);
anvil_register_backend(&anvil_backend_arm64);
anvil_register_backend(&anvil_backend_myarch);  // Add your backend
```

Add every component source to `BACKEND_SRCS` in the `Makefile`. Use the
explicit family source lists as examples, and give source files unique basenames
where they become archive members. Compiler-generated dependency files track
internal headers. Library creation replaces the archive so removed source files
cannot survive as stale object members.

## Calling Conventions

### Handling Parameters

Different architectures pass parameters differently:

The x86 and x86-64 backends select a definition's convention from the canonical
`func->type->data.func.cc`; CALL lowering consumes the callee-derived
`instr->call_cc`. No backend may silently map an incompatible convention.
Platform decoration is selected from `ctx->abi`. See the ABI/CC descriptor tables in
`src/backend/x86/abi/x86_64_abi.c` (`anvil_x64_abi_desc_t x64_abi_descs[]`) and
`src/backend/x86/abi/x86_32_abi.c` (`anvil_x86_cc_desc_t x86_cc_descs[]` /
`anvil_x86_plat_desc_t x86_plat_descs[]`).

**x86 (32-bit) — cdecl / stdcall / fastcall:**
- **cdecl**: all integer args on stack, caller cleans up (`ret`).
- **stdcall**: all integer args on stack, callee cleans up (`ret $N`).
- **fastcall**: first two integer args in ECX, EDX (`x86_fastcall_int_args`),
  remainder on stack, callee cleans up.
- Integer return: EAX (high half of a 64-bit pair: EDX).
- Platform decoration: ELF (no prefix), Mach-O (`_` prefix), Win32 COFF
  (`_name@N` for stdcall, `@name@N` for fastcall, applied at call sites).

**x86-64 — SysV / Darwin / Win64 (selected via descriptor table):**
- **SysV / Darwin** integer args: RDI, RSI, RDX, RCX, R8, R9; float args:
  XMM0-XMM7; integer return RAX (high half RDX), float return XMM0. Darwin adds
  a `_` symbol prefix.
- **Win64** integer args: RCX, RDX, R8, R9; float args: XMM0-XMM3; 32-byte shadow
  space; positional arg slots (GPR/FPR share positions); integer return RAX,
  float return XMM0.

**ARM64 (AAPCS64):**
- First 8 integer args: X0-X7
- First 8 float args: D0-D7
- Return value: X0 (integer), D0 (float)
- Frame pointer: X29
- Link register: X30
- Stack pointer: SP (16-byte aligned)
- Scratch register: X16 (used for large offsets)

**ARM64 Backend Implementation Details:**

The ARM64 backend includes several important features for robust code generation:

1. **MachineIR Value Preservation**: Source IR values are lowered to typed MachineIR virtual registers. The shared allocator assigns physical registers and inserts spill loads/stores only when required.

2. **ABI Constraints**: Function parameters and returns are represented as fixed MachineIR registers (`x0`-`x7`/`d0`-`d7` and `x0`/`d0`), then copied into allocatable local vregs.

3. **Large Frame/Spill Offsets**: Uses scratch registers when an address is outside ARM64 immediate ranges:
   ```asm
   sub x16, x29, #512
   ldr x0, [x16]
   ```

4. **Very Large Stack Frames (>4095 bytes)**: Materializes the frame size before adjusting `sp`:
   ```asm
   ldr x16, =5920          ; Load large frame size
   sub sp, sp, x16         ; Allocate stack space
   ```

5. **PHI and Switch Lowering**: PHI nodes are lowered into edge copies, including parallel-copy cycle handling. `switch` terminators lower to compare/branch chains with PHI-aware edge blocks.

6. **Type-Aware Load/Store**: Correct instruction selection based on type class and size (`ldr`, `ldrsb`, `ldrsh`, `ldrsw`, `strb`, `strh`, floating-point `ldr`/`str`).

**PowerPC 32-bit (System V):**
- First 8 integer args: R3-R10
- First 8 float args: F1-F8
- Return value: R3 (integer), F1 (float)
- Frame pointer: R31
- Link register: LR (saved via mflr/mtlr)
- Stack pointer: R1 (16-byte aligned)

**PowerPC 64-bit BE (ELFv1):**
- First 8 integer args: R3-R10
- First 8 float args: F1-F8
- Return value: R3 (integer), F1 (float)
- TOC pointer: R2 (must be saved/restored)
- Frame pointer: R31
- Link register: LR
- Stack pointer: R1
- Minimum frame: 112 bytes
- Function descriptors in `.opd` section

**PowerPC 64-bit LE (ELFv2):**
- First 8 integer args: R3-R10
- First 8 float args: F1-F8
- Return value: R3 (integer), F1 (float)
- TOC pointer: R2
- Frame pointer: R31
- Link register: LR
- Stack pointer: R1
- Minimum frame: 32 bytes
- No function descriptors (uses `.localentry`)

**SystemZ family linkage:**

The existing HLASM implementation uses R1 address lists and a caller-owned
arena with chained save areas. It sets and clears the terminal parameter bit;
AMODE and RMODE come from the selected target descriptor. R11 is the stable
frame base, while R13 points to the outgoing save area.

This is an ANVIL convention with known native ABI gaps. It does not establish
GCCMVS interoperability or implement the IBM F4SA layout. Linkage entry/exit
code belongs to `systemz/abi/`; instruction, data and symbol printing belongs
to `systemz/asm/`. See [the implementation and manual comparison](MAINFRAME.md)
before extending or integrating this target family.

### Handling Return Values

**x86/x86-64:**
- Integer: EAX/RAX
- Float: XMM0 or ST(0)

**ARM64:**
- Integer: X0
- Float: D0 (double) or S0 (float)

**SystemZ arena convention:**
- Integer: R15 (31-bit targets: a 64-bit integer in R15 high, R0 low)
- Float: F0

## Register Allocation

All MachineIR backends share one allocator: a classic **linear scan** over typed
virtual registers, `anvil_regalloc_linear_scan_classes()` in
`src/machine/regalloc.c`. Each backend supplies per-register-class allocatable
pools (`anvil_regalloc_class_config_t`) plus separate scratch pools used by spill
materialization.

Key design points to keep in mind when wiring up a new backend:

- **Clobbers are per instruction.** A call (or any instruction that destroys
  registers) carries a clobber mask per class
  (`anvil_mir_set_instr_clobbers`); registers a call preserves only in part
  (AAPCS64 v8-v15, PowerPC f14-f31 low halves) are declared in the class
  config. A value may keep a clobbered register only where it is not live
  across the clobber: the allocator records, from per-block liveness, the
  values live across each clobbering instruction, so a value redefined after a
  call (a hole in its interval) still uses a caller-saved register. Calls in
  cold blocks are split around first (`anvil_mir_split_around_cold_calls`): a
  value live across them is copied to a fresh vreg at the block's start and
  back after the last call, so its store and reload stay on the rare path.
- **Two-address classes.** A class config with `two_address` (a backend whose
  arithmetic overwrites its first operand: x86 GPR and SSE) makes a result prefer the
  register of a first operand that dies there and avoid the second operand's,
  which saves the printer's copy or temporary.
- **ABI registers are modeled as fixed vregs.** Incoming arguments and return
  values are pinned to the physical ABI registers via `anvil_mir_set_fixed_reg`,
  then copied into ordinary allocatable vregs (`ANVIL_MIR_OP_COPY`) before
  allocation. The allocator honors fixed assignments.
- **Printer temporaries are per instruction.** Registers a printer needs while
  expanding one instruction are declared on it (`anvil_mir_set_instr_scratch`)
  instead of being withheld from the pool: the allocator keeps that
  instruction's operands, result and the values live across it out of them.
- **Spills are explicit.** `anvil_mir_materialize_spills()` rewrites the
  instruction stream with `SPILL_LOAD`/`SPILL_STORE` using the scratch pool; the
  target emitter turns those into real load/store instructions.
  `anvil_regalloc_allocate()` lets the spill scratch registers be part of the
  pool, reserving them only for a function whose reloads cannot otherwise be
  placed.

**Pools (x86-64 from its ABI descriptors; see each backend's tables):**

| Backend | Allocatable GPR | Allocatable FPR | Spill scratch (in the pool) |
|---------|-----------------|-----------------|-----------------------------|
| x86-64 SysV | r8, r9, rsi, rdi, r10, r11, rdx, rcx, rax, rbx, r12-r15 | xmm0-xmm13 | r11, r10, rax; xmm8-xmm10 |
| x86-64 Win64 | r8-r11, rdx, rcx, rax, rbx, rdi, rsi, r12-r15 | xmm0-xmm3, xmm6-xmm15 | r11, r10, rax; xmm13-xmm15 |
| x86 (32-bit) | ECX, EDX, EAX, EBX, ESI, EDI (bytes: EAX-EDX) | xmm0-xmm3, xmm5-xmm7 (x87: home slots) | EAX, ECX, EDX; xmm5-xmm7 |

> The x86-64 printer keeps two XMM temporaries outside the pool
> (`sse_temps`: xmm14/xmm15 SysV, xmm4/xmm5 Win64), the i386 SSE2 unit one
> (xmm4). The emitter saves and
> restores every callee-saved register the function actually uses, spill
> reloads included.

## Handling Special Cases

### Division

Many architectures have special requirements for division:

**x86:**
```asm
; Signed division: EDX:EAX / operand
mov eax, dividend
cdq                 ; Sign-extend EAX into EDX
idiv divisor        ; Result in EAX, remainder in EDX
```

**S/370:**
```asm
; Division uses even-odd register pair
L     R3,dividend
SRDA  R2,32         ; Sign-extend R3 into R2:R3
D     R2,divisor    ; Quotient in R3, remainder in R2
```

### Comparisons

**x86:**
```asm
cmp eax, ebx
sete al             ; Set AL to 1 if equal
movzx eax, al       ; Zero-extend to full register
```

**ARM64:**
```asm
cmp x9, x10         ; Compare registers
cset x0, eq         ; Set X0 to 1 if equal, 0 otherwise
```

**S/370:**
```asm
CR    R2,R3         ; Compare registers
LA    R15,1         ; Assume true
BE    *+6           ; Skip if equal
SR    R15,R15       ; Set false
```

### Function Calls

**x86-64:**
```asm
; Call printf("Hello %d", 42)
lea rdi, [rel fmt]  ; First arg: format string
mov esi, 42         ; Second arg: integer
xor eax, eax        ; No vector args
call printf
```

**ARM64 (Linux):**
```asm
// Call printf("Hello %d", 42) - non-variadic style
adrp x0, fmt            // Load page address of fmt
add x0, x0, :lo12:fmt   // Add page offset
mov w1, #42             // Second arg: integer
bl printf               // Branch with link
```

**ARM64 (Darwin/macOS) - Variadic Functions:**
```asm
// Call printf("Hello %d", 42) - variadic args on stack
sub sp, sp, #16         // Allocate stack for variadic args
adrp x9, fmt@PAGE       // Load page address of fmt
add x9, x9, fmt@PAGEOFF // Add page offset
mov x0, x9              // First arg (fixed): format string
mov x9, #42             // Load variadic arg
str x9, [sp, #0]        // Store variadic arg on stack
bl _printf              // Branch with link (underscore prefix)
add sp, sp, #16         // Restore stack
```

**Note:** On Darwin/macOS ARM64, variadic function arguments (those after the fixed parameters) must be passed on the stack, not in registers. This is a key difference from Linux AAPCS64 where all arguments can go in registers.

**S/370:**
```asm
; Build parameter list in dynamic area
ST    R0,72(,R13)   ; Store first param address
ST    R1,76(,R13)   ; Store second param address
OI    76(R13),X'80' ; Mark last param
LA    R1,72(,R13)   ; R1 -> param list
L     R15,=V(FUNC)  ; Load function address
BALR  R14,R15       ; Call
```

## Testing Your Backend

### Unit Tests

Create test cases for each instruction type:

```c
void test_add(void) {
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_MYARCH);
    
    anvil_module_t *mod = anvil_module_create(ctx, "test");
    // Create add function...
    
    char *output = NULL;
    anvil_module_codegen(mod, &output, NULL);
    
    // Verify output contains expected instructions
    assert(strstr(output, "add") != NULL);
    
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}
```

### Integration Tests

Test complete programs:

```c
void test_factorial(void) {
    // Generate factorial function
    // Assemble output
    // Link and run
    // Verify result
}
```

## ARM64 Backend Implementation Notes

The ARM64 backend has several important implementation details:

### MachineIR Value Preservation

ARM64 is the reference MachineIR backend. It does not use the old
stack-slot-for-every-SSA-value strategy. Instead, lowering creates typed virtual
registers, marks ABI registers as fixed where needed, runs shared copy
coalescing and linear-scan allocation, and materializes spills only for values
that cannot stay in physical registers.

### Large Stack Frame Support

ARM64 addressing has immediate range limits. For larger local, spill, incoming
argument, or memory offsets, the emitter materializes an address through a
scratch register:

```c
ldr x16, =8192
sub x16, x29, x16
ldr x19, [x16]
```

### External Function Calls

Direct calls carry a symbol in MachineIR and emit `bl`. Indirect calls carry a
target register; ARM64 legalizes that target to fixed `x16` and emits `blr x16`:

```asm
bl puts
blr x16
```

### Type-Aware Load/Store

Use correct instruction variants based on type size:

| Type | Load | Store |
|------|------|-------|
| i8/u8 | `ldrb w0` | `strb w9` |
| i16/u16 | `ldrh w0` | `strh w9` |
| i32/u32/f32 | `ldr w0` | `str w9` |
| i64/u64/f64/ptr | `ldr x0` | `str x9` |

### Parameter Lowering

Incoming register arguments are modeled as fixed MachineIR vregs and copied into
normal local vregs before allocation. Additional arguments are represented with
`ANVIL_MIR_OP_INCOMING_STACK_ARG`. Darwin variadic arguments after the fixed
parameter list are forced to outgoing stack slots during call lowering.

### Array Stack Allocation

When allocating arrays via `ANVIL_OP_ALLOCA`, the backend must calculate the correct size based on the pointee type:

```c
static int arm64_type_size(anvil_type_t *type)
{
    if (!type) return 8;
    
    switch (type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 1;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 2;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32:
            return 4;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64:
        case ANVIL_TYPE_PTR:
            return 8;
        case ANVIL_TYPE_ARRAY:
            return type->data.array.count * arm64_type_size(type->data.array.elem);
        case ANVIL_TYPE_STRUCT:
            /* Sum of field sizes */
            int size = 0;
            for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                size += arm64_type_size(type->data.struc.fields[i]);
            }
            return size > 0 ? size : 8;
        default:
            return 8;
    }
}

/* In arm64_emit_func first pass: */
if (instr->op == ANVIL_OP_ALLOCA) {
    int size = 8;
    if (instr->result && instr->result->type && 
        instr->result->type->kind == ANVIL_TYPE_PTR &&
        instr->result->type->data.pointee) {
        size = arm64_type_size(instr->result->type->data.pointee);
    }
    arm64_add_stack_slot_sized(be, instr->result, size);
}
```

### Global Variable Access (macOS vs Linux)

The backend must use different relocation syntax for global variables:

**Linux (ELF):**
```asm
adrp x9, counter
ldr w0, [x9, :lo12:counter]
str w9, [x10, :lo12:counter]
```

**macOS (Mach-O):**
```asm
adrp x9, _counter@PAGE
ldr w0, [x9, _counter@PAGEOFF]
str w9, [x10, _counter@PAGEOFF]
```

Implementation:
```c
if (arm64_is_darwin(be)) {
    emit("\tadrp x9, %s%s@PAGE\n", prefix, name);
    emit("\tldr w0, [x9, %s%s@PAGEOFF]\n", prefix, name);
} else {
    emit("\tadrp x9, %s\n", name);
    emit("\tldr w0, [x9, :lo12:%s]\n", name);
}
```

## x86 / x86-64 Backend Implementation Notes

Both x86 backends follow the ARM64 MachineIR contract
(`anvil_<arch>_lower_func_to_mir` -> `..._verify_mir_legal` ->
`..._regalloc_mir` -> `..._emit_mir_abi`). Implementation-specific details:

### Multi-ABI selection (x86-64)

The descriptor struct `anvil_x64_abi_desc_t` and table `x64_abi_descs[]` in
`abi/x86_64_abi.c` hold one entry per ABI (SysV, Darwin, Win64) with argument
register sequences, return registers, shadow-space size, symbol prefix, and
register pools. Function-type construction resolves the active ABI to
`ANVIL_CC_WIN64` or `ANVIL_CC_SYSV`; lowering consumes that canonical value.

### Multi-convention selection (x86 32-bit)

`x86_cc_descs[]` covers cdecl, stdcall, and fastcall; `x86_plat_descs[]` covers
ELF / Mach-O / Win32 COFF decoration. The emitter consumes the canonical
function-type CC for the
convention (register args, callee `ret $N` cleanup) and `ctx->abi` for platform
decoration. Stack cleanup bytes are computed by `x86_compute_ret_pop()`.

### 64-bit integers on x86 (32-bit)

`i64`/`u64` are legalized into **lo/hi register pairs** (little-endian) by the
shared `common/wide_mir` legalizer: arithmetic, shifts, compares, conversions,
PHIs and selects. Multiplication, division, variable shifts and floating
conversions call the libgcc helpers with cdecl; results come back in EDX:EAX
(`RET_VALUE_PART` / `CALL_RESULT` carry the high word).

### Byte registers on x86 (32-bit)

ESI and EDI have no 8-bit sub-register on 32-bit x86 (only EAX/ECX/EDX/EBX do,
per `x86_reg_has_byte()`). Byte stores and byte casts of a value living in
ESI/EDI are routed through a byte-addressable scratch register (`movl` into
EAX/ECX, then use `al`/`cl`). Byte/word loads use `movzbl`/`movsbl` writing a
full 32-bit destination and need no fixup.

### Known limitations (x86 / x86-64)

- **GAS / AT&T output only.** NASM/Intel output is not implemented or exposed
  as a selectable syntax.
- **`u64 -> floating point` is simplified.** `UITOFP` uses the signed
  `cvtsi2ss/cvtsi2sd` path, treating the unsigned source as signed (no full
  unsigned-64 fixup).
- **struct-by-value is limited.** Aggregates are handled by address (GEP /
  struct-GEP); there is no System V field-classification of structs into
  registers. Stack arguments use fixed 8-byte (x86-64) / 4-byte-rounded (x86)
  slots.
- **Win64 is execution-tested bidirectionally** with a MinGW C shim under Wine:
  C calls Anvil-generated code, Anvil calls a C variadic function, and the gate
  checks stack arguments, shadow space and nonvolatile XMM preservation. Darwin
  remains emit-validated on the Linux host. SysV/Linux x86-64 and ELF x86-32 are
  also execution-tested paths.

### Validation

x86-64 passes the `basic_runtime` execution test and the `mcc` exec suite, and
both backends have MachineIR lowering regression tests
(`tests/x86_64_mir_lowering_regression.c`, `tests/x86_mir_lowering_regression.c`).
x86-32 output is execution-validated by assembling with `as --32` and linking
with `gcc -m32`.

## Debugging Tips

1. **Print IR**: Add debug output to see what IR you're processing
2. **Check Operand Types**: Many bugs come from type mismatches
3. **Verify Offsets**: Stack and memory offsets are common error sources
4. **Test Incrementally**: Start with simple functions, add complexity gradually
5. **Compare with Reference**: Generate code with a known compiler and compare

## Performance Considerations

1. **Minimize Memory Operations**: Keep values in registers when possible
2. **Use Efficient Instructions**: Some operations have faster alternatives
3. **Align Data**: Misaligned access is slow on most architectures
4. **Branch Prediction**: Arrange code to help branch predictors
5. **Instruction Scheduling**: Order instructions to avoid pipeline stalls

## Platform-Specific Code Generation

### OS ABI Support

ANVIL supports multiple OS ABIs through the `anvil_ctx_set_abi()` function. This affects:

- **Symbol naming**: Darwin uses underscore prefix (`_main` vs `main`)
- **Assembly directives**: `.type` (ELF) vs `.p2align` (Mach-O)
- **Section names**: `.text` (ELF) vs `__TEXT,__text` (Mach-O)
- **Relocation syntax**: `:lo12:` (ELF) vs `@PAGE/@PAGEOFF` (Mach-O)

### ARM64 macOS (Apple Silicon) Example

```c
anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_ARM64);
anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN);  // Enable macOS mode

// ... build IR ...

// Generated code will have:
// - Underscore prefix on symbols (_main, _printf)
// - Mach-O section directives
// - @PAGE/@PAGEOFF relocations
```

**Linux ARM64:**
```asm
        .globl main
        .type main, %function
main:
        adrp x0, .LC0
        add x0, x0, :lo12:.LC0
        bl printf
        .size main, .-main
```

**macOS ARM64:**
```asm
        .globl _main
        .p2align 2
_main:
        adrp x0, L_.str@PAGE
        add x0, x0, L_.str@PAGEOFF
        bl _printf
```

### Implementing ABI Support in Backends

When implementing a backend that needs ABI-specific code:

```c
static void emit_function(my_backend_t *be, anvil_func_t *func)
{
    bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
    const char *prefix = is_darwin ? "_" : "";
    
    if (is_darwin) {
        // Mach-O directives
        emit(".globl %s%s\n", prefix, func->name);
        emit(".p2align 2\n");
    } else {
        // ELF directives
        emit(".globl %s\n", func->name);
        emit(".type %s, %%function\n", func->name);
    }
    
    emit("%s%s:\n", prefix, func->name);
    // ... emit function body ...
}
```
