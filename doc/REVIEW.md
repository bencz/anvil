# ANVIL implementation review

Findings from a full review of the library (core, optimizer, MachineIR and
register allocation, and the x86, ARM64, PowerPC and SystemZ backends).
Samples are out of scope here. Each item has an ID, a priority and a status:
`[x]` fixed, `[ ]` open, `[~]` partly done. Priorities are P0 (wrong code,
crash or output that does not assemble), P1 (structure, vtables,
performance) and P2 (quality and nice-to-have).

Guiding rules for every fix: ANVIL stays agnostic (no IR or API for one
frontend or one target); target, ABI, object-format and word-size policy
lives in backend descriptors and vtables; instruction choices follow CPU
model features.

## 1. Correctness (P0)

### Core and IR
- [x] C1 A global used as a value means its address but has type T: `add @g, 1` adds to the address. Globals must be `ptr<T>` values (types.c:1243, builder.c:570). Fixed: data globals are rejected outside address positions (builder, PHI, verifier); addresses come from a GEP.
- [x] C2 Loads/stores of unsized types (opaque struct, function) pass the builder and verifier (verify.c:797, builder.c:505). Fixed: loads/stores require a sized first-class type (builder and verifier).
- [x] C3 CPU feature bits collide across architectures; `anvil_ctx_enable_feature` accepts any bit (context.c:816). Validate against the arch's feature mask. Fixed: `anvil_ctx_enable_feature` accepts only bits some CPU model of the architecture defines (`anvil_cpu_arch_feature_mask`).
- [x] C4 `ANVIL_CPU_GENERIC` means zero features on every arch (x86-64 loses SSE2). Resolve it to the arch default CPU. Fixed: GENERIC resolves to `anvil_arch_default_cpu`.
- [x] C5 `anvil_ctx_set_abi` does not check the ABI against the arch; `set_fp_format` accepts IEEE on BFP-less S/390 CPUs; `set_cpu` does not recheck the FP format. Fixed: backends declare `abi_support` and `anvil_ctx_set_abi` rejects unimplemented ABIs; the architecture descriptor lists its floating-point formats and the CPU features IEEE needs (S/390: BFP, G5+); `set_fp_format` and `set_cpu` both check it, so a BFP-less model cannot be combined with IEEE.
- [x] C6 `anvil_switch_add_case` checks the target block against the insertion function, not the switch's function (builder.c:723). Fixed: the target must belong to the switch's own function.
- [x] C7 Builder failure paths leave half-built instructions in blocks; `alloca_dyn` accepts an i1 count the verifier rejects. Fixed: `alloca_dyn` rejects an i1 count; builders insert only complete instructions (already true) and now discard the unfinished one at once when they fail; switch cases and PHI incomings grow both arrays before committing.
- [x] C8 CPU table: POWER10 ORs ZARCH bits; POWER5 claims POPCNTD (POWER7+). Fixed: POWER5/5+/6 no longer claim POPCNTD (POWER10 was already correct).

### Optimizer
- [x] O1 `simplify_cfg` can merge the entry block into its predecessor and delete it (simplify_cfg.c:618). Fixed: the entry block is never merged away.
- [x] O2 Hitting the pass iteration limit fails compilation instead of keeping the verified IR (opt.c:476). Kept by design: a regression test requires the error (it exposes oscillating passes); the limit is configurable (`anvil_pass_manager_set_iteration_limit`).
- [x] O3 SCCP calls simplify-cfg even when that pass is disabled (sccp.c:313). Kept by design: the cleanup is part of SCCP's result (dead edges and PHI inputs it creates); documented in sccp.c.
- [x] O4 The vectorizer dereferences a NULL backend (vectorize.c:227). Fixed: checks the backend before its ops.

### MachineIR and register allocation
- [x] M1 Frame slots are capped at 8191 bytes (uint16 size_bits); larger locals overwrite saved registers and the return address. Fixed: frame slots sized in bytes (`size_t`); `anvil_mir_layout_frame_slots` shared by all printers.
- [x] M2 Local alignment above 16 is clamped silently in every printer. Fixed: the shared layout fails instead of clamping when a slot needs more than the stack alignment.
- [x] M3 The split pass inserts reloads inside RET_VALUE_PART…RET bundles (machine_ir.c:2317). Fixed: the split pass holds bundle members and puts reloads before, spill stores after the bundle (shared `mir_bundle_continues`).
- [x] M4 A fixed-register interval evicts and spills a whole interval while other registers are free (regalloc.c:566). Fixed: a displaced interval moves to a register free over its whole range; it spills only when none is.
- [x] M5 Scratch demand can exceed the scratch pool and fail late without rollback (machine_ir.c:1899, 2430). Fixed: coverage is checked per bundle against the scratch registers free there, before any rewrite; a failure restores the vreg count. Scratch occupancy is precomputed once with CFG liveness (was a per-operand O(vregs x instrs) scan).

### x86 / x86-64
- [x] X1 int->float from i8/i16 converts garbage high bits (gas64.c:1066, gas32_sse2.c). Fixed: `common/narrow_mir.c` extends narrow sources before int->FP in every backend.
- [x] X2 i386 stack is misaligned by 8 at every call (gas32.c:217). Fixed: frame size keeps ESP aligned to the platform descriptor's `stack_alignment`.
- [x] X3 i386 FP mask labels repeat per function: two functions with fneg fail to assemble. Fixed: mask labels carry the function name.
- [x] X4 Darwin puts binary FP masks in `__cstring`. Fixed: masks go to the object format's 16-byte literal section.
- [x] X5 Internal-linkage functions are emitted `.globl` (all backends). Fixed: MIR functions carry linkage; printers emit bindings through `anvil_obj_emit_binding` (SystemZ: see Z3).
- [x] X6 Callers do not extend i1/i8/i16 arguments to 32 bits (clang/Apple callees assume it). Fixed: x86-64 ABI descriptors: `caller_extends_narrow_args`.
- [x] X7 Darwin global alignment capped at 8. Fixed: globals go through the shared `anvil_gnu_emit_globals` (uncapped `.p2align`).
- [x] X8 COFF stdcall/fastcall `@N` decoration disagrees between caller and callee with byval arguments. Fixed: one helper (`x86_decorated_symbol`) derives `@N` from the function type for both caller (in lowering) and callee.
- [x] X9 fastcall ECX/EDX filled by sequential moves (can overwrite each other). Fixed: register arguments move as a parallel move (cycles use `xchgl`).
- [x] X10 i386 variable shift loads ECX before reading an operand that may live in ECX. Fixed: both operands are stored before ECX/EAX are written.
- [x] X11 Win64 `va_start` uses `num_params` (wrong with a register-returned SRET). Fixed: uses the named argument slot count.
- [x] X12 No PIC/GOT/PLT relocation model; no `.cfi_*`, no `.note.GNU-stack`. Partly fixed: ELF and Mach-O functions carry `.cfi_*` frame directives on every GNU backend (unwinding verified through MCC frames), ELF modules end with `.note.GNU-stack`, Mach-O with `.subsections_via_symbols`; block labels use the format's assembler-local prefix (Mach-O `L`). Relocation models: `anvil_ctx_set_reloc_model` (STATIC/PIE/PIC; DEFAULT = target convention, Mach-O PIE), backends declare what each ABI can produce; one rule (`anvil_symbol_uses_got`) decides which symbols go through the GOT/PLT (PIC: everything preemptible; PIE: what the module does not define) and lowering marks SYMBOL_ADDR/CALL. x86-64 @GOTPCREL/@PLT, ARM64 :got:/@GOTPAGE, ppc64 TOC @got, ppc32 bcl/mflr PC-relative + @got with GOT-indirect calls, i386 call/pop GOT pointer + @GOT/@GOTOFF with GOT-indirect calls and stack-based SSE sign masks. Verified: shared objects built from MCC output (`-fPIC`) link and run on x86-64 and i386, link on ARM64/PPC; all qemu sweeps and exec suites pass with -fPIC.
- [x] X13 i386 has no atomics. Fixed: 8/16/32-bit atomics; CMPXCHG/XADD gated by the new `ANVIL_FEATURE_X86_I486` (i386 models are rejected with a clear error and report not lock-free), fences use MFENCE with SSE2 and a locked OR otherwise. 64-bit atomics (CMPXCHG8B, `ANVIL_FEATURE_X86_CX8`) remain open.

### ARM64 / PowerPC
- [x] A1 PPC64 calls have no `nop` after `bl`: linking with libc or shared objects fails. Fixed: `bl sym` + `nop` on ELFv1 and ELFv2.
- [x] A2 ELFv1 prologue stores r2 into the caller's TOC slot before `stdu`. Fixed: no TOC store in the prologue; indirect calls save/restore r2 in the current frame.
- [x] A3 ELFv2 indirect calls never save/restore r2. Fixed: ELFv2 indirect calls save/restore r2 at 24(r1) and pass the target in r12.
- [x] A4 Narrow integers (i1/i8/i16) are not normalized before width-sensitive ops (div, shifts, compares, conversions, conditions) on ARM64/PPC. Fixed: `anvil_mir_add_binary_extending` extends operands of width-sensitive ops (ARM64, PPC, SystemZ).
- [x] A5 Callee result extension missing (PPC64 ELF ABIs, ARM64 Darwin). Fixed: ABI descriptors carry `result_extend_bits` (PPC ELF64 64, SVR4 32, ARM64 Darwin 32).
- [x] A6 PPC SMOD/UMOD and large-offset paths use r11, which is a spill-reload scratch: remainder becomes 0 under pressure. Fixed: remainders and large offsets use r0 (never allocated) instead of the r11 scratch.
- [x] A7 PPC DS-form `ld/std/lwa` with displacement not a multiple of 4 (packed structs) does not assemble. Fixed: `ppc_emit_mem_access` falls back to the indexed form for unaligned DS-form displacements.
- [x] A8 PPC frames over 32 KB do not assemble (r31 save displacement). Fixed: r31 is saved through r12 (old SP) with a short displacement.
- [x] A9 PPC32 epilogue reads below SP. Fixed: the epilogue reloads r31 before releasing the frame.
- [x] A10 ARM64 dynamic alloca overlaps the outgoing argument area. Fixed: the block starts above the outgoing argument area.
- [x] A11 Data alignment: `.align <bytes>` read as log2 on ARM64 Linux and PPC; Darwin caps `.p2align` at 3. Fixed: `.p2align` everywhere through the shared global emitter.
- [x] A12 Darwin variadic aggregates go to registers (Apple puts unnamed arguments on the stack). Fixed with an IR extension: `anvil_build_call_attrs` gives unnamed arguments a call-site attribute (BYVAL ptr<aggregate>; checked by builder and verifier, read by every backend through `anvil_call_arg_attr`, copied by inliner/unroller via `anvil_instr_copy_attributes`). Callers place them by each ABI's unnamed rules (Darwin stack, ELFv2 HFAs as GPR images); callees read them with va_arg on every target: shared cursor rules (`common/va_cursor`: i386, Win64, Darwin, PPC64), SysV eightbytes from the save areas, AAPCS64 GPR/HFA/reference, ppc32 by reference, and new MVS variadic support (cursor over the parameter list). MCC passes unnamed records BYVAL. Cross-checked against gcc/clang in both directions on x86-64, i386, ARM64, ppc64, ppc64le (ppc32 against its verified callee).
- [x] A13 ELF globals lack `.type`/`.size`; const data in `.data`; COMMON emitted as `.globl`. Fixed: `.type/.size`; zero-initialized objects in `.bss`; the new `anvil_global_set_constant` attribute puts constant data in `.rodata` (or `.data.rel.ro` / `__DATA,__const` when it holds addresses); uninitialized COMMON globals are `.comm` symbols (alignment in bytes on ELF, as a power of two on Mach-O and PE). MCC marks `const` objects.
- [x] A14 PPC has no atomics and no variadic callee support. Fixed: atomics as lwarx/stwcx. (ldarx/stdcx. for 64-bit) reservation loops, 8/16-bit through a shifted mask on the containing word (both endians), sync/lwsync/isync by order; variadic callees through ABI hooks: ELF64 homes r3-r10 into the parameter save area (va_list = cursor), SVR4 ppc32 fills the native 12-byte state with a 96-byte register save area. Verified under qemu (ppc32, ppc64, ppc64le), including va_list passed to a clang-built vprintf.
- [x] A15 SVR4 ppc32 float stack arguments and ELFv2 partially-FPR HFAs differ from GCC. Fixed: ppc32 floats past f8 take 4-byte stack words (doubles 8-aligned); an ELFv2 HFA that only partly fits passes its leading members in FPRs and the rest in their own parameter doublewords. Both cross-checked with clang under qemu, in both call directions.

### SystemZ (HLASM)
- [x] Z1 Operands are padded with blanks (`L R2  ,0(,R14)`): HLASM ends the operand field at the blank. Nothing assembles. Fixed: operands are no longer padded.
- [x] Z2 No register equates (R0-R15, F0-F15). Fixed: `emit_module_begin` prints R0-R15/F0-F15 equates once per module.
- [x] Z3 Unnamed CSECT per function, AMODE/RMODE repeated, no ENTRY for functions, no EXTRN for callees. Fixed: one named CSECT with AMODE/RMODE per module; ENTRY for external functions, EXTRN for declared ones.
- [x] Z4 One base register (4 KB) and based branches: larger functions break. Fixed: with relative branches (S/390+) R12 addresses the literal pool (LARL / BRAS over AL4), functions reach 64 KB; without them (S/370, S/370-XA) the printer measures the function (instruction formats, distinct literals) and, past one base register, bases R12 on the pool (BAL over AL4 at entry) and turns every based branch into `L R15,=A(label)` + `BCR`; small functions keep the cheaper entry-point base. A pool beyond 4 KB is refused instead of emitted unassemblable. Open: BRCL for functions past 64 KB on S/390+; multiple pools.
- [x] Z5 Literal pool only at module end: literals out of reach. Fixed: every function ends with its own LTORG.
- [x] Z6 Frame/argument/spill displacements above 4095 are emitted. Fixed: `systemz_frame_operand` builds large frame addresses in an emitter temporary.
- [x] Z7 `LA reg,SYM` for external or distant symbols on 24/31-bit. Fixed: 24/31-bit symbol addresses load an `=A(...)` constant.
- [x] Z8 LARL targets at odd addresses (unaligned data/strings). Fixed: `DS 0H` before every string literal.
- [x] Z9 `SRDAG` does not exist (64-bit signed division). Fixed: 64-bit signed division divides R1 with DSGR (no SRDAG).
- [x] Z10 TRUNC/SEXT/ZEXT are plain copies (no extension). Fixed: SEXT/ZEXT define every result bit (SLL/SRA, N, LGFR/LLGFR); narrow users extend (narrow_mir).
- [x] Z11 Unsigned 32-bit division loop uses `LA R1,1(,R1)` (truncates in AMODE 24/31). Fixed: the quotient bit is set with `O R1,=F'1'`.
- [x] Z12 HFP int->float without the HFP extensions is not normalized (small ints become 0.0f). Fixed: a normalizing `AD` of zero follows the unnormalized load.
- [x] Z13 LDER emitted without the HFP extensions. Fixed: without the HFP extensions FPEXT is `SDR` + `LER`.
- [x] Z14 S/390 G1-G4 allocate F1-F14 (only F0/F2/F4/F6 exist before the G5). Fixed: FPR pools follow ANVIL_FEATURE_ZARCH_BFP (F2 allocatable, F4/F6 scratch without it; F1-F13 + F14/F15 with it).
- [x] Z15 FPRs are not preserved across calls (no clobbers, no saves). Fixed: every MVS CALL clobbers all FPRs.
- [x] Z16 FPTOUI to u32 uses a signed conversion; UITOFP from u32 converts as signed. Fixed: z/Arch u32 FPTOUI converts at 64 bits; UITOFP converts the zero-extended value at 64 bits.
- [x] Z17 FP literals printed with `%g` (precision loss, nan/inf). Fixed: exact `=X`/`=XL8` bit patterns (HFP re-encoded; Inf/NaN saturate).
- [x] Z18 Narrow conditions tested with 64-bit LTGR. Fixed: condition tests use LTR or LTGR by operand width.
- [x] Z19 Names: collisions after uppercasing, 63-char labels, 8-char OBJ externals, column-72 continuation. Fixed: one deterministic mapping for every module: lowercase names of up to 8 characters uppercase; any other name keeps up to three characters plus '@' and a hash (at most 8, so OBJ externals fit and no line reaches column 72); ENTRY/EXTRN of renamed symbols get an ALIAS with the source spelling (continued past column 71); internal labels use '#' and block indices, so they cannot collide with symbols.
- [x] Z20 31-bit pair add uses signed AR (fixed-point overflow trap if enabled). Fixed: register add/sub use ALR/SLR/ALGR/SLGR.

## 2. Structure and vtables (P1)

- [~] S1 Shared frame layout for all printers (five copies today), alignment checked, not clamped.
- [ ] S2 Shared register-allocation driver (five copies): pools, scratch, call clobbers from descriptors, one verify.
- [~] S3 Caller-saved registers allocatable on ARM64, PPC, SystemZ, i386 with CALL clobbers from ABI descriptors; register hints for ABI copies. Partly done: the linear scan follows copy hints (a value copied to or from a fixed register prefers it; PHI-copy partners prefer each other), so argument/result copies vanish; ARM64 allocates x0-x8 and v0-v7/v16-v31 (AAPCS64 register-convention table: pools, scratch, call clobbers), PowerPC r3-r10 and f1-f12 (volatile sets in the ABI ops; conversions the CPU cannot do inline count as calls, so regalloc takes the CPU features), and leaf functions no longer need frames there. x86-64 allocates all 14 GPRs and xmm0-xmm13: the printer's temporaries are declared per instruction (`anvil_mir_set_instr_scratch`: CL for a variable count, RDX:RAX for division and high products, R11/RAX for conversions, FP constants and dynamic allocation), so the allocator keeps only that instruction's operands, result and the values live across it out of them; the spill scratch registers (r11, r10, rax; xmm8-xmm10) stay in the pool and are reserved only for a function whose reloads cannot be placed otherwise (`anvil_regalloc_allocate`). Division no longer copies its divisor into R11, and a subtraction whose result shares the subtrahend's register is `neg` + `add` instead of a detour through RAX. i386 allocates all six GPRs (caller-saved ECX/EDX/EAX first) and XMM0-3/5-7 (x87 home slots alike): CL is a variable shift's printer scratch; division, high products and dynamic allocation only clobber EDX:EAX/EAX (they read their operands first; a divisor in EAX/EDX divides from a stack copy); the stack scratch slots remain only for FP constants and conversions. Bytes and booleans stay in EAX-EDX through a generic allocator option (`narrow_bits`/`narrow_regs`, honoured by reloads too), which removed every byte-register fallback. The allocation driver no longer reserves spill scratch for a whole function at once: where reloads do not fit, it spills the values occupying scratch registers there (`anvil_mir_spill_for_scratch`, recomputable ones first), then reserves 1, 2, ... scratch registers. Reloads stay usable within a block where the printer declares every write to the scratch registers (`printer_declares_writes`, x86): repeated reloads and store-then-reload pairs disappear. Open: the x86-64 XMM temporaries (`sse_temps`); SystemZ gains nothing (MVS linkage saves R14-R12 with one STM).
- [ ] S4 Shared IR->MIR lowering helpers (value map O(1), phi edges, switch, select, GEP) behind target callbacks (four to five copies).
- [~] S5 Object-format descriptors (ELF, Mach-O, COFF, GOFF/OBJ): sections, symbol visibility, labels, alignment directive, `.type/.size`, rodata/bss; shared GNU data emission. Also: ELF/Mach-O module trailers (`.note.GNU-stack`, `.subsections_via_symbols`), the CFI switch and the assembler-local label prefix live in the descriptor; three backends share `anvil_obj_finish_module`.
- [x] S16 Shared small helpers instead of per-backend copies: `anvil_parallel_move` (register parallel moves with swap or scratch; i386 fastcall/result pairs, x86-64 atomics), `anvil_format_failure` (seven verifier/legalizer copies), printf-format checking on `anvil_strbuf_appendf`/`anvil_set_error` (it found an S/370 `SLL` operand bug and argument-order slips), PPC backend operations shared by the three variants.
- [ ] S6 Target facts in one place: core switches on arch for calling convention, syntax, FP format, data layout; arch info duplicated in core and backends.
- [ ] S7 Word-size mnemonic branches in PPC and SystemZ printers into descriptor tables.
- [ ] S8 ABI policy in printers (callee-saved sets, temp registers, va_start registers, MVS call sequence) into ABI descriptors.
- [ ] S9 Runtime helper names (libgcc) from a runtime/ABI descriptor, not hard-coded in wide_mir.
- [ ] S10 Central opcode info table (name, arity, flags, terminator, side effects, trap) for core and optimizer.
- [ ] S11 Use lists and replace-all-uses; positional insertion; public erase/move.
- [ ] S12 Uniqued types and constants per context.
- [ ] S13 Verifier and builder quadratic paths (instruction order, block membership, reachability, phi checks).
- [~] S14 MIR: cached CFG/liveness; O(n^2) coalescing; per-temp realloc; label naming in printers. Partly done: the copy coalescer also folds PHI moves (`t = op; x = COPY t`, x multiply defined) by letting op write x when nothing in between touches x, so loop increments become `add $1, %reg`. Open: cached CFG/liveness, the quadratic scans, label naming.
- [ ] S15 A register-assignment verifier.

## 3. Optimizer (P1)

- [~] P1 Constant interning so CSE/GVN see equal constants (with S12). Partly done: CSE and GVN compare and hash integer constant operands by type and value, so `gep @a, 0, 0` written twice is one address (strings: three hoisted copies of one array address took callee-saved registers). Open: interning itself (S12).
- [ ] P2 mem2reg/SROA for allocas outside the entry block (hoist static allocas).
- [~] P3 GVN over all pure ops (GEP, casts, select, FP) with a scoped dominator-tree table. Partly done: CSE and dominator-scoped GVN key expressions by result type and now cover integer/pointer casts, NEG and NOT; a MachineIR local value numbering (`anvil_mir_local_cse`) removes the repeats lowering creates (index widening per GEP). Open: GEP/select/FP operations.
- [~] P4 Loop rotation. Partly done at MachineIR level: `anvil_mir_duplicate_branch_targets` copies a small test block into the jumps that reach it (keeping one incoming edge), so top-tested loops branch back from the latch (one branch per iteration). Open: IR-level rotation for the optimizer (LICM/unrolling see top-tested loops).
- [~] P5 Induction-variable widening and strength reduction. Done: `ANVIL_PASS_LOOP_STRENGTH` (O2+, after loop vectorization): in a loop stepping its induction variable by one, `gep T base, i, ...` with a loop-invariant base and sizeof(T) no scaled-index size (structs: 24, 40, 56 bytes...) gets a pointer PHI from `gep T base, start` advancing one T in the latch, the access becoming `gep T p, 0, ...` (an add per iteration instead of an extension, a multiply and an add per access stream). A narrow index (extended by its signedness) is rewritten only when the header's `i < n` (signed or unsigned as i is) proves it does not wrap inside the loop. ARM64 `sumf` over 40-byte records: 12 -> 8 instructions per iteration. The index may also follow a leading zero into an array (`gep [N x T] base, 0, i, ...`, globals such as nbody's `bodies[j]`), the access becoming `gep T p, 0, ...`. The same proof widens a narrow induction variable on 64-bit targets: I = ext(i) steps by itself, extensions and GEP indices read I, the header tests I < ext(n) (n extended once in the preheader) and other readers take trunc(I), so no `movslq`/`sxtw`/`extsw` runs per access; truncations carry a copy hint in the allocator. Open: replacing the exit test by a pointer compare (LFTR), subregister coalescing of trunc(I) with I.
- [ ] P6 Inlining at O2 with a cost model; allow static allocas in callees.
- [ ] P7 Alias analysis: distinct objects, non-escaping allocas, variable GEP bases.
- [~] P8 Fold BITCAST of constants; instcombine peepholes; trivial PHIs. Done: single-input PHIs fold when their block merges into its predecessor; pointer-to-pointer bitcasts lower to the same vreg.
- [x] P9 Tail-recursion elimination. Done: `ANVIL_PASS_TAIL_RECURSION` (O2+, after simplify_cfg): returns of `f(args)` or `x + f(args)` of the function itself (wrapping integer add, x computed before the call; directly or through the return block's PHI; also `f(args); ret void`) become jumps to a header after the entry whose PHIs take the parameters and an accumulator from zero; the other returns add the accumulator. Functions with allocas, variadic or by-value arguments keep their frames. `fib(n - 1) + fib(n - 2)` keeps one call per level (clang's shape); test `tail_recursion_c99`.
- [ ] P10 FP: policy for observable FP exceptions; CSE/DCE/LICM of FP ops when unobservable.
- [ ] P11 Call-effect inference for defined functions.
- [ ] P12 Mark-sweep DCE (dead PHI cycles, dead loads).
- [ ] P13 Known-bits used for sdiv->shift, sext->zext.
- [ ] P14 Per-pass IR verification optional; rerun only dirty passes; drop subsumed passes at O2.
- [ ] P15 Shared opt utilities (clone, insert, may-trap table, def-use, type width).
- [x] Q13 Square root reached the back ends as a libm call (nbody: `sqrt` through the PLT, ~7% of its time). Done: generic IR `FSQRT` (`anvil_build_fsqrt`, scalar or vector, IEEE correctly rounded; folds on non-negative IEEE constants; packs into lanes in the loop vectorizer). Scalar operations the tables price (FMA, FSQRT) share one MIR opcode, `ANVIL_MIR_OP_SCALAR` (`ir_op` + width), lowered through the same `anvil_vector_mir_form` path as vectors, so a new scalar operation is a table row: x86-64 `sqrtss/sd`, `sqrtps/pd`; i386 `sqrtss/sd` with SSE2, else x87 `fsqrt` (`x86_scalar_isa`, the same condition that picks the FP unit); ARM64 `fsqrt`; PowerPC `fsqrt(s)` only with `ANVIL_FEATURE_PPC_FSQRT` (970 and POWER; the G3/G4 lack it and keep the call), `xvsqrtsp/dp`. Knowing that C's `sqrt`/`sqrtf` compute it is MCC's business: it keeps C's errno duty (`math_errhandling`) as gcc/clang do, calling the library only for a NaN result in a cold block; `-fno-math-errno` drops the call, `-fno-builtin`/`-ffreestanding` keep the plain call. Test `scalar_operation_regression`, MCC `math_sqrt_c99`.
- [ ] Q14 Reciprocal and reciprocal-square-root estimates (`rsqrtss/ps`, `rcpps`, `frsqrte`/`frecpe` + `frsqrts`/`frecps`, `xvrsqrtesp`) refined by Newton-Raphson steps: the modern form of the "fast inverse square root" trick. They are not correctly rounded, so they may replace `1/sqrt(x)` or `a/b` only in a function that permits approximation (a new FP policy beside contraction and reassociation, never the default); with it, the operation and step count come from the tables.
- [ ] Q12 Two architecture descriptors: `anvil_ctx_get_arch_info` reads the core `arch_info_table`, while each backend also defines an `anvil_arch_info_t` behind `get_arch_info` that nothing calls. The backend descriptor should be the single source (vtables first).
- [ ] M12 MCC: `_Alignas` and `__attribute__((aligned))` on file-scope declarations are rejected.
- [x] M14 MCC: integer promotions and usual arithmetic conversions. `unsigned char`/`unsigned short`/`_Bool` promoted to `unsigned int` (C11 6.3.1.1: `int`), and any unsigned operand made the result unsigned (C11 6.3.1.8: only when its rank is not lower; `long long + unsigned` is `long long`, LP64 `long + unsigned` is `long`). `(-5 + (unsigned char)1) < 0` was 0 and `/ 2` gave 2147483646. Fixed in `mcc_type_promote`/`mcc_type_common`.
- [x] M13 MCC: `#x` stringification gave the literal's value with its closing quote (`"u3\""`): the value pointed into the quoted buffer; and `sizeof "abc"` was the pointer size (also as an array bound, which made a VLA). Fixed in `pp_stringify_tokens` (spelling and quoted text built apart) and `sema_analyze_sizeof_expr` (a string literal is `char[length + 1]`); test `stringify_sizeof_c99`.
- [~] P17 Register pressure after O3 inlining: `strings` is 1.72x of clang at O2 but 2.11x at O3 (no loop vectorized). Done: every back end lowers in a loop-contiguous order (`anvil_opt_cfg_loop_layout`; plain RPO placed a loop latch after all code reachable from the loop exit, so the loop's values looked live to the end of the function and were spilled), the CFG's RPO puts a branch's true successor next, `anvil_mir_order_blocks` makes printers follow that layout (fallthrough, no out-of-line PHI edge blocks), and spill materialization recomputes symbol and frame addresses (`anvil_mir_instr_recomputable`) as it did constants; spills in `strings` main 51 -> 22. The clobber test of the linear scan asks whether a value is live across each clobbering instruction (per-block liveness, `clobber_map_add_across`), so a value inside a hole of its interval (redefined after a call, as a PHI fed by a fallback call) keeps a caller-saved register. Cold blocks (`anvil_block_set_cold`, carried to MIR by `anvil_mir_block_for`) and what only they dominate follow the rest of their innermost loop (off the fall-through path, but inside the loop so values do not stay live to the end of the function). Before allocation, `anvil_mir_split_around_cold_calls` copies each value live across a cold block's calls into a fresh vreg at the block's start and back after the last call: only the copy crosses the call, so where nothing survives a call (SysV XMM) the store and reload stay on the rare path. The split happens only in classes where no allocatable register survives the cold calls (SysV XMM): elsewhere the copies took callee-saved registers at the point of most pressure and the loop's own values spilled (nbody 135 -> 116 ms). Loop headers of the final layout (`anvil_mir_loop_header_blocks`, backward targets of non-cold blocks) start with `.p2align 4` on x86-64, ARM64 and PowerPC: the same strings code linked by clang ran 34 ms unaligned, 29 ms aligned. Two-address classes (`anvil_regalloc_class_config_t.two_address`, x86 GPR and SSE) hint a result into its dying first operand's register and away from the second operand's; x86-64 FP register copies use `movaps` (a `movsd` between registers merges and kept a false loop-carried dependency). nbody: 2.56x -> 1.63x of clang at O2. Open: live-range splitting (a whole-function value spilled everywhere is recomputed or reloaded inside hot loops), rotation of loops whose header exceeds 4 instructions, inliner pressure model.
- [~] P16 Loop vectorizer and target-preferred vector width. Done: `ANVIL_PASS_LOOP_VECTORIZE` (O3, `src/opt/loop_vectorize.c`) for innermost counted unit-stride loops (`<`, `u<`, `!=`), multi-index GEP streams, invariant splats, run-time overlap checks for stored streams and a scalar remainder loop; tests `loop_vectorize_regression` (x86-64, ARM64, ppc64/ppc64le POWER8, allocation-failure sweep) and MCC `loop_vectorize_c99`. Integer lanes and narrow C types (V5) and a cost model decide when it pays. Benchmark at O3: matmul 2.43x (O2) -> 1.19x of clang. Open: target-preferred width (AVX 256-bit), reductions (V8), interleaving, if-converted bodies (V6).

## 4. SIMD (P1)

- [~] V1 Target vector hooks for every backend: preferred width and per-operation cost, gated by CPU features (SSE2/AVX/AVX2/AVX-512, NEON/SVE, AltiVec/VSX/POWER9, z13 vector).
- [ ] V2 Integer vectors, logic, shifts, compares, selects, splat/extract/insert/shuffle, reductions.
- [ ] V3 Vector arguments/returns per ABI; spill slots and alignment for 32/64-byte vectors.
- [~] V4 Printers: SSE/AVX (VEX, vzeroupper), NEON (q registers, AAPCS64 v8-v15 low halves), AltiVec/VSX (vs32-63, LE swaps, alignment), z vector. Done: 128-bit FP lanes on SSE2, NEON and VSX (copies, spills, loads/stores, arithmetic, `splat`); the regalloc treats AAPCS64 v8-v15 and PowerPC f14-f31 as preserved only in their low 64 bits. Shared lowering decisions live in `backend/common/ir_lower.{h,c}`; a pointer-to-pointer bitcast lowers to the same vreg so addresses keep folding. Open: AVX/VEX, AltiVec-only CPUs, z vector, x86 i386.

### 4.1 Idiom recognition roadmap

The optimizer recognizes structures in the IR and rewrites them into generic
operations; each backend's cost hook says, for the CPU model, whether it has a
fast form and what it costs, so the same IR becomes the best sequence each
machine allows (and stays scalar where none exists). No operation exists for
one target or one front end.

| Generic operation (IR) | Recognized from | x86 | ARM64 | PowerPC | z |
|---|---|---|---|---|---|
| [x] V5 integer lane arithmetic, logic, shifts | loops over integer arrays; `trunc(op(ext a, ext b))` from C promotions computed in the narrow lanes | SSE2 `padd*`/`psub*`/`pmullw`/`pand`/`psll*`/`psra*` (count from a GPR), SSE4.1 `pmulld` | NEON `add`/`sub`/`mul`/`and`/`eor`, `ushl`/`sshl` (+`neg` for right shifts) | AltiVec `vaddu*m`/`vsubu*m`/`vmladduhm`/`vsl*`/`vsr*`/`vsra*`, POWER8 `vaddudm`/`vmuluwm`/`mtvsrwz` | `VA*`/`VN`/`VESL*` (open) |
| [x] V6 lane compare -> mask, select (blend) | `if` and `?:` in a loop body (if-conversion to `select`), min/max/clamp | SSE2 `pcmpeq*`/`pcmpgt*` (SSE4.1/4.2 for 64-bit), unsigned via `pmaxu*`/`pminu*`, `cmpps`/`cmppd` (all 14 predicates), select `pand`/`pandn`/`por` | `cmeq`/`cmgt`/`cmge`/`cmhi`/`cmhs`, `fcmeq`/`fcmgt`/`fcmge` (+`mvn` for unordered), `bsl`/`bit`/`bif` | `vcmpequ*`/`vcmpgts*`/`vcmpgtu*`, `xvcmpeq/ge/gt sp/dp`, `xxsel`, `xxlnor` | `VCEQ*`, `VSEL` (open) |
| [~] V7 mask any / first set lane | counted loops with data-dependent early exits (search, first mismatch, run length; any language) | `REDUCE_OR` of a mask: `pmovmskb`+`neg`+`sbb` | `umaxv`+`fmov`+`sxtb` | `mfvsrd`+`neg/or/sradi` (64-bit mode) | `VFEE`/`VFENE` (open) |
| [x] V8 horizontal reduction | loop accumulators: `+ & \| ^`, min/max written as `select(cmp)`, C-promoted narrow ones; FP sums only with `anvil_func_set_fp_reassociation` | halves folded with `pshufd`/`pshuflw`/`psrlw` and the lane op (`paddd`, `pmaxsd`, `addps`...) | `addv`, `smaxv`/`uminv`..., `faddp`, `ext`+op for bitwise | `xxpermdi`+`vsldoi` rotations and the lane op (every lane ends with the result) | open |
| [x] V9 fused multiply-add | `a*b+c` with a single-use product, under `anvil_func_set_fp_contraction`; `s += a*b` vectorizes as a sum | FMA3 `vfmadd231ps/pd/ss/sd` | `fmla`, `fmadd` | `xvmaddasp/dp`, `fmadd(s)` | open (`MADBR`) |
| [~] V10 widen/narrow, saturation | sums of narrow elements into wider accumulators (`int s += u8[i]`): `ACCUM_ZEXT/SEXT` (lane grouping left to the target); C clamps of widened sums/differences to the lanes' range (one or two selects, any compare form): `ADD_SAT_U/S`, `SUB_SAT_U/S`; open: mixed-width stores, general clamps (min/max lanes) | `psadbw` (u8), `pmaddwd` by ones (i16), word halves (u16), SSSE3 `pmaddubsw`+`pmaddwd` (i8); `paddus*`/`psubus*`/`padds*`/`psubs*`; open: `pmovzx*`/`packus*` | `uaddlp`+`uadalp`/`sadalp`; `uqadd`/`uqsub`/`sqadd`/`sqsub`; open: `uxtl`/`sqxtn` | `vsum4ubs`/`vsum4sbs`/`vsum4shs` + `vadduwm`; `vaddubs`...`vsubsws`; open: `vupk*`/`vpk*s*` | open: `VSUM`/`VUPH`/`VPKS` |
| [ ] V11 shuffles, interleaved access | stride-2/3/4 struct-of-arrays access, byte swaps, table lookups | `pshufb`, `shufps`, `unpck*` | `ld2`/`ld3`/`ld4`, `tbl`, `zip`/`uzp` | `vperm`, `xxpermdi` | `VPERM` |
| [ ] V12 bit counting and manipulation (scalar) | popcount/clz/ctz loops, `x & (x-1)`, rotates | `popcnt`, `lzcnt`/`tzcnt`, BMI `blsr`/`andn`, `rorx` | `cnt`, `clz`, `rbit` | `popcntd`, `cntlzd`, `cnttzd` | `POPCNT`, `FLOGR` |
| [ ] V13 CRC32C, carry-less multiply, AES/SHA rounds | table-driven CRC loops, explicit intrinsics | `crc32`, `pclmulqdq`, AES-NI, SHA | `crc32c*`, `pmull`, `aese`/`sha256h` | `vpmsum*`, `vcipher`, `vshasigma*` | `KIMD`/`KM`, `VGFM` |
| [ ] V14 wider vectors and predication | target-preferred width; tail without a scalar loop | AVX2 256-bit, AVX-512 masks | SVE `whilelt` predicates | POWER10 | z15 |

V5 infrastructure: one MIR opcode `ANVIL_MIR_OP_VECTOR` carries the IR lane
operation (`ir_op`) and lane width; each backend describes its vector
instructions once as a table (`backend/common/vector_isa.h`: operation, lane
width and kind, operand shape, CPU features, cost, mnemonic) that answers the
cost hook, MIR legality and the printer; the MIR function records the CPU
features it was lowered for (`anvil_mir_func_set_cpu_features`). Shifts take a
per-lane vector count or one scalar count (`ANVIL_VECTOR_SHAPE_UNIFORM`).
PowerPC vectors moved to the AltiVec registers (`ANVIL_MIR_REG_VECTOR`,
vs32-vs63) because VMX integer instructions cannot address vs0-vs31. The loop
vectorizer decides with a cost model: scalar cost per iteration against the
vector loop's cost per vector, its setup (end computation, splats, overlap
checks) and the scalar remainder, for the constant trip count or 64 assumed
iterations (a three-iteration loop stays scalar).

V6: a vector comparison yields a lane mask of integers of the lane width
(`anvil_type_vector_mask`, all ones = true) and `select` takes such a mask lane
by lane (`anvil_sem_select_types`, shared by builder and verifier). The
loop vectorizer turns compares into masks and selects into lane selects.
Generic work it needed: `simplify_cfg` folds a two-way branch whose sides
hold at most 8 speculatable instructions (integer/pointer arithmetic,
compares, selects, casts; no loads, division or FP arithmetic, whose effects
the IR models) into `select`s; GVN/CSE key on GEPs and selects (source type
included); trivial single-input PHIs fold when blocks merge; MCC allocates
every temporary (`?:`, `&&`, call ABI copies) in the entry block. Benchmarks
(O2): hashmap 1.72x -> 1.53x, qsort 1.38x -> 1.19x.

V7: a generic IR reduction `REDUCE_OR` (OR of every lane; nonzero exactly
when some lane is) and search-loop vectorization: a counted loop whose body
is a chain of blocks, each leaving on a comparison of lane values, runs whole
vectors while `i + lanes <= n`, tests each exit with the OR of its mask, and
on a hit resumes the original scalar loop at the start of that vector, which
finds the element and takes the original exit. Lanes past the leaving one are
read, so the target must declare `access_fault_granularity` (memory
protection granularity; 4 KB pages, 2 KB S/370 storage keys) and every
stream must be 16-byte aligned at the start (checked at run time; otherwise
the scalar loop runs). Comparisons of C-promoted narrow values narrow to the
lanes (unsigned order for zero-extended ones). Printers reduce masks with
short idioms (`anvil_vector_mir_is_mask`). Generic IR work it needed:
`const_fold` folds integer casts (incl. same-width bitcasts) of constants;
`simplify_cfg` branches on the boolean under `c == 0`, `zext(c) != 0`,
`xor c, 1` and threads a branch whose PHI a predecessor makes constant (`&&`
materialized as an integer). Open: unbounded searches (no trip count:
needs the same guarantee plus alignment peeling), first-set-lane extraction
instead of the scalar resumption, SSE4.2 `pcmpistri`.

V10 (first step): `ACCUM_ZEXT`/`ACCUM_SEXT` add the extended lanes of a
narrower vector of the same size into a wider one, in groups the target
chooses (only a full reduction of the result is defined), priced through the
new shapes `ANVIL_VECTOR_SHAPE_WIDEN2/4/8` and carried in MIR by the VECTOR
instruction's `widening` ratio. The loop vectorizer's LV_ACCUM class turns
`acc + ext(lane)` into it, choosing the narrowest accumulator lanes at least
as wide as the scalar accumulator that the target takes (wider lanes wrap to
the same low bits). Saturation: generic `ADD_SAT_U/S`,
`SUB_SAT_U/S` (scalar or vector, priced per target). The vectorizer reads a
clamp of `t = ext(a) +/- ext(b)` (extension matching the lanes'
signedness): each select normalized to `v > k` applying the lanes' maximum
(`v > k ? MAX : v`, k in {MAX - 1, MAX}) or minimum is one bound; parts of a
clamp are LV_BOUND; the select completing the bounds the sum's range needs
(unsigned add: the maximum, unsigned difference: zero, signed: both) is the
saturating lane operation. A clamp to any other bound stays scalar. The
i16 audio mix `t = x + y; clamp to [-32768, 32767]` becomes load, load,
`paddsw`, store. The first step surfaced an MCC bug: `unsigned char`/`unsigned short`
promoted to `unsigned int` and any unsigned operand won the usual arithmetic
conversions (`(-5 + (unsigned char)1) < 0` was false); fixed to C11 6.3.1.1
and 6.3.1.8 (M14).

V8/V9: reductions are a generic IR family (`REDUCE_ADD/AND/OR/XOR`,
`REDUCE_SMIN/SMAX/UMIN/UMAX`, `REDUCE_FADD`; `anvil_build_reduce`) and FMA one
IR operation (`anvil_build_fma`, scalar or vector) whose scalar availability
the same tables answer through `ANVIL_VECTOR_SHAPE_SCALAR`. The loop
vectorizer accumulates in a vector PHI from the operation's identity (min/max:
from the start value), reduces after the loop and combines with the start
value. `strength_reduce` contracts on one use snapshot. Two FP policies join
`fp_vectorization`: reassociation (MCC `-fassociative-math`) and contraction
(MCC `-ffp-contract=fast`); both default off. Printer temporaries are
descriptor facts: x86-64 `sse_temps` (SysV xmm15/xmm14, Win64 xmm5/xmm4), ARM64
`vector_temps` (v31/v30; v16-v19 are spill scratch and were unsafe as
temporaries), PowerPC v0/v1.

Order: V5 (integer lanes) is the base for V6/V7; V7 brings the string and
byte-search idioms; V8/V9 close most of the gap on numeric kernels (nbody,
dot products); V14 needs V1's preferred width.

## 5. Code quality (P2)

- [~] Q1 Addressing-mode folding (x86 base+index*scale+disp, ARM64 [x, w, sxtw #3], PPC indexed forms). Done for the common case: MachineIR loads/stores carry an optional index register and scale; a generic pre-allocation pass (`anvil_mir_fold_address_arithmetic`, target hook for legal forms) folds ADD(base, MUL/SHL(index, 1-8)) into every same-block access through the address and deletes the arithmetic. x86-64: disp(base,index,scale); ARM64: [base, index, lsl #log2(size)]; PowerPC: X-form (lwzx/ldx/lfdx...). Benchmarks: crc32 1.38x -> 1.13x, matmul 3.57x -> 2.57x, strings 2.35x -> 2.00x. i386: the same forms as x86-64. Open: displacement folding on ARM64/PPC, SystemZ (RX index register).
- [x] Q2 Selects with cmov/csel/isel/LOCR; FCMP fused with branches. Done: x86-64 branches straight on `ucomis` flags for the predicates one condition decides (OGT/OGE/OLT/OLE with swapped operands, ONE, ORD, UEQ, UGT/UGE, ULT/ULE, UNO); OEQ/UNE still materialize. A generic MIR pass (`anvil_mir_sink_compares`, run before allocation) moves a compare whose only use is a later BR_COND or SELECT in its block next to it, so printers fuse them (`anvil_mir_isel_feeds_next_select` beside `feeds_next_branch`). Integer selects: x86-64 `cmp` + `cmovCC` (FP compares too: `ucomisd` + `cmova`; FP results still branch), ARM64 `cmp` + `csel cc` without `cset`, PowerPC `cmpw` + `isel` with `ANVIL_FEATURE_PPC_ISEL` (POWER7+; r0 as the first source falls back to branches), SystemZ `LOCGR` with `ANVIL_FEATURE_ZARCH_LOADSTORE` (z196+). Open: i386 `cmov` (i386 now fuses compares with branches, Q6), FP selects by masks (`cmpsd`+`andpd`/`blendvpd`, `fcsel` is there).
- [x] Q3 Division by constants (magic numbers); remainders by powers of two. Done: generic IR `UMULH`/`SMULH` (high half of the double-width product; folded by `const_fold`, CSE/LICM/if-conversion treat them as pure), priced per target in the scalar tables; `strength_reduce` rewrites `udiv/sdiv/umod/smod` by any other constant with the Granlund-Montgomery reciprocal (Hacker's Delight 10-1 and 10-2 at the operand width, "add" fix-up, signed negative divisors). x86-64 `mul/imul` (RDX:RAX), i386 `mull/imull`, ARM64 `umulh/smulh` and `umull/smull`+`lsr #32`, PowerPC `mulhwu/mulhw` and 64-bit-mode `mulhdu/mulhd`; SystemZ keeps dividing. Tests: `scalar_operation_regression` (every quotient and remainder of 19 divisors x 19 dividends at 32/64 bits, signed and unsigned, folded exactly), MCC `division_constants_c99` (all targets). Open: the narrow-type promotion (i8/i16 run at 32 bits only after C promotion), even divisors pre-shifted to avoid the "add" fix-up, x86-64 `imul r64` + `shr` for 32-bit unsigned (one instruction less).
- [~] Q4 Constant materialization (movz/movk, xor zero, FP immediates, constant pools). Done on x86-64: `xorl` for zero, `movl` for values that fit 32 unsigned bits, `movq` for signed 32-bit ones, `movabsq` only beyond, `xorps` for FP zero. Open: the other printers, constant pools for FP.
- [~] Q5 Frame omission for leaf functions, ldp/stp pairing, red zone. Done: `anvil_mir_needs_frame_record` (calls, dynamic allocation, stack-passed arguments, va_start) shared by the printers: a function needing none and owning no frame bytes has no prologue or epilogue on ARM64, x86-64 (not Win64, whose unwind directives describe the prologue) and PowerPC, where the leaf test had counted the ABI linkage area of callers and kept a frame in every function. Open: red zone for small leaf frames (SysV 128 bytes, ELFv2 288), ldp/stp pairing.
- [~] Q6 i386 printer instruction selection (immediates, fusion, fall-through); i386 FPR allocation. Done: the i386 printer uses the shared isel facts (immediates for ADD..XOR/MUL/SUB/shifts/compares, `test` for zero, `lea` for three-operand adds, multiply by powers of two as shifts, dead constant MOVs dropped, load extensions skipped), fuses integer and FP compares with branches, falls through to the next block and aligns loop headers; i386 also runs the generic loop rotation, local CSE and addressing-mode folding (disp(base,index,scale), descriptor shared with x86-64 in `x86_address.h`). AT&T pieces both x86 printers spell alike (condition codes, FCMP branch table and materialization, labels, conditional branches) moved to `asm/x86_gas.{h,c}`. SSE register copies use `movaps` and conversions/scalar unary operations zero their destination first (no false dependency; matmul i386 74 -> 26 ms). Bench (clang -m32 -O2 / MCC pentium4 -O2, ms; before this round in parentheses): crc32 62/75 (140), fib 17/29 (45), hashmap 28/42 (75), matmul 15/26 (114), nbody 84/156 (508), qsort 103/121 (180), strings 21/50 (85); sieve overflows a 32-bit `long` (UB). Open: `cmov` selects, FP constant pool (constants still go through the frame), symbol displacements in addresses (`table(,%ecx,4)`), memory operands for spilled values.
- [ ] Q7 ARM64 LSE atomics, PPC isel/popcnt/POWER8 moves, SystemZ GIE compare-and-branch, relative branches.
- [~] Q8 Bit intrinsics (ctpop, ctlz, cttz, bswap, rotate), fma, sqrt, min/max gated by features. Done: fma (V9), sqrt (Q13), rotate: generic `ROTL` (count modulo the width, operands as a shift's), formed by `strength_reduce` from `(x << k) | (x >> (w - k))` with constant or variable k where the target prices it: x86-64 `rol` (immediate or CL), ARM64 `ror #(w - k)` or `neg`+`ror`, PowerPC `rotlw(i)`/`rotld(i)`; const-folded. Open: ctpop/ctlz/cttz/bswap (V12).
- [ ] Q9 Jump tables for switches.
- [~] Q10 Remove redundant extensions and self-moves. Partly done: the shared isel facts record the opcode of each single-definition value (`anvil_mir_isel_def_op`); x86-64, ARM64 and PowerPC know how far their narrow loads extend (movs/movz, ldrs/ldr w, lbz+extsb/lha/lhz/lwa/lwz) and print an extension of such a value of the same signedness as a move, or nothing in the same register. On x86-64 a 32-to-64-bit zero extension in place is dropped when a 32-bit operation wrote the value (it cleared the upper half): `movl %ecx, %ecx` is not move-eliminated and sat on crc32's loop-carried chain (82 -> 77 ms). The allocator gives up recomputable values (constants, symbol and frame addresses) first when no register is free, and a COPY between a register and a spill slot is the store or reload itself. Open: extensions of values whose known bits prove them (P13).
