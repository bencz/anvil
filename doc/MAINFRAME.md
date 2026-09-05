# SystemZ family backends

`src/backend/systemz/` contains the compiler family for the S/370 lineage.
The directory name groups related instruction encodings and lowering rules; it
does not make S/370 a 64-bit target. S/370, S/370-XA, S/390 and z/Architecture
remain separate targets with separate descriptors and capability limits.

The historical public `anvil_mainframe_*` API and `anvil_mainframe_mir.h` header
are retained for source compatibility. There is no target named `mainframe`.

## Source ownership

```text
src/backend/systemz/
    systemz_internal.h       Internal component contracts and emission state
    systemz_target.c         Descriptor lookup and common register pools
    systemz_lower.c          Source IR to MIR, including 32-bit wide-value lowering
    systemz_legal.c          MIR legality and register-allocation bridge
    systemz_codegen.c        Configuration validation and pipeline orchestration
    targets/
        s370.c              S/370 identity, capabilities and policy binding
        s370_xa.c           S/370-XA identity, capabilities and policy binding
        s390.c              S/390 identity, capabilities and policy binding
        zarch.c             z/Architecture identity, capabilities and policy binding
    abi/
        mvs_arena_31.c       ANVIL 32-bit save-area prologue and epilogue
        mvs_arena_64.c       ANVIL 64-bit save-area prologue and epilogue
    asm/
        hlasm.c             Allocated MIR instructions and HLASM function emission
        hlasm_data.c        Global storage and initializer encoding
        hlasm_names.c       Symbol spelling
        hlasm_dispatch.c    HLASM printer vtable and module terminator
```

`systemz_abi_ops_t` selects linkage entry/exit behavior.
`systemz_asm_ops_t` selects function/data printing and module termination.
Descriptors bind these policies explicitly. The current linkage routines print
HLASM and are only paired with that printer. Adding GAS requires implementing
that printer and the corresponding linkage emission; adding an OS ABI also
requires argument classification, register preservation and frame management.
Changing a descriptor string does not implement either capability.

Instruction-width decisions use descriptor capabilities. Architecture and host
selection do not use conditional compilation in these components. No component
includes another component's `.c` file.

## Implemented target profiles

These are ANVIL's current profiles, not a claim that every historical processor
in each family implements all of the profile's facilities.

| Target | Pointer storage | Address bits | GPR width | FPR profile | Default FP | AMODE / RMODE |
|---|---:|---:|---:|---|---|---|
| S/370 | 4 bytes | 24 | 32 | F0, F2, F4, F6 | HFP | 24 / 24 |
| S/370-XA | 4 bytes | 31 | 32 | F0, F2, F4, F6 | HFP | 31 / ANY |
| S/390 | 4 bytes | 31 | 32 | 16 registers | HFP | 31 / ANY |
| z/Architecture | 8 bytes | 64 | 64 | 16 registers | HFP / IEEE | 64 / ANY |

All four have real lowering, legality checking, allocation, spilling and text
emission. Their validation is materially weaker than the execution coverage for
x86-64 and AArch64. Atomics, SIMD and native variadic ABI support are not supplied
by this family. CPU model/facility gating still needs a systematic audit.

## Architecture, linkage and assembler are separate contracts

The ISA does not prescribe an upward-growing C stack, R1 parameter lists or
HLASM syntax. Those choices belong to an execution environment and its tools.
The `ANVIL_STACK_UP` metadata describes ANVIL's current arena convention.

IBM defines AMODE as addressing mode and RMODE as residence mode. In particular,
`RMODE ANY` means 31-bit residence, even alongside `AMODE 64`; it does not mean
unrestricted 64-bit residence. See the [HLASM AMODE/RMODE reference](https://www.ibm.com/docs/en/hla-and-tf/1.6.0?topic=support-addressing-mode-amode-residence-mode-rmode).

HLASM itself can produce ELF32 and ELF64 through ASMAXT2E. Therefore HLASM does
not imply the MVS ABI, and GAS does not by itself implement the Linux s390x ABI.
ANVIL currently supplies only HLASM with its MVS-oriented arena convention.
Linux s390x SysV, z/OS Language Environment and other linkage environments need
separate implementations. See [IBM APAR PH15557](https://www.ibm.com/support/pages/apar/PH15557).

Function and module codegen reject unsupported ABI/syntax combinations before
producing output, including empty modules and declarations. Public low-level
`anvil_mainframe_emit_mir` uses the selected descriptor's default printer.

## Current linkage and known incompatibilities

The 32-bit entry sequence saves GPRs in a 72-byte caller-provided area. R11 holds
the stable frame base; R13 advances through storage assumed to be owned by the
caller. R1 points to an address list. The emitter marks and strips a terminal bit
on parameter addresses, including its private 64-bit list representation.

There is no storage acquisition, arena capacity check or stack-growth runtime.
The generated prologue assumes more storage than the minimum caller-provided
save area. It must not be advertised as general MVS, Metal C, LE or GCCMVS
interoperability without a matching runtime and executable ABI tests.

The previous documentation incorrectly called the current 144-byte layout
F4SA. The layouts differ:

| Field | ANVIL arena-64 offset | IBM F4SA offset |
|---|---:|---:|
| Signature | Not emitted | 4 (`F4SA`) |
| Saved R14 | 24 | 8 |
| Saved R15 | 32 | 16 |
| Saved R0 through R12 | 40 through 136 | 24 through 120 |
| Back chain | 8 | 128 |
| Forward chain | 16 | 136 |

IBM specifies saving the registers into the caller-provided area and creating
and chaining any additional area required by the callee. A correct F4SA change
must update prologue, epilogue, dynamic allocation and the runtime contract
together; renaming the existing layout would be incorrect. See [IBM: If starting in AMODE 64](https://www.ibm.com/docs/en/zos/2.5.0?topic=area-if-starting-in-amode-64).

Further open items found during source review:

- FPR allocation currently permits registers without a complete call-clobber
  and preservation policy. z/OS normally treats FPR8-FPR15 as nonvolatile;
  language options can affect the contract. See [IBM AFP conventions](https://www.ibm.com/docs/en/cobol-zos/6.5.0?topic=ecco-afp).
- R0 occurs as a base register in emitted memory operands. Base-field zero has
  special ISA semantics; these sequences require instruction-level correction
  and execution tests, not only assembly substring checks. See [IBM's register-zero addressing rules](https://www.ibm.com/docs/en/hla-and-tf/1.6.0?topic=instruction-using-general-register-zero).
- Long functions, literal reach, HLASM continuation fields, symbol visibility,
  external relocations and the selected character encoding need assembler and
  binder validation. Uppercasing a name is not a universal ABI requirement.
- The 64-bit terminal-bit parameter-list convention, aggregate arguments,
  register returns and cross-language calls have not been certified against a
  chosen native compiler/runtime contract.

These limitations remain visible rather than being hidden by the directory
reorganization. ISA validation should use the applicable edition of the
[z/Architecture Principles of Operation, SA22-7832-14](https://www.ibm.com/docs/en/module_1678991624569/pdf/SA22-7832-14.pdf?cp=HW11W)
and the original [System/370 Principles of Operation, GA22-7000-0](https://www.bitsavers.org/pdf/ibm/370/princOps/GA22-7000-0_370_Principles_Of_Operation_Jun70.pdf).
The latter is an IBM manual hosted in the Bitsavers archive.

## Validation boundary

`tests/mainframe_mir_lowering_regression.c` exercises source lowering, variant
legality, spilling and HLASM output. `tests/backend_dispatch_regression.c`
exercises all seven PPC/SystemZ targets through registered backend callbacks,
including unsupported configurations, declarations and empty modules.

These tests run on Windows and Linux hosts. They do not assemble with IBM HLASM
or execute under MVS/z/OS. QEMU's Linux s390x user mode cannot validate MVS
linkage or a GOFF load module. Native readiness requires a selected OS/runtime,
the matching assembler and binder, and bidirectional call tests covering
register preservation, argument lists, frame storage and floating point.
