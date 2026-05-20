# MCC - Micro C Compiler

MCC is a small C compiler sample built on ANVIL. Its main purpose in this repository is to stress the generic ANVIL pipeline: frontend-generated source IR, source verification, target-independent optimization, backend selection, and final assembly generation.

## Features

- **C89/ANSI C baseline** with selected C99/C11/C23/GNU extensions
- **Preprocessor** with full support for:
  - `#include` (local and system headers)
  - `#define` / `#undef` (object-like and function-like macros)
  - `#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif`
  - Conditional expressions with arithmetic and logical operators
  - `defined()` operator
  - `#pragma`, `#error`, `#warning`, `#line`
  - Stringification (`#`), token pasting (`##`), variadic macros, and `__VA_OPT__`
- **C type system and semantic analysis**:
  - Basic integer/floating types, including `_Bool` and `long long`
  - Type qualifiers: `const`, `volatile`
  - Storage classes: `auto`, `register`, `static`, `extern`, `typedef`
  - Derived types: pointers, arrays, structs, unions, enums, functions
  - `typedef` with multiple names (`typedef int INT, *PINT;`)
  - Nested structs and unions
  - Arrays in struct fields (`int data[10]`) and local VLAs through ANVIL dynamic alloca
  - Struct/union with embedded anonymous unions/structs
- **Code generation through ANVIL**:
  - MCC emits architecture-independent ANVIL source IR.
  - ANVIL verifies and optimizes that IR before invoking a selected backend.
  - Any registered ANVIL backend can be selected with `-arch=...`.
  - The executable stress suite currently validates the most complete runtime path available on the host.

## Building

```bash
# Build MCC (requires ANVIL library)
make

# Clean and rebuild
make clean && make
```

## Usage

```bash
# Compile C source to assembly (default: x86-64)
./mcc -o output.s input.c

# Compile multiple files into a single output
./mcc -o output.s main.c utils.c math.c

# With verbose output to see progress
./mcc -v -o output.s file1.c file2.c file3.c

# Specify target architecture through ANVIL
./mcc -arch=s370 -o output.asm input.c      # IBM S/370
./mcc -arch=s390 -o output.asm input.c      # IBM S/390
./mcc -arch=zarch -o output.asm input.c     # z/Architecture
./mcc -arch=x86 -o output.s input.c         # x86 32-bit
./mcc -arch=x86_64 -o output.s input.c      # x86-64
./mcc -arch=arm64 -o output.s input.c       # ARM64 (Linux)
./mcc -arch=arm64_macos -o output.s input.c # ARM64 (Apple Silicon/macOS)

# Add include paths
./mcc -Iincludes -o output.asm input.c

# Set optimization level
./mcc -O2 -o output.asm input.c

# Check syntax only (multiple files supported)
./mcc -fsyntax-only file1.c file2.c

# Dump AST (for debugging)
./mcc -ast-dump input.c

# Dump ANVIL IR before backend code generation
./mcc -dump-ir -o output.s input.c

# Compile, assemble/link, execute, and compare against the native compiler
make test-exec
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
│  (AST → ANVIL Source IR)                                    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                         ANVIL                                │
│  (Verifier → Optimizer → selected backend → assembly)       │
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
│   ├── lexer/          # Modular lexer
│   ├── preprocessor/   # Modular preprocessor
│   ├── parser/         # Recursive descent parser
│   ├── ast/            # AST utilities and dumps
│   ├── types.c         # Type system implementation
│   ├── symtab.c        # Symbol table implementation
│   ├── sema/           # Modular semantic analysis
│   ├── opt/            # AST optimizer
│   └── codegen/        # ANVIL code generator
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
│   ├── multi_file/         # Multi-file compilation tests
│   │   ├── main_test.c     # Main file with external function calls
│   │   ├── math_funcs.c    # Math functions (add, subtract, multiply, square)
│   │   └── utils.c         # Utility functions (abs_val, max, min)
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
- Selected C99/C11/C23/GNU syntax used by the test suite, including `long long`, `_Bool`, C99 for-loop declarations, VLAs, `_Generic`, `_Static_assert`, `typeof`, and statement expressions

### Current Limitations

- Full `_Complex`/`_Imaginary` code generation
- Designated initializers
- Compound literals
- Complete bitfield layout/codegen semantics
- Full external C runtime implementation; bundled headers mostly provide declarations

## License

Same as ANVIL (Unlicense)
