# MCC - Micro C Compiler

A minimal C89 compiler implementation using ANVIL for code generation. MCC demonstrates how to build a complete compiler frontend that targets multiple architectures through the ANVIL intermediate representation.

## Features

- **C89/ANSI C compliant** (with some extensions)
- **Preprocessor** with full support for:
  - `#include` (local and system headers)
  - `#define` / `#undef` (object-like and function-like macros)
  - `#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif`
  - Conditional expressions with arithmetic and logical operators
  - `defined()` operator
  - `#pragma`, `#error`, `#warning`, `#line`
- **Full C89 type system**:
  - Basic types: `char`, `short`, `int`, `long`, `float`, `double`
  - Type qualifiers: `const`, `volatile`
  - Storage classes: `auto`, `register`, `static`, `extern`, `typedef`
  - Derived types: pointers, arrays, structs, unions, enums, functions
  - `typedef` with multiple names (`typedef int INT, *PINT;`)
  - Nested structs and unions
  - Arrays in struct fields (`int data[10]`)
  - Struct/union with embedded anonymous unions/structs
- **Code generation** via ANVIL IR for multiple architectures:
  - IBM S/370, S/370-XA, S/390, z/Architecture (HLASM output)
  - x86, x86-64 (AT&T syntax)
  - PowerPC 32/64-bit

## Building

```bash
# Build MCC (requires ANVIL library)
make

# Clean and rebuild
make clean && make
```

## Usage

```bash
# Compile C source to assembly (default: S/390)
./mcc -o output.asm input.c

# Specify target architecture
./mcc -arch=s370 -o output.asm input.c      # IBM S/370
./mcc -arch=s390 -o output.asm input.c      # IBM S/390
./mcc -arch=zarch -o output.asm input.c     # z/Architecture
./mcc -arch=x86 -o output.s input.c         # x86 32-bit
./mcc -arch=x86_64 -o output.s input.c      # x86-64

# Add include paths
./mcc -Iincludes -o output.asm input.c

# Set optimization level
./mcc -O2 -o output.asm input.c

# Dump AST (for debugging)
./mcc -dump-ast input.c
```

## Standard Library Headers

MCC includes a minimal set of C89 standard library headers in the `includes/` directory:

| Header | Description |
|--------|-------------|
| `stddef.h` | Common definitions (`NULL`, `size_t`, `ptrdiff_t`, `offsetof`) |
| `stdarg.h` | Variable argument handling (`va_list`, `va_start`, `va_arg`, `va_end`) |
| `limits.h` | Implementation limits (`INT_MAX`, `CHAR_BIT`, etc.) |
| `float.h` | Floating-point characteristics |
| `stdio.h` | Standard I/O (declarations only) |
| `stdlib.h` | General utilities (declarations only) |
| `string.h` | String handling (declarations only) |
| `ctype.h` | Character classification |
| `math.h` | Mathematics (declarations only) |
| `errno.h` | Error codes |
| `assert.h` | Diagnostics |
| `time.h` | Date and time |
| `signal.h` | Signal handling |
| `setjmp.h` | Non-local jumps |

**Note:** These headers provide declarations only. The actual implementations must be provided by a runtime library.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Source Code (.c)                        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      Preprocessor                            │
│  (#include, #define, #ifdef, macro expansion)               │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                         Lexer                                │
│  (Tokenization: identifiers, keywords, literals, operators) │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                         Parser                               │
│  (Recursive descent, builds AST)                            │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Semantic Analysis                         │
│  (Type checking, symbol resolution, scope management)       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Code Generator                            │
│  (AST → ANVIL IR → Target Assembly)                         │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Assembly Output (.s)                      │
└─────────────────────────────────────────────────────────────┘
```

## Project Structure

```
mcc/
├── include/            # Header files
│   ├── mcc.h           # Main header (includes all others)
│   ├── lexer.h         # Lexer interface
│   ├── token.h         # Token definitions (100+ token types)
│   ├── preprocessor.h  # Preprocessor interface
│   ├── ast.h           # AST node definitions (40+ node types)
│   ├── parser.h        # Parser interface
│   ├── types.h         # C type system
│   ├── symtab.h        # Symbol table
│   ├── sema.h          # Semantic analysis
│   └── codegen.h       # Code generator interface
├── src/                # Implementation files
│   ├── main.c          # Entry point, command-line parsing
│   ├── context.c       # Compiler context, memory management
│   ├── lexer.c         # Lexer implementation (~500 lines)
│   ├── preprocessor.c  # Preprocessor implementation (~1100 lines)
│   ├── parser.c        # Recursive descent parser (~1200 lines)
│   ├── ast.c           # AST utilities
│   ├── types.c         # Type system implementation (~570 lines)
│   ├── symtab.c        # Symbol table implementation
│   ├── sema.c          # Semantic analysis (~750 lines)
│   └── codegen.c       # ANVIL code generator (~1100 lines)
├── includes/           # C standard library headers
│   ├── stdio.h, stdlib.h, string.h, ...
├── tests/              # Test programs
│   ├── simple_struct.c     # Basic struct test
│   ├── struct_test.c       # Complex struct test
│   ├── bitwise.c           # Bitwise operations test
│   ├── preprocessor_test.c # Preprocessor test
│   ├── typedef_simple.c    # Basic typedef test
│   ├── typedef_multi.c     # Multiple typedef names test
│   ├── advanced_types.c    # Advanced types (typedef, nested structs, unions)
│   └── ...
├── docs/               # Documentation
│   ├── ARCHITECTURE.md # Detailed architecture
│   ├── LEXER.md        # Lexer documentation
│   ├── PARSER.md       # Parser documentation
│   ├── CODEGEN.md      # Code generator documentation
│   └── ...
├── Makefile
└── README.md
```

## C89 Language Support

### Supported

- All C89 declarations and statements
- All C89 expressions and operators
- Function definitions and prototypes
- Structs, unions, enums
- Nested structs and unions
- `typedef` for type aliases
- Pointers and arrays
- Type casts
- Preprocessor directives (`#if`, `#elif`, `#else`, `#endif`, `#ifdef`, `#ifndef`, `#define`, `#include`)

### Not Supported (C99+ features)

- `//` comments (use `/* */`)
- Variable-length arrays (VLA)
- `_Bool`, `_Complex`, `_Imaginary`
- Designated initializers
- Compound literals
- `inline` functions
- `restrict` qualifier
- `long long` type

### Current Limitations

- Bitfields
- Stringification (`#`) and token pasting (`##`) in macros

## License

Same as ANVIL (Unlicense)
