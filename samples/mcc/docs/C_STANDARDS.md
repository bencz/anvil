# MCC C Language Standards Documentation

This document describes the C language standards supported by MCC and their differences.

## Supported Standards

| Standard | Flag | Description | Aliases |
|----------|------|-------------|---------|
| C89 | `-std=c89` | ANSI C (X3.159-1989) | |
| C90 | `-std=c90` | ISO C (ISO/IEC 9899:1990) | `iso9899:1990` |
| C99 | `-std=c99` | ISO C99 (ISO/IEC 9899:1999) | `c9x`, `iso9899:1999` |
| C11 | `-std=c11` | ISO C11 (ISO/IEC 9899:2011) | `c1x`, `iso9899:2011` |
| C17 | `-std=c17` | ISO C17 (ISO/IEC 9899:2018) | `c18`, `iso9899:2017` |
| C23 | `-std=c23` | ISO C23 (ISO/IEC 9899:2024) | `c2x` |
| GNU89 | `-std=gnu89` | GNU dialect of C89 | `gnu90` |
| GNU99 | `-std=gnu99` | GNU dialect of C99 | `gnu9x` |
| GNU11 | `-std=gnu11` | GNU dialect of C11 | `gnu1x` |

## C89/C90 Features (Baseline)

C89 and C90 are essentially the same standard - C89 is the ANSI version, C90 is the ISO version.

### Types
- Basic types: `void`, `char`, `short`, `int`, `long`, `float`, `double`
- Type qualifiers: `const`, `volatile`
- Type modifiers: `signed`, `unsigned`
- Derived types: pointers, arrays, structs, unions, enums
- `typedef` for type aliases

### Control Flow
- `if`/`else` statements
- `switch`/`case`/`default`
- `while`, `do-while`, `for` loops
- `goto` and labels
- `break`, `continue`, `return`

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`
- Logical: `&&`, `||`, `!`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Assignment: `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- Increment/Decrement: `++`, `--`
- Ternary: `?:`
- Comma: `,`
- `sizeof`
- Type casts
- Address-of `&` and dereference `*`

### Preprocessor
- `#define`, `#undef`
- `#include`
- `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`
- `#error`, `#pragma`, `#line`
- `defined()` operator

### Other
- Function prototypes
- Variadic functions (`...`)
- String and character literals
- Block comments `/* */`

## C99 New Features

C99 adds the following features to C89/C90:

### New Types

| Feature | Description | Example |
|---------|-------------|---------|
| `long long` | 64-bit integer type | `long long x = 123456789012LL;` |
| `_Bool` | Boolean type | `_Bool flag = 1;` |
| `_Complex` | Complex number types | `double _Complex z;` |
| `_Imaginary` | Imaginary number types | `double _Imaginary i;` |

### New Type Qualifiers

| Feature | Description | Example |
|---------|-------------|---------|
| `restrict` | Pointer aliasing hint | `void f(int *restrict p)` |
| `inline` | Inline function hint | `inline int square(int x) { return x*x; }` |

### Declaration Changes

| Feature | Description | Example |
|---------|-------------|---------|
| Mixed declarations | Declarations anywhere in block | `int x = 1; x++; int y = 2;` |
| For-loop declarations | Declare in for init | `for (int i = 0; i < n; i++)` |
| Variable Length Arrays | Runtime-sized arrays | `int arr[n];` |
| Flexible array members | Struct trailing array | `struct S { int n; int data[]; };` |
| Designated initializers | Named initializers | `int arr[10] = { [5] = 42 };` |
| Compound literals | Anonymous aggregates | `(int[]){1, 2, 3}` |

### Preprocessor Additions

| Feature | Description | Example |
|---------|-------------|---------|
| Variadic macros | `__VA_ARGS__` | `#define LOG(fmt, ...) printf(fmt, __VA_ARGS__)` |
| `_Pragma` operator | Pragma in macros | `_Pragma("once")` |
| Empty macro arguments | Allow empty args | `MACRO(,x)` |

### Other C99 Features

| Feature | Description | Example |
|---------|-------------|---------|
| `//` comments | Line comments | `// comment` |
| `__func__` | Function name | `printf("%s\n", __func__);` |
| Universal characters | Unicode escapes | `char *s = "\u00E9";` |
| Hex float literals | Hex floating point | `double x = 0x1.0p10;` |

## GNU Extensions

GNU extensions add features beyond the ISO standards:

| Feature | Description |
|---------|-------------|
| `//` comments in C89 | Line comments available in gnu89 |
| `__attribute__` | Compiler attributes |
| Inline assembly | `asm` statements |
| Statement expressions | `({ ... })` |
| Typeof | `typeof(expr)` |
| Zero-length arrays | `int arr[0];` |
| Case ranges | `case 1 ... 5:` |

## Predefined Macros

### C89/C90
```c
__STDC__        /* Always 1 */
```

### C99
```c
__STDC__            /* Always 1 */
__STDC_VERSION__    /* 199901L */
__STDC_HOSTED__     /* 1 for hosted implementation */
```

### C11
```c
__STDC__            /* Always 1 */
__STDC_VERSION__    /* 201112L */
__STDC_HOSTED__     /* 1 for hosted implementation */
__STDC_UTF_16__     /* 1 if char16_t is UTF-16 */
__STDC_UTF_32__     /* 1 if char32_t is UTF-32 */
```

### C17
```c
__STDC__            /* Always 1 */
__STDC_VERSION__    /* 201710L */
__STDC_HOSTED__     /* 1 for hosted implementation */
```

### C23
```c
__STDC__            /* Always 1 */
__STDC_VERSION__    /* 202311L */
__STDC_HOSTED__     /* 1 for hosted implementation */
```

## Feature System

MCC uses a scalable feature flag system to track which language features are available. The system supports **256 features** organized into 4 words of 64 bits each, allowing fine-grained control over language features.

### Checking Features in Code

```c
/* In MCC source code - use mcc_feature_id_t enum values */
if (mcc_ctx_has_feature(ctx, MCC_FEAT_LONG_LONG)) {
    /* Handle long long type */
}

if (mcc_ctx_has_feature(ctx, MCC_FEAT_LINE_COMMENT)) {
    /* Allow // comments */
}

if (mcc_ctx_has_feature(ctx, MCC_FEAT_MIXED_DECL)) {
    /* Allow declarations after statements */
}

/* Enable/disable features at runtime */
mcc_ctx_enable_feature(ctx, MCC_FEAT_LINE_COMMENT);
mcc_ctx_disable_feature(ctx, MCC_FEAT_VLA);
```

### Feature Organization

Features are organized into 4 words (256 total features):

| Word | Range | Contents |
|------|-------|----------|
| 0 | 0-63 | C89 types, control flow, basic operators |
| 1 | 64-127 | C89 operators, preprocessor, C99 types |
| 2 | 128-191 | C99 declarations, preprocessor, C11 features |
| 3 | 192-255 | C17, C23, GNU extensions |

### Feature Categories

**C89 Types (Word 0: 0-23):**
- `MCC_FEAT_BASIC_TYPES` - char, short, int, long, float, double
- `MCC_FEAT_POINTERS` - Pointer types
- `MCC_FEAT_ARRAYS` - Array types
- `MCC_FEAT_STRUCTS` - struct types
- `MCC_FEAT_UNIONS` - union types
- `MCC_FEAT_ENUMS` - enum types
- `MCC_FEAT_TYPEDEF` - typedef declarations
- `MCC_FEAT_CONST` - const qualifier
- `MCC_FEAT_VOLATILE` - volatile qualifier
- `MCC_FEAT_BITFIELDS` - Bit-field members

**C89 Control Flow (Word 0: 24-39):**
- `MCC_FEAT_IF_ELSE` - if/else statements
- `MCC_FEAT_SWITCH`, `MCC_FEAT_CASE`, `MCC_FEAT_DEFAULT` - switch statements
- `MCC_FEAT_WHILE`, `MCC_FEAT_DO_WHILE`, `MCC_FEAT_FOR` - loops
- `MCC_FEAT_GOTO`, `MCC_FEAT_LABELS` - goto and labels
- `MCC_FEAT_BREAK`, `MCC_FEAT_CONTINUE`, `MCC_FEAT_RETURN` - control transfer

**C89 Operators (Word 0-1: 40-79):**
- `MCC_FEAT_OP_ADD`, `MCC_FEAT_OP_SUB`, `MCC_FEAT_OP_MUL`, `MCC_FEAT_OP_DIV`, `MCC_FEAT_OP_MOD`
- `MCC_FEAT_OP_AND`, `MCC_FEAT_OP_OR`, `MCC_FEAT_OP_XOR`, `MCC_FEAT_OP_NOT`
- `MCC_FEAT_OP_LOG_AND`, `MCC_FEAT_OP_LOG_OR`, `MCC_FEAT_OP_LOG_NOT`
- `MCC_FEAT_OP_EQ`, `MCC_FEAT_OP_NE`, `MCC_FEAT_OP_LT`, `MCC_FEAT_OP_GT`, `MCC_FEAT_OP_LE`, `MCC_FEAT_OP_GE`
- `MCC_FEAT_OP_ASSIGN`, `MCC_FEAT_OP_COMPOUND_ASSIGN`
- `MCC_FEAT_OP_INC`, `MCC_FEAT_OP_DEC`
- `MCC_FEAT_OP_TERNARY`, `MCC_FEAT_OP_COMMA`, `MCC_FEAT_OP_SIZEOF`, `MCC_FEAT_OP_CAST`
- `MCC_FEAT_OP_ADDR`, `MCC_FEAT_OP_DEREF`, `MCC_FEAT_OP_MEMBER`, `MCC_FEAT_OP_ARROW`

**C89 Preprocessor (Word 1: 80-103):**
- `MCC_FEAT_PP_DEFINE`, `MCC_FEAT_PP_UNDEF` - macro definition
- `MCC_FEAT_PP_INCLUDE` - file inclusion
- `MCC_FEAT_PP_IF`, `MCC_FEAT_PP_IFDEF`, `MCC_FEAT_PP_IFNDEF`, `MCC_FEAT_PP_ELIF`, `MCC_FEAT_PP_ELSE`, `MCC_FEAT_PP_ENDIF`
- `MCC_FEAT_PP_ERROR`, `MCC_FEAT_PP_PRAGMA`, `MCC_FEAT_PP_LINE`
- `MCC_FEAT_PP_DEFINED`, `MCC_FEAT_PP_STRINGIFY`, `MCC_FEAT_PP_CONCAT`

**C99 Types (Word 1: 120-127):**
- `MCC_FEAT_LONG_LONG` - long long int
- `MCC_FEAT_BOOL` - _Bool type
- `MCC_FEAT_COMPLEX` - _Complex type
- `MCC_FEAT_IMAGINARY` - _Imaginary type
- `MCC_FEAT_RESTRICT` - restrict qualifier
- `MCC_FEAT_INLINE` - inline functions

**C99 Declarations (Word 2: 128-143):**
- `MCC_FEAT_MIXED_DECL` - Mixed declarations and code
- `MCC_FEAT_FOR_DECL` - Declarations in for loop init
- `MCC_FEAT_VLA` - Variable Length Arrays
- `MCC_FEAT_FLEXIBLE_ARRAY` - Flexible array members
- `MCC_FEAT_DESIGNATED_INIT` - Designated initializers
- `MCC_FEAT_COMPOUND_LIT` - Compound literals

**C99 Preprocessor (Word 2: 144-159):**
- `MCC_FEAT_PP_VARIADIC` - Variadic macros
- `MCC_FEAT_PP_VA_ARGS` - __VA_ARGS__ identifier
- `MCC_FEAT_PP_PRAGMA_OP` - _Pragma operator
- `MCC_FEAT_PP_EMPTY_ARGS` - Empty macro arguments

**C99 Other (Word 2: 160-175):**
- `MCC_FEAT_LINE_COMMENT` - // comments
- `MCC_FEAT_FUNC_NAME` - __func__ predefined identifier
- `MCC_FEAT_UNIVERSAL_CHAR` - Universal character names \uXXXX
- `MCC_FEAT_HEX_FLOAT` - Hexadecimal floating constants

**C11 Features (Word 2: 176-191):**
- `MCC_FEAT_ALIGNAS`, `MCC_FEAT_ALIGNOF` - Alignment specifiers
- `MCC_FEAT_NORETURN` - _Noreturn function specifier
- `MCC_FEAT_STATIC_ASSERT` - _Static_assert
- `MCC_FEAT_GENERIC` - _Generic selection
- `MCC_FEAT_ATOMIC` - _Atomic type qualifier
- `MCC_FEAT_THREAD_LOCAL` - _Thread_local storage class
- `MCC_FEAT_ANONYMOUS_STRUCT` - Anonymous structs/unions

**C23 Features (Word 3: 200-223):**
- `MCC_FEAT_NULLPTR` - nullptr constant
- `MCC_FEAT_CONSTEXPR` - constexpr specifier
- `MCC_FEAT_TYPEOF`, `MCC_FEAT_TYPEOF_UNQUAL` - typeof operators
- `MCC_FEAT_AUTO_TYPE` - auto type inference
- `MCC_FEAT_BOOL_KEYWORD` - bool as keyword
- `MCC_FEAT_BINARY_LIT` - 0b binary literals
- `MCC_FEAT_DIGIT_SEP` - ' digit separator
- `MCC_FEAT_ATTR_SYNTAX` - [[attribute]] syntax

**GNU Extensions (Word 3: 224-255):**
- `MCC_FEAT_GNU_EXT` - General GNU extensions flag
- `MCC_FEAT_GNU_ASM` - GNU inline assembly
- `MCC_FEAT_GNU_ATTR` - __attribute__
- `MCC_FEAT_GNU_TYPEOF` - __typeof__
- `MCC_FEAT_GNU_STMT_EXPR` - Statement expressions ({ })
- `MCC_FEAT_GNU_LABEL_ADDR` - Labels as values &&
- `MCC_FEAT_GNU_CASE_RANGE` - case 1 ... 5:
- `MCC_FEAT_GNU_ZERO_ARRAY` - Zero-length arrays
- `MCC_FEAT_GNU_BUILTIN` - __builtin_* functions

## Usage Examples

### Compile with C89
```bash
./mcc -std=c89 -o output.s input.c
```

### Compile with C99
```bash
./mcc -std=c99 -o output.s input.c
```

### Compile with GNU extensions
```bash
./mcc -std=gnu99 -o output.s input.c
```

### Check current standard
```bash
./mcc -v -std=c99 input.c
# Output: Using C standard: c99 (ISO/IEC 9899:1999)
```

## Implementation Status

### C89/C90 - Mostly Complete
- [x] Basic types
- [x] Pointers, arrays, structs, unions, enums
- [x] typedef
- [x] Control flow (if, while, for, switch, goto)
- [x] All operators
- [x] Preprocessor (#define, #include, #if, etc.)
- [x] Stringification operator (`#`)
- [x] String literal concatenation
- [ ] Bitfields (not yet implemented)
- [ ] Token pasting (`##`) (not yet implemented)

### C99 - In Progress
- [ ] `long long` type
- [ ] `_Bool` type
- [ ] `restrict` qualifier
- [ ] `inline` functions
- [ ] Mixed declarations and code
- [ ] For-loop declarations
- [ ] Variable Length Arrays
- [ ] Designated initializers
- [ ] Compound literals
- [ ] `//` line comments
- [ ] `__func__`
- [x] Variadic macros (`__VA_ARGS__`)
- [ ] Hexadecimal float literals

### C23 - Partial
- [x] `#elifdef` / `#elifndef` directives
- [x] `__VA_OPT__` for variadic macros
- [ ] `nullptr` constant
- [ ] `constexpr` specifier
- [ ] `typeof` / `typeof_unqual`
- [ ] Binary literals (`0b`)
- [ ] Digit separators (`'`)

### GNU Extensions - Planned
- [ ] `__attribute__`
- [ ] Inline assembly
- [ ] Statement expressions
- [ ] typeof

## Architecture

The C standard system is implemented similarly to ANVIL's CPU model system, with a scalable feature set supporting 256+ features.

### Data Structures

```c
/* Standard enum (like anvil_cpu_model_t) */
typedef enum {
    MCC_STD_DEFAULT,
    MCC_STD_C89,
    MCC_STD_C90,
    MCC_STD_C99,
    MCC_STD_C11,
    MCC_STD_C17,
    MCC_STD_C23,
    MCC_STD_GNU89,
    MCC_STD_GNU99,
    MCC_STD_GNU11,
    MCC_STD_COUNT
} mcc_c_std_t;

/* Scalable feature set - 4 words = 256 features */
#define MCC_FEATURE_WORDS 4

typedef struct mcc_c_features {
    uint64_t bits[MCC_FEATURE_WORDS];
} mcc_c_features_t;

/* Feature ID enum - identifies individual features */
typedef enum {
    MCC_FEAT_BASIC_TYPES = 0,
    MCC_FEAT_POINTERS,
    /* ... 256 features total ... */
    MCC_FEAT_COUNT = 256
} mcc_feature_id_t;

/* Standard info table (like cpu_model_table) */
static const mcc_c_std_info_t c_std_info_table[] = {
    { MCC_STD_C89, "c89", "ANSI C", 1989, "ANSI X3.159-1989", ... },
    { MCC_STD_C99, "c99", "ISO C99", 1999, "ISO/IEC 9899:1999", ... },
    /* ... */
};
```

### Feature Manipulation Macros

```c
/* Initialize features to zero */
MCC_FEATURES_INIT(f)

/* Set/clear/test a single feature */
MCC_FEATURES_SET(f, feat)
MCC_FEATURES_CLEAR(f, feat)
MCC_FEATURES_HAS(f, feat)

/* Copy features */
MCC_FEATURES_COPY(dst, src)
```

### Feature Set Operations (inline functions)

```c
/* Combine two feature sets (OR) */
void mcc_features_or(mcc_c_features_t *dst, const mcc_c_features_t *src);

/* Intersect two feature sets (AND) */
void mcc_features_and(mcc_c_features_t *dst, const mcc_c_features_t *src);

/* Remove features (AND NOT) */
void mcc_features_remove(mcc_c_features_t *dst, const mcc_c_features_t *src);

/* Check if all features in 'required' are present */
bool mcc_features_has_all(const mcc_c_features_t *features, const mcc_c_features_t *required);

/* Check if any feature in 'check' is present */
bool mcc_features_has_any(const mcc_c_features_t *features, const mcc_c_features_t *check);

/* Check if feature set is empty */
bool mcc_features_is_empty(const mcc_c_features_t *features);
```

### Key Files

| File | Description |
|------|-------------|
| `include/c_std.h` | C standard definitions, feature IDs, macros, inline functions |
| `src/c_std.c` | C standard implementation, feature initialization, lookup tables |
| `src/context.c` | Context integration, feature checking, runtime overrides |

### API Functions

```c
/* Get standard info */
const mcc_c_std_info_t *mcc_c_std_get_info(mcc_c_std_t std);

/* Parse standard name (e.g., "c99", "gnu89", "-std=c99") */
mcc_c_std_t mcc_c_std_from_name(const char *name);

/* Get standard name */
const char *mcc_c_std_get_name(mcc_c_std_t std);

/* Resolve DEFAULT to actual standard */
mcc_c_std_t mcc_c_std_resolve(mcc_c_std_t std);

/* Get features for a standard */
void mcc_c_std_get_features(mcc_c_std_t std, mcc_c_features_t *features);

/* Check if a feature is available in a standard */
bool mcc_c_std_has_feature(mcc_c_std_t std, mcc_feature_id_t feature);

/* Get base standard (e.g., GNU99 -> C99) */
mcc_c_std_t mcc_c_std_get_base(mcc_c_std_t std);

/* Check if standard is a GNU variant */
bool mcc_c_std_is_gnu(mcc_c_std_t std);

/* Compare standards by year */
int mcc_c_std_compare(mcc_c_std_t a, mcc_c_std_t b);

/* Get predefined macros for a standard */
const mcc_predefined_macro_t *mcc_c_std_get_predefined_macros(mcc_c_std_t std, size_t *count);
```

### Context API

```c
/* Check if feature is enabled in context */
bool mcc_ctx_has_feature(mcc_context_t *ctx, mcc_feature_id_t feature);

/* Get current standard */
mcc_c_std_t mcc_ctx_get_std(mcc_context_t *ctx);

/* Get standard name */
const char *mcc_ctx_get_std_name(mcc_context_t *ctx);

/* Enable/disable features at runtime */
void mcc_ctx_enable_feature(mcc_context_t *ctx, mcc_feature_id_t feature);
void mcc_ctx_disable_feature(mcc_context_t *ctx, mcc_feature_id_t feature);
```

### Implementation Status

**C99 Features:**

| Feature | Status | Notes |
|---------|--------|-------|
| `long long` | ✅ Implemented | 64-bit integer type |
| `_Bool` | ✅ Implemented | Boolean type |
| `restrict` | ✅ Implemented | Pointer aliasing hint |
| `inline` | ✅ Implemented | Inline functions |
| Mixed declarations | ✅ Implemented | Declarations after statements |
| `for` loop declarations | ✅ Implemented | `for (int i = 0; ...)` |
| Compound literals | ✅ Implemented | `(int[]){1, 2, 3}` |
| Designated initializers | ✅ Implemented | `.field = value` |
| Flexible array members | ✅ Implemented | `struct { int n; char data[]; }` |
| VLA | ⚠️ Parsing only | Variable Length Arrays |
| Hex floats | ✅ Implemented | `0x1.0p10` |
| `//` comments | ✅ Implemented | Line comments |

**C11 Features:**

| Feature | Status | Notes |
|---------|--------|-------|
| `_Alignas` | ✅ Implemented | Alignment specifier with AST storage |
| `_Alignof` | ✅ Implemented | Alignment query (supports expressions) |
| `_Static_assert` | ✅ Implemented | Compile-time assertions |
| `_Generic` | ✅ Implemented | Type-generic selection |
| `_Noreturn` | ✅ Implemented | No-return function specifier |
| `_Thread_local` | ✅ Implemented | Thread-local storage |
| `_Atomic` | ✅ Implemented | Atomic type qualifier |
| Anonymous structs/unions | ✅ Implemented | Unnamed struct/union members |

**C23 Features:**

| Feature | Status | Notes |
|---------|--------|-------|
| `[[attributes]]` | ✅ Implemented | Standard attributes with AST storage |
| `[[deprecated]]` | ✅ Implemented | Deprecation attribute |
| `[[nodiscard]]` | ✅ Implemented | Must-use return value |
| `[[maybe_unused]]` | ✅ Implemented | Suppress unused warnings |
| `[[noreturn]]` | ✅ Implemented | No-return attribute |
| `[[fallthrough]]` | ✅ Implemented | Switch fallthrough |
| `typeof` | ✅ Implemented | Type-of operator |
| `typeof_unqual` | ✅ Implemented | Unqualified type-of |
| `nullptr` | ✅ Implemented | Null pointer constant |
| `true`/`false` | ✅ Implemented | Boolean keywords |
| `bool` keyword | ✅ Implemented | Boolean type keyword |
| Binary literals | ✅ Implemented | `0b1010` |
| Digit separators | ✅ Implemented | `1'000'000` |
| `u8` char literals | ✅ Implemented | `u8'A'` |
| `static_assert` | ✅ Implemented | Without message |

**GNU Extensions:**

| Feature | Status | Notes |
|---------|--------|-------|
| Statement expressions | ⚠️ Pending | `({ ... })` |
| Labels as values | ⚠️ Pending | `&&label` |
| Case ranges | ⚠️ Pending | `case 'a' ... 'z':` |
| `__typeof__` | ⚠️ Pending | GNU typeof |
| `__attribute__` | ⚠️ Pending | GNU attributes |

### Cross-Standard Warnings

MCC emits warnings when features from newer standards are used in older modes:

```bash
# Using C99 features in C89 mode
./mcc -std=c89 file.c
# warning: 'inline' is a keyword in C99; treating as identifier
# warning: '_Bool' is a C99 extension
# warning: long long is a C99 feature

# Using C11 features in C99 mode
./mcc -std=c99 file.c
# error: '_Static_assert' requires C11 or later
# warning: '_Noreturn' is a C11 extension

# Using C23 features in C11 mode
./mcc -std=c11 file.c
# warning: 'typeof' is a keyword in C23; treating as identifier
# warning: attribute syntax [[...]] is a C23 feature
# warning: digit separators are a C23 feature
```

### Extending the Feature System

To add more features (beyond 256):

1. Increase `MCC_FEATURE_WORDS` in `c_std.h`:
   ```c
   #define MCC_FEATURE_WORDS 8  /* 8 * 64 = 512 features */
   ```

2. Add new feature IDs to `mcc_feature_id_t` enum

3. Update `MCC_FEAT_COUNT`

4. Add feature initialization in `c_std.c`
