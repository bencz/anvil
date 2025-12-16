# ANVIL Base64 Library Example

This example demonstrates using ANVIL to generate a base64 encoding library that can be linked with C programs.

## Features

- **Base64 encoding**: Converts binary data to base64 string
- **Memory allocation**: Calls external `malloc` for output buffer
- **Bitwise operations**: Demonstrates shifts, masks, and OR operations
- **Complex control flow**: Loops with multiple conditional branches
- **Character mapping**: Index-to-character conversion using select operations

## Generated Functions

### `int base64_encoded_len(int input_len)`
Returns the length of the base64 encoded output (without null terminator).

**Formula**: `((input_len + 2) / 3) * 4`

### `char* base64_encode(const char* input, int len, int* out_len)`
Encodes input bytes to a base64 string.

**Parameters**:
- `input`: Pointer to input data
- `len`: Length of input data in bytes
- `out_len`: Pointer to store output length (can be NULL)

**Returns**: Malloc'd null-terminated string (caller must free)

## Base64 Encoding Algorithm

Base64 encodes 3 bytes (24 bits) into 4 characters (6 bits each):

```
Input:  [byte1]  [byte2]  [byte3]
        XXXXXXXX YYYYYYYY ZZZZZZZZ

Output: [char1]  [char2]  [char3]  [char4]
        XXXXXX   XXYYYY   YYYYZZ   ZZZZZZ
```

Character mapping:
- 0-25  → 'A'-'Z'
- 26-51 → 'a'-'z'
- 52-61 → '0'-'9'
- 62    → '+'
- 63    → '/'
- Padding: '='

## Building

```bash
# Build and test
make test

# Or step by step:
make generate_base64    # Build the ANVIL generator
make base64_lib.s       # Generate assembly
make base64_lib.o       # Assemble
make test_base64        # Link with test program
./test_base64           # Run tests
```

## Example Usage

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char* base64_encode(const char* input, int len, int* out_len);

int main(void) {
    const char* message = "Hello, World!";
    int out_len;
    
    char* encoded = base64_encode(message, strlen(message), &out_len);
    
    printf("Input:  %s\n", message);
    printf("Base64: %s\n", encoded);
    printf("Length: %d\n", out_len);
    
    free(encoded);
    return 0;
}
```

Output:
```
Input:  Hello, World!
Base64: SGVsbG8sIFdvcmxkIQ==
Length: 20
```

## Test Vectors

The test program verifies against RFC 4648 test vectors:

| Input    | Base64 Output |
|----------|---------------|
| ""       | ""            |
| "f"      | "Zg=="        |
| "fo"     | "Zm8="        |
| "foo"    | "Zm9v"        |
| "foob"   | "Zm9vYg=="    |
| "fooba"  | "Zm9vYmE="    |
| "foobar" | "Zm9vYmFy"    |

## Architecture Support

Tested on:
- ARM64 (macOS Apple Silicon)
- x86-64 (Linux)

Should work on all ANVIL-supported architectures.

## Key ANVIL Features Demonstrated

1. **External function calls**: `malloc` for dynamic allocation
2. **Bitwise operations**: `shl`, `shr`, `and`, `or` for bit manipulation
3. **Select operations**: `anvil_build_select` for conditional values
4. **Byte-level memory access**: `i8` loads and stores
5. **Complex loops**: Multiple conditional branches within loop body
6. **Type conversions**: `zext`, `trunc` for byte/int conversions
