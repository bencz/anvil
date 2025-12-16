# MCC Documentation

This directory contains detailed documentation for the MCC (Micro C Compiler) project, designed to help developers understand the compiler's architecture and implementation.

## Documentation Index

| Document | Description |
|----------|-------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | High-level overview of the compiler architecture |
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
| `mcc_context_t` | context.h | Compiler context, memory management |
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
| `lexer.c` | ~500 | Tokenization |
| `preprocessor.c` | ~1150 | Macro expansion, conditionals, includes |
| `parser.c` | ~1350 | Recursive descent parser (with typedef support) |
| `types.c` | ~570 | Type system |
| `sema.c` | ~750 | Type checking, symbol resolution |
| `codegen.c` | ~1100 | ANVIL IR generation |

### Test Files

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
1. Add token type in `token.h`
2. Add to keyword table in `lexer.c`
3. Handle in parser as needed

**Adding a new operator:**
1. Add token type in `token.h`
2. Add lexer recognition in `lexer.c`
3. Add to operator precedence in `parser.c`
4. Add type checking in `sema.c`
5. Add code generation in `codegen.c`

**Adding a new statement:**
1. Add AST node type in `ast.h`
2. Add parsing in `parser.c`
3. Add semantic analysis in `sema.c`
4. Add code generation in `codegen.c`

**Adding a typedef:**
- Typedefs are handled in the parser
- `parse_declaration()` registers typedef names in `parser->typedefs`
- `parse_type_specifier()` checks for typedef names via `is_typedef_name()`
- `parse_statement()` checks if identifier is typedef before parsing expression

**Debugging tips:**
- Use `-dump-ast` to see the parsed AST
- Check `ctx->error_count` after each phase
- Add debug prints in specific functions
- Test with minimal C programs first

### Current Limitations

- Bitfields
- Stringification (`#`) and token pasting (`##`) in macros
