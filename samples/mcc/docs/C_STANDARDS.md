# MCC C mode and capability matrix

`-std=` selects syntax and feature gates; it is not a claim of complete ISO or
GNU conformance. This file and `src/c_std.c` are the authoritative capability
surface. A feature is listed as supported only when parsing, semantic analysis,
ANVIL IR lowering and a regression exist.

| Mode | Implemented surface beyond the previous mode |
|---|---|
| `c89`, `c90` | MCC baseline declarations, expressions, control flow, records, pointers, arrays and core preprocessing |
| `c99` | Selected tested features including `long long`, `_Bool`, mixed/for declarations, VLAs, line comments and variadic macros |
| `c11`, `c17` | `_Alignof`, `_Static_assert` with a message, `_Generic`, anonymous struct/union members |
| `c23` | `nullptr`, `typeof`, `typeof_unqual`, `bool`/`true`/`false`, binary literals, digit separators, `#elifdef`, `#elifndef`, `__VA_OPT__` |
| `gnu89`, `gnu99`, `gnu11` | `typeof`, statement expressions, case ranges, zero-length arrays and line comments on top of the corresponding subset |

Accepted aliases are `c2x`, `c18`, `gnu90`, `gnu9x` and `gnu1x`.

## Explicitly unsupported

The following are rejected or diagnosed, rather than parsed and discarded:

- C11 alignment specifiers (`_Alignas`/`alignas`), atomics, thread-local
  storage and noreturn semantics;
- C99 complex and imaginary arithmetic;
- bit-fields and ABI-compatible `long double`;
- `sizeof` on a VLA (retaining the declaration-time evaluated bound is not
  implemented; MCC never reevaluates it with incorrect side effects);
- C23 `[[attributes]]`, `constexpr`, inferred `auto`, `char8_t`/`u8` character
  literals, `#embed` and message-less `static_assert`;
- `_Pragma`, `#include_next`, GNU attributes, labels-as-values and computed
  `goto`.

`#pragma once` is implemented. Other `#pragma` directives are hard errors
because ignoring layout/ABI pragmas would silently miscompile the program.
Unsupported constructs live in `tests/negative` and `make
test-negative` requires every one of them to fail.

## Predefined macros

MCC defines `__STDC__`, the version appropriate to the selected mode and the
tested builtin macros. C11-or-newer modes define `__STDC_NO_ATOMICS__=1`,
`__STDC_NO_COMPLEX__=1` and `__STDC_NO_THREADS__=1`; MCC does not advertise
UTF-16/UTF-32 execution character support.

## Why modes are subsets

The sample is primarily an end-to-end stress frontend for ANVIL. Features are
added vertically. Syntax-only token skipping, ignored qualifiers and metadata
that never reaches IR are not considered implementations.
