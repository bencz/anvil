# MCC optimization pipeline

MCC does not perform source-AST optimization. After preprocessing, parsing,
and semantic analysis, it lowers the program directly to ANVIL IR. The `-O`
option is forwarded to ANVIL, whose verified IR/MIR passes own optimization:

| MCC flag | ANVIL level |
|----------|-------------|
| `-O0` | `ANVIL_OPT_NONE` |
| `-Og` | `ANVIL_OPT_DEBUG` |
| `-O1` | `ANVIL_OPT_BASIC` |
| `-O2` | `ANVIL_OPT_STANDARD` |
| `-O3` | `ANVIL_OPT_AGGRESSIVE` |

This boundary is deliberate. Earlier MCC revisions advertised AST passes for
constant/copy propagation, CSE, DCE/DSE, LICM, inlining, tail calls, loop
unrolling, and vectorization. Several were analysis-only no-ops; others lacked
the alias, volatile, width/signedness, sequencing, scope, or CFG proofs needed
for a correct C transformation. They have been removed from the API, build,
and pipelines rather than reported as optimizations.

New optimizations should normally be implemented in ANVIL IR or MIR with:

- explicit preconditions for integer width, signedness, overflow, and FP rules;
- conservative memory effects for pointers, calls, volatile, and atomics;
- SSA/CFG preservation and verifier coverage;
- executable differential regressions at every enabled optimization level.

An AST pass should only be reintroduced if it performs a complete
semantics-preserving transformation and has equivalent type, scope, alias,
sequencing, and control-flow guarantees.
