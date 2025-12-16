# MCC Documentation

This directory contains detailed documentation for the MCC (Micro C Compiler) project, designed to help developers understand the compiler's architecture and implementation.

## Documentation Index

| Document | Description |
|----------|-------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | High-level overview of the compiler architecture |
| [C_STANDARDS.md](C_STANDARDS.md) | **C language standards (C89, C99, C11, etc.) and feature system** |
| [LEXER.md](LEXER.md) | Lexer/tokenizer documentation |
| [PREPROCESSOR.md](PREPROCESSOR.md) | C preprocessor documentation |
| [PARSER.md](PARSER.md) | Parser and AST documentation |
| [TYPES.md](TYPES.md) | Type system documentation |
| [SEMA.md](SEMA.md) | Semantic analysis documentation |
| [CODEGEN.md](CODEGEN.md) | Code generator and ANVIL integration |

## Quick Reference

### Compilation Pipeline

```
Source (.c) → Preprocessor → Lexer → Parser → Sema → Codegen → Assembly
```

### Multi-File Compilation

MCC supports compiling multiple source files into a single output:

```bash
# Compile multiple files
./mcc -o output.s file1.c file2.c file3.c

# With verbose output to see progress
./mcc -v -arch=x86_64 -o output.s main.c utils.c math.c

# Check syntax of multiple files
./mcc -fsyntax-only file1.c file2.c
```

All files share:
- A common symbol table (functions/variables visible across files)
- A single ANVIL module (all code in one output)
- Shared type context

### Key Data Structures

| Structure | File | Purpose |
|-----------|------|---------|
| `mcc_context_t` | mcc.h | Compiler context, memory management, effective features |
| `mcc_c_std_t` | c_std.h | C language standard enum (C89, C99, etc.) |
| `mcc_c_features_t` | c_std.h | Scalable feature set (256 features) |
| `mcc_feature_id_t` | c_std.h | Feature identifier enum |
| `mcc_token_t` | token.h | Token representation |
| `mcc_lexer_t` | lexer.h | Lexer state |
| `mcc_preprocessor_t` | preprocessor.h | Preprocessor state |
| `mcc_ast_node_t` | ast.h | AST node |
| `mcc_parser_t` | parser.h | Parser state |
| `mcc_type_t` | types.h | Type representation |
| `mcc_symtab_t` | symtab.h | Symbol table |
| `mcc_sema_t` | sema.h | Semantic analyzer |
| `mcc_codegen_t` | codegen.h | Code generator |

### Source Files Overview

| File | Lines | Description |
|------|-------|-------------|
| `c_std.c` | ~500 | C language standards and feature system |
| `context.c` | ~280 | Compiler context, feature checking |
| `types.c` | ~590 | Type system (includes C99 long long) |

**Modular Components:**

| Directory | Files | Description |
|-----------|-------|-----------|
| `src/lexer/` | 9 files | Modular lexer with C standard support |
| `src/preprocessor/` | 6 files | Modular preprocessor with C standard support |
| `src/parser/` | 6 files | Modular parser with C standard support |
| `src/sema/` | 6 files | Modular semantic analyzer with C standard support |
| `src/codegen/` | 5 files | Modular code generator with ANVIL integration |

### Test Suite

Tests are organized by C standard in the `tests/` directory:

| Directory | Standard | Description |
|-----------|----------|-----------|
| `tests/c89/` | C89 | Basic types, control flow, operators, functions, structs, typedef, preprocessor |
| `tests/c99/` | C99 | Comments, declarations, literals, preprocessor, types |
| `tests/c11/` | C11 | Anonymous structs, `_Generic`, keywords, `_Static_assert` |
| `tests/c23/` | C23 | Attributes, keywords, literals, `typeof` |
| `tests/gnu/` | GNU | GNU extensions (in progress) |
| `tests/cross/` | Cross | Cross-standard tests (C99 in C89, C11 in C99, etc.) |

**Running Tests:**

The build system uses modular Makefiles in `mk/` for organized test targets:

```bash
# Syntax Tests (using -fsyntax-only)
make test              # Run all syntax tests
make test-syntax-c89   # Run C89 syntax tests only
make test-syntax-c99   # Run C99 syntax tests only
make test-syntax-c11   # Run C11 syntax tests only
make test-syntax-c23   # Run C23 syntax tests only
make test-syntax-gnu   # Run GNU extension tests
make test-quick        # Quick test (C89 only)

# Code Generation Tests
make test-codegen           # Basic code generation tests
make test-codegen-x86_64    # x86_64 code generation
make test-codegen-arm64     # ARM64 code generation
make test-codegen-arm64-macos  # ARM64 macOS code generation
make test-codegen-s370      # S/370 code generation
make test-codegen-multifile # Multi-file compilation
make test-codegen-all-arch  # All architectures

# Cross-Standard Tests
make test-cross        # Verify warnings for features in older standards

# Combined
make test-all          # Run all tests (syntax + codegen + cross)

# Help
make help              # Show all available targets
```

**Cross-Standard Tests:**

| Test | Description |
|------|-------------|
| `c99_in_c89.c` | C99 features compiled with `-std=c89` (should warn) |
| `c11_in_c99.c` | C11 features compiled with `-std=c99` (should warn) |
| `c11_in_c89.c` | C11 features compiled with `-std=c89` (should warn) |
| `c23_in_c11.c` | C23 features compiled with `-std=c11` (should warn) |
| `c23_in_c89.c` | C23 features compiled with `-std=c89` (should warn) |

**Legacy Tests:**

| Test | Description |
|------|-------------|
| `typedef_simple.c` | Basic typedef (`typedef int INT;`) |
| `typedef_multi.c` | Multiple typedef names (`typedef int A, *PA;`) |
| `advanced_types.c` | Typedef, nested structs, unions, arrays in structs |
| `struct_test.c` | Complex struct with nested structs |
| `preprocessor_test.c` | `#if`, `#elif`, `#else`, `#define`, macros |
| `bitwise.c` | Bitwise operations (`&`, `|`, `^`, `~`, `<<`, `>>`) |
| `simple_struct.c` | Basic struct with member access |
| `multi_file/` | Multi-file compilation tests |

### C Language Standards

MCC supports multiple C language standards via the `-std=` flag:

```bash
./mcc -std=c89 -o output.s input.c   # ANSI C89 (default)
./mcc -std=c99 -o output.s input.c   # ISO C99
./mcc -std=gnu99 -o output.s input.c # GNU dialect of C99
```

See [C_STANDARDS.md](C_STANDARDS.md) for complete documentation of:
- Supported standards (C89, C90, C99, C11, C17, C23, GNU variants)
- Feature differences between standards
- 256-feature scalable feature system
- Predefined macros per standard

### Supported C89 Features

**Types:**
- Basic: `void`, `char`, `short`, `int`, `long`, `float`, `double`
- Qualifiers: `const`, `volatile`
- Storage: `auto`, `register`, `static`, `extern`, `typedef`
- Derived: pointers, arrays, structs, unions, enums, functions
- `typedef` for type aliases
- Nested structs and unions
- Struct/union with embedded anonymous unions/structs

**Statements:**
- Control: `if`, `else`, `switch`, `case`, `default`
- Loops: `while`, `do`, `for`
- Jumps: `break`, `continue`, `return`, `goto`
- Labels: named labels, `case`, `default`

**Expressions:**
- All C89 operators with correct precedence
- Function calls
- Array subscripting
- Struct/union member access (`.` and `->`)
- Type casts
- `sizeof`

**Preprocessor:**
- `#define`, `#undef`
- `#include`
- `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`
- `#error`, `#warning`, `#line`, `#pragma`
- `defined()` operator
- Arithmetic and logical expressions

### Target Architectures

| Architecture | Output | Calling Convention |
|--------------|--------|-------------------|
| S/370 | HLASM | MVS linkage |
| S/370-XA | HLASM | MVS linkage |
| S/390 | HLASM | MVS linkage |
| z/Architecture | HLASM | MVS linkage |
| x86 | AT&T | cdecl |
| x86-64 | AT&T | System V AMD64 |
| PowerPC | GAS | AIX/ELF |
| ARM64 | GAS | AAPCS64 (Linux) |
| ARM64 macOS | GAS | Darwin ABI (Apple Silicon) |

## For Assistants

When working with MCC code:

1. **Understanding the flow**: Start with ARCHITECTURE.md for the big picture
2. **Modifying parsing**: See PARSER.md for AST structure and grammar
3. **Adding operators**: Follow the chain: lexer → parser → sema → codegen
4. **Type issues**: Check TYPES.md and SEMA.md
5. **Code generation**: See CODEGEN.md for ANVIL API usage

### Common Tasks

**Adding a new keyword:**
1. Add token type in `include/token.h`
2. Add to keyword table in `src/lexer/lex_ident.c` with required feature
3. Add token name in `src/lexer/lex_token.c`
4. Handle in parser as needed

**Adding a new operator:**
1. Add token type in `include/token.h`
2. Add lexer recognition in `src/lexer/lex_operator.c`
3. Add to operator precedence in `src/parser/parse_expr.c`
4. Add type checking in `src/sema/sema_expr.c`
5. Add code generation in `src/codegen/codegen_expr.c`

**Adding a new statement:**
1. Add AST node type in `include/ast.h`
2. Add parsing in `src/parser/parse_stmt.c`
3. Add semantic analysis in `src/sema/sema_stmt.c`
4. Add code generation in `src/codegen/codegen_stmt.c`

**Adding a new expression:**
1. Add AST node type in `include/ast.h`
2. Add parsing in `src/parser/parse_expr.c`
3. Add semantic analysis in `src/sema/sema_expr.c`
4. Add code generation in `src/codegen/codegen_expr.c`

**Adding a new type:**
1. Add type kind in `include/types.h`
2. Add parsing in `src/parser/parse_type.c`
3. Add type creation in `src/types.c`
4. Add semantic analysis in `src/sema/sema_type.c`
5. Add code generation in `src/codegen/codegen_type.c`

**Adding a typedef:**
- Typedefs are handled in the parser
- `parse_declaration()` in `src/parser/parse_decl.c` registers typedef names
- `parse_type_specifier()` in `src/parser/parse_type.c` checks for typedef names
- `parse_statement()` in `src/parser/parse_stmt.c` checks if identifier is typedef

**Debugging tips:**
- Use `-dump-ast` to see the parsed AST
- Check `ctx->error_count` after each phase
- Add debug prints in specific functions
- Test with minimal C programs first

### Current Limitations

- Stringification (`#`) and token pasting (`##`) in macros
- Line continuation (`\`) in preprocessor

### Recent Improvements

- **Modular Semantic Analyzer**: Refactored `sema.c` into `src/sema/` with 6 modular files
- **C99 long long**: Added `mcc_type_llong()` and `mcc_type_ullong()` for 64-bit integer types
- **Multiple Typedef Names**: Support for `typedef int INT, *PINT, **PPINT;` syntax
- **Complex Declarators**: Full support for C declarator syntax including `int (*arr)[10]`, `int (*func)(int, int)`, etc.
- **Bitfields**: Parsing and storage of bitfield widths in struct fields
- **Enum Constants**: Proper storage of enum constant names and values
- **_Generic (C11)**: Full parsing with AST storage for type associations
- **_Static_assert (C11)**: Support in structs and at file scope
- **Thread Local Storage (C11)**: `_Thread_local` keyword support
- **C11 Keywords**: `_Alignas`, `_Alignof`, `_Atomic`, `_Noreturn` with proper parsing
- **C23 Attributes**: `[[deprecated]]`, `[[nodiscard]]`, `[[maybe_unused]]`, `[[noreturn]]`, `[[fallthrough]]` with AST storage
- **C23 typeof**: `typeof` and `typeof_unqual` type specifiers
- **C23 Literals**: Binary literals (`0b1010`), digit separators (`1'000'000`), `u8` character literals
- **Multiple Variable Declarations**: Full support for `int a, b, c;` with `AST_DECL_LIST` node
- **Cross-Standard Warnings**: Warnings when using features from newer standards in older modes
- **Organized Test Suite**: Tests organized by C standard (C89, C99, C11, C23, GNU) with cross-standard tests
- **Modular Code Generator**: Refactored `codegen.c` into `src/codegen/` with 5 modular files
- **Modular Build System**: Makefile split into `mk/` with separate config, rules, and test files
