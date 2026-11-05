# Agent instructions

Applies throughout this repository. Follow the owner's explicit instructions first.

## Understand before changing

- Read the code you will change, what calls it and what it calls, and its doc in `doc/`, before editing. Know the path
  end to end (builder, verifier, optimizer passes, codegen preparation, MachineIR lowering, allocation, printer) before
  deciding where a change belongs.
- Never write from an assumption about an API, an IR rule, an ABI or an instruction: open it and check (the verifier's
  rule, the ABI document, the ISA manual, the printer that emits it). If the task needs more than you have read, read
  more or narrow the task; never guess.
- Look for an existing helper, analysis or table before adding one (`src/backend/common`, `src/opt/opt_utils.h`,
  `src/analysis`, the isel facts); extend it instead of writing a second copy.

## Working rules

- Start at [doc/INDEX.md](doc/INDEX.md): it routes each task to the doc that maps the code involved. Read that doc
  before searching the source.
- Keep documentation current: a change is done only when its docs match it, so update the matching doc in `doc/` in
  the same change. A doc that contradicts the code is a defect: fix it when you find it, or report it. Docs are in
  English and describe the current state, never a changelog. `doc/REVIEW.md` is the list of findings and their status:
  update an item when a change completes or advances it (what is done, what stays open, measured effect). Use
  [README.md](README.md) for the overview, building and benchmarks; keep its benchmark tables in step with measurements
  (machine, compiler versions, method). Keep this file limited to agent guidance.
- The library and the samples build without warnings (`-Wall -Wextra -std=c11`); new code adds none. Fix causes instead
  of suppressing diagnostics.
- Write readable C11: lines at most 180 characters, one statement per line, clear names, the surrounding file's brace
  style. Comments only for non-obvious invariants, external contracts (ABIs, object formats, instruction semantics) or
  formulas; no section banners, implementation history or commented-out code.
- Verify claims against the current implementation. Preserve the owner's changes and stay within the task. Inspect
  `git status` and `git diff`; do not run `git add`, `git commit` or `git push` unless the owner asks for it.
- When the owner asks for a commit, never add a `Co-Authored-By` trailer or any other co-author or attribution line to
  the message, whatever a tool's defaults suggest.
- Use the owner's language when reporting work. State the changes, the checks performed and the remaining limitations.
  Report performance as clang and ANVIL side by side (milliseconds, or instruction counts under qemu), not only ratios.
- Hard rule: keep the agent's temporary folder clean (`/tmp/claude-<uid>` on Linux, `$TMPDIR/claude` on macOS,
  `%LOCALAPPDATA%\Temp\claude` on Windows). Captures, logs, probe output, scratch scripts and file copies live there
  only while a step needs them; delete them as soon as it is done, and leave nothing behind at the end of a task.
  - List what is there and decide file by file. Delete a file only when the work it served is finished and its result
    is kept where it belongs (applied and committed, or recorded in the repository).
  - Anything an ongoing task still uses stays until that task ends; a file worth keeping beyond it moves into the
    repository first.
- Hard rule: never read environment variables in C code (library, samples, tests, examples) to configure behaviour,
  tune values or switch features. Settings belong in the API (context, function and pass-manager setters), command-line
  flags of the samples, or code constants. A variable may be read only in a throwaway local experiment, removed right
  after it, before anything else is done; it never reaches a commit.

## ANVIL is generic

- ANVIL serves any front end and any target. No IR operation, API, pass, flag or comment exists for one sample, one
  language or one architecture. A language's library or runtime semantics (C's `strlen`, a Smalltalk primitive) belong
  in that sample; a front-end opt-in does not make them generic. The generic route is a better optimization of the code
  itself.
- Target policy lives in descriptors: `anvil_arch_info_t` (`src/core/context.c`), the ABI descriptors under each
  backend's `abi/`, and the `anvil_backend_ops_t` vtable (`vector_operation_cost`, `local_call_convention`, ...). Never
  branch on the architecture inside a pass, the shared lowering helpers or a printer's generic code. Generic headers and
  passes do not name ISAs or registers, not even as examples.
- Instruction and vector choices follow the CPU model's features (`anvil_ctx_has_feature`, the cost hooks), never fixed
  per-architecture assumptions.
- Every improvement reaches every target (x86-64, i386, ARM64, PowerPC 32/64/64LE, the SystemZ family) in the same
  round. A target that cannot take it (no vector unit, no such form) is named with the reason in the matching doc.
- Samples are samples. `samples/mcc` and `samples/smalltalk` use only the public headers (never
  `anvil_internal.h`). Smalltalk takes global names from the `st-image` sources, not from C tables.

## Code organization and quality

- One implementation per rule: a formula, table, check or lowering lives in one place and every caller uses it (shared
  MachineIR lowering in `src/backend/common`, alias facts in `src/analysis`). Search before adding a helper.
- A pass leaves verifiable IR on every path, allocation failure included: allocate everything first, then commit the
  rewrite (tests inject failures with `anvil_test_fail_alloc_after`). Prefer an error result over silently skipping
  work after a failed allocation.
- Code generation is deterministic: never order output by pointer values or hash iteration.
- Every behavior change gets a regression case: a unit test in `tests/` that builds IR through the API (front-end
  independent, on every target the change touches), and for C-visible behavior an execution test in
  `samples/mcc/tests/exec/` compared with clang on every target.
- A performance change is measured A/B: the previous commit (a worktree in the agent's temporary folder) against the
  change, both built and run back to back on the same idle machine, with `bench/ir/run.sh` (code generation alone)
  and the MCC bench, reporting the run-to-run noise. A change needs a benchmark that exercises it: add one to
  `bench/ir` (kernel, C reference and driver) and to `samples/mcc/tests/bench` when none does, before claiming a
  gain. Passing tests is not a performance result.
- Before a task is called done, its whole diff is reviewed against these rules, and the findings are fixed before the
  commit. The review looks for:
  - the same rule, formula, constant or lowering written twice, within the diff or alongside code that already exists
    (search the repository for each new helper and constant);
  - target, language or sample specifics in generic code;
  - raw numbers that repeat a named constant;
  - members the change left unused;
  - comments that restate the code;
  - docs, `doc/REVIEW.md` items and README tables left behind by the change.

  A large change also gets a fresh read-only review that is not anchored on the author's reasoning.

## Layout

- `include/anvil/`: the public API (`anvil.h`, `anvil_opt.h`, `anvil_machine.h`, `anvil_analysis.h`, `anvil_cpu.h`,
  `anvil_debug.h`) and `anvil_internal.h`, for the library only.
- `src/core` (IR, types, builder, verifier, context and CPU tables), `src/opt` (passes, codegen preparation),
  `src/analysis` (alias and memory facts), `src/machine` (MachineIR, liveness, coalescing, allocation, MIR verifier).
- `src/backend/common` (shared lowering, isel facts, layout, vector ISA tables) and one directory per family: `x86`,
  `arm64`, `ppc`, `systemz`, each with `abi/`, `asm/` and `targets/` where it applies.
- `samples/` (front ends), `examples/` (API examples with runtime tests), `bench/ir` (kernels written against the API,
  compared with clang), `tests/` (regressions), `doc/`.

## Verification

Run suites one after another: the MCC execution harness shares its output directory. MCC links `libanvil` statically,
so rebuild it (`make -C samples/mcc`) after a library change, and never rebuild the library or MCC while a suite runs.

- `make tests`: core, optimizer, MachineIR, backend and Smalltalk regressions.
- `make test-examples`, `make test-examples-advanced`, `make test-aggregate-abi`.
- `make -C samples/smalltalk test`, and the applications under `samples/smalltalk/examples` built with
  `make smalltalk-app` and run (see `samples/smalltalk/README.md`).
- `make -C samples/mcc test-all`.
- `samples/mcc/tests/exec/run_exec_tests.sh --all-opts`; `-O3 --mcc-flags=-mcpu=haswell` with and without
  `-fno-fp-vectorize`; `--all-opts --arch=x86 --mcc-flags=-mcpu=i486 --target-cflags=-m32` and the same with
  `-mcpu=pentium4`.
- Other targets under qemu-user: `tests/run_arm64.py --corpus` for ARM64; PowerPC and every target's kernels through
  `bench/ir/run.sh --arch=...`, which compares the outputs with clang's. SystemZ (HLASM) cannot run here: its changes
  are checked by unit tests and by reading the emitted code.
- For documentation-only changes, check facts, links, line length and the diff; avoid unrelated suites.
