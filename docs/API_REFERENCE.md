# ANVIL API Reference

## Initialization

### anvil_init
```c
void anvil_init(void);
```
Initialize the ANVIL library. Must be called before any other ANVIL functions.

### anvil_shutdown
```c
void anvil_shutdown(void);
```
Shutdown the ANVIL library and free global resources.

## Module Management

### anvil_module_new
```c
AnvilModule* anvil_module_new(const char* name);
```
Create a new compilation module.

**Parameters:**
- `name`: Module name (used in debug output)

**Returns:** New module, or NULL on failure.

### anvil_module_free
```c
void anvil_module_free(AnvilModule* mod);
```
Free a module and all associated resources.

## Function Creation

### anvil_func_new
```c
AnvilFunc* anvil_func_new(AnvilModule* mod, const char* name, AnvilType* ret_type);
```
Create a new function in the module.

**Parameters:**
- `mod`: Parent module
- `name`: Function name
- `ret_type`: Return type (use `anvil_type_void()` for void functions)

### anvil_func_add_param
```c
AnvilVar* anvil_func_add_param(AnvilFunc* fn, const char* name, AnvilType* type);
```
Add a parameter to a function. Must be called before adding instructions.

### anvil_func_add_local
```c
AnvilVar* anvil_func_add_local(AnvilFunc* fn, const char* name, AnvilType* type);
```
Add a local variable to a function.

## Type System

### Primitive Types
```c
AnvilType* anvil_type_void(void);
AnvilType* anvil_type_bool(void);
AnvilType* anvil_type_i8(void);
AnvilType* anvil_type_i16(void);
AnvilType* anvil_type_i32(void);
AnvilType* anvil_type_i64(void);
AnvilType* anvil_type_u8(void);
AnvilType* anvil_type_u16(void);
AnvilType* anvil_type_u32(void);
AnvilType* anvil_type_u64(void);
AnvilType* anvil_type_f32(void);
AnvilType* anvil_type_f64(void);
```

### Compound Types
```c
AnvilType* anvil_type_ptr(AnvilModule* mod, AnvilType* pointee);
AnvilType* anvil_type_array(AnvilModule* mod, AnvilType* elem, int count);
AnvilType* anvil_type_struct(AnvilModule* mod, const char* name);
void anvil_struct_add_field(AnvilType* struct_type, const char* name, AnvilType* type);
```

## Constants

### Integer Constants
```c
AnvilValue* anvil_const_i8(AnvilFunc* fn, int8_t val);
AnvilValue* anvil_const_i16(AnvilFunc* fn, int16_t val);
AnvilValue* anvil_const_i32(AnvilFunc* fn, int32_t val);
AnvilValue* anvil_const_i64(AnvilFunc* fn, int64_t val);
AnvilValue* anvil_const_u8(AnvilFunc* fn, uint8_t val);
AnvilValue* anvil_const_u16(AnvilFunc* fn, uint16_t val);
AnvilValue* anvil_const_u32(AnvilFunc* fn, uint32_t val);
AnvilValue* anvil_const_u64(AnvilFunc* fn, uint64_t val);
```

### Other Constants
```c
AnvilValue* anvil_const_bool(AnvilFunc* fn, bool val);
AnvilValue* anvil_const_f32(AnvilFunc* fn, float val);
AnvilValue* anvil_const_f64(AnvilFunc* fn, double val);
AnvilValue* anvil_const_null(AnvilFunc* fn, AnvilType* ptr_type);
AnvilValue* anvil_const_string(AnvilModule* mod, const char* str);
```

## Arithmetic Operations

All arithmetic operations return an `AnvilValue*` representing the result.

```c
AnvilValue* anvil_add(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_sub(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_mul(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_div(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_mod(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_neg(AnvilFunc* fn, AnvilValue* val);
```

## Bitwise Operations

```c
AnvilValue* anvil_and(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_or(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_xor(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_not(AnvilFunc* fn, AnvilValue* val);
AnvilValue* anvil_shl(AnvilFunc* fn, AnvilValue* val, AnvilValue* shift);
AnvilValue* anvil_shr(AnvilFunc* fn, AnvilValue* val, AnvilValue* shift);
AnvilValue* anvil_sar(AnvilFunc* fn, AnvilValue* val, AnvilValue* shift);
```

## Comparison Operations

Return `i1` (boolean) values.

```c
AnvilValue* anvil_eq(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_ne(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_lt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_le(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_gt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_ge(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
```

## Memory Operations

```c
AnvilValue* anvil_load(AnvilFunc* fn, AnvilVar* var);
void anvil_store(AnvilFunc* fn, AnvilVar* var, AnvilValue* val);
AnvilValue* anvil_alloca(AnvilFunc* fn, AnvilType* type);
AnvilValue* anvil_deref(AnvilFunc* fn, AnvilValue* ptr);
AnvilValue* anvil_addr_of(AnvilFunc* fn, AnvilVar* var);
AnvilValue* anvil_index(AnvilFunc* fn, AnvilValue* ptr, AnvilValue* idx);
AnvilValue* anvil_field(AnvilFunc* fn, AnvilValue* ptr, const char* field_name);
```

## Control Flow

### Return
```c
void anvil_ret(AnvilFunc* fn, AnvilValue* val);
void anvil_ret_void(AnvilFunc* fn);
```

### Conditionals
```c
AnvilIf* anvil_if_begin(AnvilFunc* fn, AnvilValue* cond);
void anvil_if_else(AnvilIf* if_stmt);
void anvil_if_end(AnvilIf* if_stmt);
```

### Loops
```c
AnvilLoop* anvil_while_begin(AnvilFunc* fn, AnvilValue* cond);
void anvil_while_end(AnvilLoop* loop);

AnvilLoop* anvil_for_begin(AnvilFunc* fn, AnvilVar* var, 
                            AnvilValue* start, AnvilValue* end, AnvilValue* step);
void anvil_for_end(AnvilLoop* loop);

void anvil_break(AnvilFunc* fn);
void anvil_continue(AnvilFunc* fn);
```

### Function Calls
```c
AnvilValue* anvil_call(AnvilFunc* fn, const char* func_name, 
                        AnvilType* ret_type, int num_args, ...);
AnvilValue* anvil_call_variadic(AnvilFunc* fn, const char* func_name, 
                                 AnvilType* ret_type, int num_fixed_args, 
                                 int num_args, ...);
AnvilValue* anvil_call_indirect(AnvilFunc* fn, AnvilValue* func_ptr, 
                                 AnvilType* func_type, int num_args, ...);
```

**anvil_call_variadic** is used for calling variadic functions like `printf`. The `num_fixed_args` parameter specifies how many arguments are fixed (non-variadic). On some ABIs (like ARM64 Apple), variadic arguments are passed on the stack rather than in registers.

**Example:**
```c
// printf("Result: %d, %d\n", a, b);
// 1 fixed arg (format string), 3 total args
anvil_call_variadic(fn, "printf", anvil_type_i32(), 1, 3,
                    anvil_const_string(mod, "Result: %d, %d\n"), a, b);
```

## Target Configuration

### Predefined Targets
```c
AnvilTarget anvil_target_x86_64_linux(void);
AnvilTarget anvil_target_x86_64_windows(void);
AnvilTarget anvil_target_arm64_linux(void);
AnvilTarget anvil_target_arm64_macos(void);
AnvilTarget anvil_target_ppc64_linux(void);  // PowerPC 64-bit Big Endian Linux
```

### Target from Triple
```c
AnvilTarget anvil_target_from_triple(const char* triple);
```
Parse a target triple like "x86_64-linux-gnu" or "aarch64-apple-darwin".

### AnvilTarget Structure
```c
typedef struct AnvilTarget {
    int arch;           // ANVIL_ARCH_X86_64, ANVIL_ARCH_ARM64, etc.
    int os;             // ANVIL_OS_LINUX, ANVIL_OS_WINDOWS, ANVIL_OS_MACOS
    const char* abi_name;  // "sysv", "win64", "aapcs64", "apple"
    uint64_t features;  // Target-specific feature flags
} AnvilTarget;
```

### Architecture Constants
```c
#define ANVIL_ARCH_X86_64   0
#define ANVIL_ARCH_ARM64    1
#define ANVIL_ARCH_X86      2
#define ANVIL_ARCH_ARM32    4
#define ANVIL_ARCH_RISCV64  5
#define ANVIL_ARCH_PPC64    10  // PowerPC 64-bit Big Endian
```

### OS Constants
```c
#define ANVIL_OS_NONE       0
#define ANVIL_OS_LINUX      1
#define ANVIL_OS_WINDOWS    2
#define ANVIL_OS_MACOS      3
#define ANVIL_OS_BSD        4
```

## Compilation

### anvil_compile
```c
AnvilCompileResult anvil_compile(AnvilModule* mod, AnvilTarget target, int opt_level);
```

**Parameters:**
- `mod`: Module to compile
- `target`: Target configuration
- `opt_level`: Optimization level (0-5)
  - `ANVIL_OPT_NONE (0)`: No optimization
  - `ANVIL_OPT_DEBUG (1)`: Minimal optimization, preserve debug info
  - `ANVIL_OPT_BASIC (2)`: Basic optimizations
  - `ANVIL_OPT_STANDARD (3)`: Standard optimizations
  - `ANVIL_OPT_AGGRESSIVE (4)`: Aggressive optimizations
  - `ANVIL_OPT_SIZE (5)`: Optimize for size

**Returns:** `AnvilCompileResult` with generated assembly or errors.

### AnvilCompileResult
```c
typedef struct AnvilCompileResult {
    char* code;         // Generated assembly (NULL on error)
    size_t length;      // Length of code
    char* errors;       // Error messages (NULL on success)
    int num_errors;     // Number of errors
} AnvilCompileResult;
```

### anvil_result_free
```c
void anvil_result_free(AnvilCompileResult* result);
```
Free the memory associated with a compilation result.

## Debugging

### anvil_module_dump_ir
```c
void anvil_module_dump_ir(AnvilModule* mod, FILE* out);
```
Print the IR representation of a module to a file.

### anvil_get_last_error
```c
const char* anvil_get_last_error(void);
```
Get the last error message (if any).

## Complete Example

```c
#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    // Create module
    AnvilModule* mod = anvil_module_new("factorial");
    
    // Create function: int factorial(int n)
    AnvilFunc* fn = anvil_func_new(mod, "factorial", anvil_type_i32());
    AnvilVar* n = anvil_func_add_param(fn, "n", anvil_type_i32());
    AnvilVar* result = anvil_func_add_local(fn, "result", anvil_type_i32());
    AnvilVar* i = anvil_func_add_local(fn, "i", anvil_type_i32());
    
    // result = 1
    anvil_store(fn, result, anvil_const_i32(fn, 1));
    
    // for (i = 1; i <= n; i++)
    AnvilLoop* loop = anvil_for_begin(fn, i, 
        anvil_const_i32(fn, 1), 
        anvil_load(fn, n), 
        anvil_const_i32(fn, 1));
    {
        // result = result * i
        AnvilValue* prod = anvil_mul(fn, 
            anvil_load(fn, result), 
            anvil_load(fn, i));
        anvil_store(fn, result, prod);
    }
    anvil_for_end(loop);
    
    // return result
    anvil_ret(fn, anvil_load(fn, result));
    
    // Compile for ARM64 macOS
    AnvilTarget target = anvil_target_arm64_macos();
    AnvilCompileResult compile_result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
    
    if (compile_result.errors) {
        fprintf(stderr, "Compilation failed: %s\n", compile_result.errors);
    } else {
        printf("Generated assembly:\n%s", compile_result.code);
    }
    
    anvil_result_free(&compile_result);
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
```
