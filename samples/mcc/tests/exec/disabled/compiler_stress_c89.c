/*
 * EXTREME C89/ANSI C COMPILER STRESS TEST
 * ========================================
 * 
 * This code is designed to push C89/ANSI C compilers to their limits.
 * Compile with: gcc -ansi -pedantic -Wall -Wextra -O2 compiler_stress_c89.c -lm
 * 
 * C89 Constraints respected:
 *  - No // comments (only block comments)
 *  - No mixed declarations and code
 *  - No designated initializers
 *  - No compound literals
 *  - No Variable Length Arrays (VLA)
 *  - No inline keyword
 *  - No _Bool / bool
 *  - No long long (use long)
 *  - No restrict keyword
 *  - No _Complex
 *  - No variadic macros (__VA_ARGS__)
 *  - Declarations at block start only
 *
 * Features tested:
 *  - Deep recursive macro expansion
 *  - Token pasting and stringification
 *  - X-Macros for code generation
 *  - Function pointer matrices and dispatch tables
 *  - Bit-field packing edge cases
 *  - Deep pointer indirection (8 levels)
 *  - Struct/union type punning
 *  - Recursive data structures
 *  - Setjmp/longjmp for non-local jumps
 *  - Signal handling
 *  - Duff's device and computed switches
 *  - Complex expression evaluation
 *  - Memory pool allocator
 *  - State machine with jump tables
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <setjmp.h>
#include <signal.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

/* ============================================================================
   SECTION 1: PREPROCESSOR TORTURE (C89 COMPATIBLE)
   ============================================================================ */

/* Basic token pasting */
#define CAT(a, b) a ## b
#define CAT_(a, b) CAT(a, b)
#define CAT3(a, b, c) CAT_(CAT_(a, b), c)
#define CAT4(a, b, c, d) CAT_(CAT3(a, b, c), d)
#define CAT5(a, b, c, d, e) CAT_(CAT4(a, b, c, d), e)
#define CAT6(a, b, c, d, e, f) CAT_(CAT5(a, b, c, d, e), f)

/* Stringification */
#define STR(x) #x
#define XSTR(x) STR(x)

/* Multi-level expansion */
#define EXPAND(x) x
#define EXPAND2(x) EXPAND(EXPAND(x))
#define EXPAND3(x) EXPAND2(EXPAND2(x))
#define EXPAND4(x) EXPAND3(EXPAND3(x))

/* Repetition macros */
#define REP0(X)
#define REP1(X) X
#define REP2(X) REP1(X) X
#define REP3(X) REP2(X) X
#define REP4(X) REP3(X) X
#define REP5(X) REP4(X) X
#define REP6(X) REP5(X) X
#define REP7(X) REP6(X) X
#define REP8(X) REP7(X) X
#define REP9(X) REP8(X) X
#define REP10(X) REP9(X) X
#define REP16(X) REP8(X) REP8(X)
#define REP32(X) REP16(X) REP16(X)

/* Conditional compilation macros */
#define BOOL_0 0
#define BOOL_1 1
#define BOOL_2 1
#define BOOL_3 1
#define BOOL_TO_01(x) CAT_(BOOL_, x)

#define IF_0(t, f) f
#define IF_1(t, f) t
#define IF_ELSE(cond, t, f) CAT_(IF_, cond)(t, f)

/* Preprocessor arithmetic (limited range) */
#define INC_0 1
#define INC_1 2
#define INC_2 3
#define INC_3 4
#define INC_4 5
#define INC_5 6
#define INC_6 7
#define INC_7 8
#define INC_8 9
#define INC_9 10
#define INC(n) CAT_(INC_, n)

#define DEC_1 0
#define DEC_2 1
#define DEC_3 2
#define DEC_4 3
#define DEC_5 4
#define DEC_6 5
#define DEC_7 6
#define DEC_8 7
#define DEC_9 8
#define DEC_10 9
#define DEC(n) CAT_(DEC_, n)

/* Bit manipulation macros */
#define BIT(n) (1UL << (n))
#define MASK(n) (BIT(n) - 1)
#define SET_BIT(x, n) ((x) | BIT(n))
#define CLR_BIT(x, n) ((x) & ~BIT(n))
#define TOG_BIT(x, n) ((x) ^ BIT(n))
#define GET_BIT(x, n) (((x) >> (n)) & 1)

/* Min/Max with side-effect safety (using GNU extension or double eval) */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)

/* Array utilities */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ARRAY_END(arr) ((arr) + ARRAY_SIZE(arr))

/* Offset and container macros */
#define OFFSETOF(type, member) ((size_t)&((type *)0)->member)
#define CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - OFFSETOF(type, member)))

/* ============================================================================
   SECTION 2: X-MACRO CODE GENERATION
   ============================================================================ */

/* Define all opcodes using X-Macro pattern */
#define OPCODE_LIST \
    X(OP_NOP,    0,  "nop",    0) \
    X(OP_LOAD,   1,  "load",   1) \
    X(OP_STORE,  2,  "store",  1) \
    X(OP_ADD,    3,  "add",    2) \
    X(OP_SUB,    4,  "sub",    2) \
    X(OP_MUL,    5,  "mul",    2) \
    X(OP_DIV,    6,  "div",    2) \
    X(OP_MOD,    7,  "mod",    2) \
    X(OP_AND,    8,  "and",    2) \
    X(OP_OR,     9,  "or",     2) \
    X(OP_XOR,    10, "xor",    2) \
    X(OP_NOT,    11, "not",    1) \
    X(OP_NEG,    12, "neg",    1) \
    X(OP_SHL,    13, "shl",    2) \
    X(OP_SHR,    14, "shr",    2) \
    X(OP_JMP,    15, "jmp",    1) \
    X(OP_JZ,     16, "jz",     2) \
    X(OP_JNZ,    17, "jnz",    2) \
    X(OP_CALL,   18, "call",   1) \
    X(OP_RET,    19, "ret",    0) \
    X(OP_PUSH,   20, "push",   1) \
    X(OP_POP,    21, "pop",    1) \
    X(OP_HALT,   22, "halt",   0)

/* Generate enum */
typedef enum {
#define X(name, val, str, args) name = val,
    OPCODE_LIST
#undef X
    OP_COUNT
} OpCode;

/* Generate name strings array */
static const char *opcode_names[] = {
#define X(name, val, str, args) str,
    OPCODE_LIST
#undef X
};

/* Generate argument counts array */
static const int opcode_argc[] = {
#define X(name, val, str, args) args,
    OPCODE_LIST
#undef X
};

/* Error codes using X-Macro */
#define ERROR_LIST \
    X(ERR_NONE,         0,  "Success") \
    X(ERR_NOMEM,        1,  "Out of memory") \
    X(ERR_OVERFLOW,     2,  "Stack overflow") \
    X(ERR_UNDERFLOW,    3,  "Stack underflow") \
    X(ERR_DIVZERO,      4,  "Division by zero") \
    X(ERR_BADOP,        5,  "Invalid opcode") \
    X(ERR_BOUNDS,       6,  "Out of bounds") \
    X(ERR_NULLPTR,      7,  "Null pointer") \
    X(ERR_IO,           8,  "I/O error")

typedef enum {
#define X(name, val, str) name = val,
    ERROR_LIST
#undef X
    ERR_COUNT
} ErrorCode;

static const char *error_strings[] = {
#define X(name, val, str) str,
    ERROR_LIST
#undef X
};

/* ============================================================================
   SECTION 3: TYPE SYSTEM STRESS
   ============================================================================ */

/* Deep pointer types */
typedef int *Ptr1;
typedef Ptr1 *Ptr2;
typedef Ptr2 *Ptr3;
typedef Ptr3 *Ptr4;
typedef Ptr4 *Ptr5;
typedef Ptr5 *Ptr6;
typedef Ptr6 *Ptr7;
typedef Ptr7 *Ptr8;

/* Function pointer types with increasing complexity */
typedef int (*Fn0)(void);
typedef Fn0 (*Fn1)(int);
typedef Fn1 (*Fn2)(int, int);
typedef Fn2 (*Fn3)(Fn0);
typedef int (*BinaryOp)(int, int);
typedef int (*UnaryOp)(int);
typedef void (*VoidFn)(void);

/* Array of function pointers */
typedef BinaryOp BinaryOpTable[16];
typedef UnaryOp UnaryOpTable[16];

/* Bit-field stress structure */
struct BitFieldStress {
    unsigned int a : 1;
    unsigned int b : 2;
    unsigned int c : 3;
    unsigned int d : 5;
    unsigned int e : 7;
    unsigned int f : 11;
    unsigned int   : 0;  /* Force alignment */
    unsigned int g : 13;
    signed int   h : 4;
    signed int   i : 12;
    unsigned int j : 1;
    unsigned int   : 5;  /* Padding bits */
    unsigned int k : 8;
};

/* Union for type punning (implementation-defined but common) */
union TypePun {
    unsigned long ul;
    long sl;
    float f;
    unsigned char bytes[sizeof(long)];
    struct {
        unsigned short lo;
        unsigned short hi;
    } words;
};

/* Nested structures */
struct Level3 {
    int value;
    char tag[8];
};

struct Level2 {
    struct Level3 inner;
    int count;
    union {
        int as_int;
        float as_float;
        void *as_ptr;
    } data;
};

struct Level1 {
    struct Level2 nested;
    struct Level2 *next;
    int flags;
};

struct DeepNest {
    int id;
    struct Level1 content;
    struct {
        int x, y, z;
        struct {
            int r, g, b, a;
        } color;
    } position;
};

/* Flexible array emulation (C89 style with size 1) */
struct FlexArray {
    size_t count;
    size_t capacity;
    int type;
    int data[1];  /* C89 "struct hack" */
};

/* Linked list node */
struct ListNode {
    void *data;
    struct ListNode *next;
    struct ListNode *prev;
};

/* Tree node */
struct TreeNode {
    int key;
    void *value;
    struct TreeNode *left;
    struct TreeNode *right;
    struct TreeNode *parent;
    int height;
};

/* ============================================================================
   SECTION 4: FUNCTION POINTER DISPATCH SYSTEM
   ============================================================================ */

/* Binary operations */
static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int op_div(int a, int b) { return b != 0 ? a / b : 0; }
static int op_mod(int a, int b) { return b != 0 ? a % b : 0; }
static int op_and(int a, int b) { return a & b; }
static int op_or(int a, int b)  { return a | b; }
static int op_xor(int a, int b) { return a ^ b; }
static int op_shl(int a, int b) { return a << b; }
static int op_shr(int a, int b) { return a >> b; }
static int op_min(int a, int b) { return a < b ? a : b; }
static int op_max(int a, int b) { return a > b ? a : b; }
static int op_eq(int a, int b)  { return a == b; }
static int op_ne(int a, int b)  { return a != b; }
static int op_lt(int a, int b)  { return a < b; }
static int op_gt(int a, int b)  { return a > b; }

/* Unary operations */
static int uop_neg(int a)   { return -a; }
static int uop_not(int a)   { return ~a; }
static int uop_abs(int a)   { return a < 0 ? -a : a; }
static int uop_sign(int a)  { return (a > 0) - (a < 0); }
static int uop_sq(int a)    { return a * a; }
static int uop_inc(int a)   { return a + 1; }
static int uop_dec(int a)   { return a - 1; }
static int uop_bool(int a)  { return a != 0; }

/* Dispatch tables */
static BinaryOp binary_ops[] = {
    op_add, op_sub, op_mul, op_div,
    op_mod, op_and, op_or,  op_xor,
    op_shl, op_shr, op_min, op_max,
    op_eq,  op_ne,  op_lt,  op_gt
};

static UnaryOp unary_ops[] = {
    uop_neg, uop_not, uop_abs, uop_sign,
    uop_sq,  uop_inc, uop_dec, uop_bool
};

/* Higher-order function: apply binary op to array */
static void array_binop(int *dest, const int *src1, const int *src2, 
                        size_t len, BinaryOp op)
{
    size_t i;
    for (i = 0; i < len; i++) {
        dest[i] = op(src1[i], src2[i]);
    }
}

/* Higher-order function: apply unary op to array */
static void array_uop(int *dest, const int *src, size_t len, UnaryOp op)
{
    size_t i;
    for (i = 0; i < len; i++) {
        dest[i] = op(src[i]);
    }
}

/* Function composition */
static int compose_unary(int x, UnaryOp f, UnaryOp g)
{
    return f(g(x));
}

/* ============================================================================
   SECTION 5: DEEP POINTER CHAIN OPERATIONS
   ============================================================================ */

static Ptr8 build_ptr_chain_8(int value)
{
    int *p1;
    int **p2;
    int ***p3;
    int ****p4;
    int *****p5;
    int ******p6;
    int *******p7;
    int ********p8;
    
    p1 = (int *)malloc(sizeof(int));
    p2 = (int **)malloc(sizeof(int *));
    p3 = (int ***)malloc(sizeof(int **));
    p4 = (int ****)malloc(sizeof(int ***));
    p5 = (int *****)malloc(sizeof(int ****));
    p6 = (int ******)malloc(sizeof(int *****));
    p7 = (int *******)malloc(sizeof(int ******));
    p8 = (int ********)malloc(sizeof(int *******));
    
    if (!p1 || !p2 || !p3 || !p4 || !p5 || !p6 || !p7 || !p8) {
        free(p1); free(p2); free(p3); free(p4);
        free(p5); free(p6); free(p7); free(p8);
        return NULL;
    }
    
    *p1 = value;
    *p2 = p1;
    *p3 = p2;
    *p4 = p3;
    *p5 = p4;
    *p6 = p5;
    *p7 = p6;
    *p8 = p7;
    
    return p8;
}

/* Dereference macros */
#define DEREF1(p) (*(p))
#define DEREF2(p) (**(p))
#define DEREF3(p) (***(p))
#define DEREF4(p) (****(p))
#define DEREF5(p) (*****(p))
#define DEREF6(p) (******(p))
#define DEREF7(p) (*******(p))
#define DEREF8(p) (********(p))

static void free_ptr_chain_8(Ptr8 p8)
{
    Ptr7 p7;
    Ptr6 p6;
    Ptr5 p5;
    Ptr4 p4;
    Ptr3 p3;
    Ptr2 p2;
    Ptr1 p1;
    
    if (!p8) return;
    
    p7 = *p8;
    p6 = *p7;
    p5 = *p6;
    p4 = *p5;
    p3 = *p4;
    p2 = *p3;
    p1 = *p2;
    
    free(p1); free(p2); free(p3); free(p4);
    free(p5); free(p6); free(p7); free(p8);
}

/* ============================================================================
   SECTION 6: MEMORY POOL ALLOCATOR
   ============================================================================ */

#define POOL_BLOCK_SIZE 4096
#define POOL_ALIGN 8

struct PoolBlock {
    struct PoolBlock *next;
    size_t used;
    char data[POOL_BLOCK_SIZE - sizeof(struct PoolBlock *) - sizeof(size_t)];
};

struct MemPool {
    struct PoolBlock *head;
    struct PoolBlock *current;
    size_t total_allocated;
    size_t block_count;
};

static struct PoolBlock *pool_new_block(void)
{
    struct PoolBlock *block;
    block = (struct PoolBlock *)malloc(POOL_BLOCK_SIZE);
    if (block) {
        block->next = NULL;
        block->used = 0;
    }
    return block;
}

static void pool_init(struct MemPool *pool)
{
    pool->head = pool_new_block();
    pool->current = pool->head;
    pool->total_allocated = 0;
    pool->block_count = pool->head ? 1 : 0;
}

static void *pool_alloc(struct MemPool *pool, size_t size)
{
    size_t aligned_size;
    size_t available;
    void *ptr;
    struct PoolBlock *new_block;
    
    /* Align size */
    aligned_size = (size + POOL_ALIGN - 1) & ~(POOL_ALIGN - 1);
    
    if (!pool->current) return NULL;
    
    available = sizeof(pool->current->data) - pool->current->used;
    
    if (aligned_size > available) {
        /* Need new block */
        new_block = pool_new_block();
        if (!new_block) return NULL;
        
        pool->current->next = new_block;
        pool->current = new_block;
        pool->block_count++;
    }
    
    ptr = pool->current->data + pool->current->used;
    pool->current->used += aligned_size;
    pool->total_allocated += aligned_size;
    
    return ptr;
}

static void pool_destroy(struct MemPool *pool)
{
    struct PoolBlock *block;
    struct PoolBlock *next;
    
    block = pool->head;
    while (block) {
        next = block->next;
        free(block);
        block = next;
    }
    
    pool->head = NULL;
    pool->current = NULL;
    pool->total_allocated = 0;
    pool->block_count = 0;
}

/* ============================================================================
   SECTION 7: SETJMP/LONGJMP EXCEPTION EMULATION
   ============================================================================ */

#define TRY_STACK_SIZE 16

static jmp_buf try_stack[TRY_STACK_SIZE];
static int try_stack_top = 0;
static int exception_code = 0;
static const char *exception_msg = NULL;

#define TRY \
    do { \
        if (try_stack_top < TRY_STACK_SIZE) { \
            int _exc_code = setjmp(try_stack[try_stack_top++]); \
            if (_exc_code == 0) {

#define CATCH(code) \
            } else { \
                try_stack_top--; \
                code = _exc_code;

#define END_TRY \
            } \
        } \
    } while(0)

#define THROW(code, msg) \
    do { \
        exception_code = (code); \
        exception_msg = (msg); \
        if (try_stack_top > 0) { \
            longjmp(try_stack[try_stack_top - 1], code); \
        } else { \
            fprintf(stderr, "Unhandled exception %d: %s\n", code, msg); \
            exit(1); \
        } \
    } while(0)

/* Test function that may throw */
static int risky_divide(int a, int b)
{
    if (b == 0) {
        THROW(ERR_DIVZERO, "Division by zero");
    }
    return a / b;
}

static void *risky_alloc(size_t size)
{
    void *p = malloc(size);
    if (!p) {
        THROW(ERR_NOMEM, "Allocation failed");
    }
    return p;
}

/* ============================================================================
   SECTION 8: BIT MANIPULATION ALGORITHMS
   ============================================================================ */

/* Reverse bits in a 32-bit word */
static unsigned long reverse_bits_32(unsigned long x)
{
    x = ((x & 0x55555555UL) << 1)  | ((x & 0xAAAAAAAAUL) >> 1);
    x = ((x & 0x33333333UL) << 2)  | ((x & 0xCCCCCCCCUL) >> 2);
    x = ((x & 0x0F0F0F0FUL) << 4)  | ((x & 0xF0F0F0F0UL) >> 4);
    x = ((x & 0x00FF00FFUL) << 8)  | ((x & 0xFF00FF00UL) >> 8);
    x = ((x & 0x0000FFFFUL) << 16) | ((x & 0xFFFF0000UL) >> 16);
    return x;
}

/* Population count (number of set bits) */
static int popcount_32(unsigned long x)
{
    x = x - ((x >> 1) & 0x55555555UL);
    x = (x & 0x33333333UL) + ((x >> 2) & 0x33333333UL);
    x = (x + (x >> 4)) & 0x0F0F0F0FUL;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return (int)(x & 0x3F);
}

/* Count leading zeros */
static int clz_32(unsigned long x)
{
    int n = 0;
    if (x == 0) return 32;
    if ((x & 0xFFFF0000UL) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF000000UL) == 0) { n += 8;  x <<= 8;  }
    if ((x & 0xF0000000UL) == 0) { n += 4;  x <<= 4;  }
    if ((x & 0xC0000000UL) == 0) { n += 2;  x <<= 2;  }
    if ((x & 0x80000000UL) == 0) { n += 1; }
    return n;
}

/* Count trailing zeros */
static int ctz_32(unsigned long x)
{
    int n = 0;
    if (x == 0) return 32;
    if ((x & 0x0000FFFFUL) == 0) { n += 16; x >>= 16; }
    if ((x & 0x000000FFUL) == 0) { n += 8;  x >>= 8;  }
    if ((x & 0x0000000FUL) == 0) { n += 4;  x >>= 4;  }
    if ((x & 0x00000003UL) == 0) { n += 2;  x >>= 2;  }
    if ((x & 0x00000001UL) == 0) { n += 1; }
    return n;
}

/* Next power of 2 */
static unsigned long next_pow2(unsigned long x)
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

/* Integer log base 2 */
static int ilog2(unsigned long x)
{
    return 31 - clz_32(x);
}

/* Rotate left */
static unsigned long rotl_32(unsigned long x, int n)
{
    n &= 31;
    return (x << n) | (x >> (32 - n));
}

/* Rotate right */
static unsigned long rotr_32(unsigned long x, int n)
{
    n &= 31;
    return (x >> n) | (x << (32 - n));
}

/* Gray code */
static unsigned long to_gray(unsigned long x)
{
    return x ^ (x >> 1);
}

static unsigned long from_gray(unsigned long x)
{
    unsigned long mask;
    for (mask = x >> 1; mask; mask >>= 1) {
        x ^= mask;
    }
    return x;
}

/* Morton code (Z-order) for 2D */
static unsigned long morton_encode(unsigned int x, unsigned int y)
{
    unsigned long result = 0;
    int i;
    for (i = 0; i < 16; i++) {
        result |= ((unsigned long)((x >> i) & 1) << (2 * i));
        result |= ((unsigned long)((y >> i) & 1) << (2 * i + 1));
    }
    return result;
}

/* ============================================================================
   SECTION 9: DUFF'S DEVICE AND LOOP UNROLLING
   ============================================================================ */

/* Classic Duff's device for memory copy */
static void duff_memcpy(char *to, const char *from, size_t count)
{
    size_t n;
    
    if (count == 0) return;
    
    n = (count + 7) / 8;
    
    switch (count % 8) {
    case 0: do { *to++ = *from++;
    case 7:      *to++ = *from++;
    case 6:      *to++ = *from++;
    case 5:      *to++ = *from++;
    case 4:      *to++ = *from++;
    case 3:      *to++ = *from++;
    case 2:      *to++ = *from++;
    case 1:      *to++ = *from++;
            } while (--n > 0);
    }
}

/* Unrolled array sum */
static long unrolled_sum(const int *arr, size_t len)
{
    long sum = 0;
    size_t i;
    size_t unrolled_len;
    
    /* Process 8 elements at a time */
    unrolled_len = len & ~7UL;
    for (i = 0; i < unrolled_len; i += 8) {
        sum += arr[i];
        sum += arr[i + 1];
        sum += arr[i + 2];
        sum += arr[i + 3];
        sum += arr[i + 4];
        sum += arr[i + 5];
        sum += arr[i + 6];
        sum += arr[i + 7];
    }
    
    /* Handle remainder */
    for (; i < len; i++) {
        sum += arr[i];
    }
    
    return sum;
}

/* ============================================================================
   SECTION 10: STATE MACHINE WITH FUNCTION POINTERS
   ============================================================================ */

typedef enum {
    ST_IDLE,
    ST_RUNNING,
    ST_PAUSED,
    ST_ERROR,
    ST_DONE,
    ST_COUNT
} MachineState;

typedef enum {
    EV_START,
    EV_PAUSE,
    EV_RESUME,
    EV_STOP,
    EV_ERROR,
    EV_RESET,
    EV_COUNT
} MachineEvent;

struct Machine;
typedef MachineState (*StateHandler)(struct Machine *, MachineEvent);

struct Machine {
    MachineState state;
    int data;
    int error_code;
    StateHandler handlers[ST_COUNT];
    void (*on_enter)(struct Machine *, MachineState);
    void (*on_exit)(struct Machine *, MachineState);
};

/* State handlers */
static MachineState handle_idle(struct Machine *m, MachineEvent ev)
{
    switch (ev) {
    case EV_START:
        return ST_RUNNING;
    case EV_ERROR:
        m->error_code = 1;
        return ST_ERROR;
    default:
        return ST_IDLE;
    }
}

static MachineState handle_running(struct Machine *m, MachineEvent ev)
{
    (void)m;
    switch (ev) {
    case EV_PAUSE:
        return ST_PAUSED;
    case EV_STOP:
        return ST_DONE;
    case EV_ERROR:
        return ST_ERROR;
    default:
        return ST_RUNNING;
    }
}

static MachineState handle_paused(struct Machine *m, MachineEvent ev)
{
    (void)m;
    switch (ev) {
    case EV_RESUME:
        return ST_RUNNING;
    case EV_STOP:
        return ST_DONE;
    case EV_ERROR:
        return ST_ERROR;
    default:
        return ST_PAUSED;
    }
}

static MachineState handle_error(struct Machine *m, MachineEvent ev)
{
    switch (ev) {
    case EV_RESET:
        m->error_code = 0;
        return ST_IDLE;
    default:
        return ST_ERROR;
    }
}

static MachineState handle_done(struct Machine *m, MachineEvent ev)
{
    switch (ev) {
    case EV_RESET:
        m->data = 0;
        return ST_IDLE;
    default:
        return ST_DONE;
    }
}

static void machine_init(struct Machine *m)
{
    m->state = ST_IDLE;
    m->data = 0;
    m->error_code = 0;
    m->handlers[ST_IDLE] = handle_idle;
    m->handlers[ST_RUNNING] = handle_running;
    m->handlers[ST_PAUSED] = handle_paused;
    m->handlers[ST_ERROR] = handle_error;
    m->handlers[ST_DONE] = handle_done;
    m->on_enter = NULL;
    m->on_exit = NULL;
}

static void machine_dispatch(struct Machine *m, MachineEvent ev)
{
    MachineState old_state;
    MachineState new_state;
    
    old_state = m->state;
    new_state = m->handlers[old_state](m, ev);
    
    if (new_state != old_state) {
        if (m->on_exit) m->on_exit(m, old_state);
        m->state = new_state;
        if (m->on_enter) m->on_enter(m, new_state);
    }
}

/* ============================================================================
   SECTION 11: EXPRESSION PARSER (RECURSIVE DESCENT)
   ============================================================================ */

struct Parser {
    const char *input;
    size_t pos;
    int error;
    char error_msg[128];
};

static void parser_init(struct Parser *p, const char *input)
{
    p->input = input;
    p->pos = 0;
    p->error = 0;
    p->error_msg[0] = '\0';
}

static void parser_error(struct Parser *p, const char *msg)
{
    p->error = 1;
    strncpy(p->error_msg, msg, sizeof(p->error_msg) - 1);
    p->error_msg[sizeof(p->error_msg) - 1] = '\0';
}

static void skip_whitespace(struct Parser *p)
{
    while (p->input[p->pos] == ' ' || p->input[p->pos] == '\t') {
        p->pos++;
    }
}

static char peek(struct Parser *p)
{
    skip_whitespace(p);
    return p->input[p->pos];
}

static char advance(struct Parser *p)
{
    skip_whitespace(p);
    return p->input[p->pos++];
}

/* Forward declarations */
static double parse_expr(struct Parser *p);
static double parse_term(struct Parser *p);
static double parse_factor(struct Parser *p);
static double parse_unary(struct Parser *p);
static double parse_primary(struct Parser *p);

static double parse_number(struct Parser *p)
{
    double result = 0.0;
    double fraction = 0.1;
    int has_dot = 0;
    int has_digits = 0;
    char c;
    
    while (1) {
        c = p->input[p->pos];
        if (c >= '0' && c <= '9') {
            has_digits = 1;
            if (has_dot) {
                result += (c - '0') * fraction;
                fraction *= 0.1;
            } else {
                result = result * 10 + (c - '0');
            }
            p->pos++;
        } else if (c == '.' && !has_dot) {
            has_dot = 1;
            p->pos++;
        } else {
            break;
        }
    }
    
    if (!has_digits) {
        parser_error(p, "Expected number");
    }
    
    return result;
}

static double parse_primary(struct Parser *p)
{
    char c;
    double result;
    
    c = peek(p);
    
    if (c == '(') {
        advance(p);
        result = parse_expr(p);
        if (peek(p) == ')') {
            advance(p);
        } else {
            parser_error(p, "Expected ')'");
        }
        return result;
    } else if ((c >= '0' && c <= '9') || c == '.') {
        return parse_number(p);
    } else {
        parser_error(p, "Expected number or '('");
        return 0.0;
    }
}

static double parse_unary(struct Parser *p)
{
    char c;
    
    c = peek(p);
    if (c == '-') {
        advance(p);
        return -parse_unary(p);
    } else if (c == '+') {
        advance(p);
        return parse_unary(p);
    }
    return parse_primary(p);
}

static double parse_factor(struct Parser *p)
{
    double base;
    
    base = parse_unary(p);
    
    if (peek(p) == '^') {
        advance(p);
        return pow(base, parse_factor(p));  /* Right associative */
    }
    
    return base;
}

static double parse_term(struct Parser *p)
{
    double left;
    char op;
    
    left = parse_factor(p);
    
    while ((op = peek(p)) == '*' || op == '/' || op == '%') {
        advance(p);
        if (op == '*') {
            left *= parse_factor(p);
        } else if (op == '/') {
            left /= parse_factor(p);
        } else {
            left = fmod(left, parse_factor(p));
        }
    }
    
    return left;
}

static double parse_expr(struct Parser *p)
{
    double left;
    char op;
    
    left = parse_term(p);
    
    while ((op = peek(p)) == '+' || op == '-') {
        advance(p);
        if (op == '+') {
            left += parse_term(p);
        } else {
            left -= parse_term(p);
        }
    }
    
    return left;
}

static double evaluate_expression(const char *expr, int *error)
{
    struct Parser p;
    double result;
    
    parser_init(&p, expr);
    result = parse_expr(&p);
    *error = p.error;
    
    return result;
}

/* ============================================================================
   SECTION 12: HASH TABLE IMPLEMENTATION
   ============================================================================ */

#define HASH_SIZE 64
#define HASH_DELETED ((void *)-1)

struct HashEntry {
    unsigned long hash;
    const char *key;
    void *value;
};

struct HashTable {
    struct HashEntry entries[HASH_SIZE];
    size_t count;
    size_t deleted;
};

/* FNV-1a hash */
static unsigned long hash_string(const char *str)
{
    unsigned long hash = 2166136261UL;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 16777619UL;
    }
    return hash;
}

static void hash_init(struct HashTable *ht)
{
    size_t i;
    for (i = 0; i < HASH_SIZE; i++) {
        ht->entries[i].key = NULL;
        ht->entries[i].value = NULL;
        ht->entries[i].hash = 0;
    }
    ht->count = 0;
    ht->deleted = 0;
}

static void *hash_get(struct HashTable *ht, const char *key)
{
    unsigned long h;
    size_t idx;
    size_t i;
    
    h = hash_string(key);
    idx = h % HASH_SIZE;
    
    for (i = 0; i < HASH_SIZE; i++) {
        size_t probe = (idx + i) % HASH_SIZE;
        
        if (ht->entries[probe].key == NULL) {
            return NULL;  /* Not found */
        }
        
        if (ht->entries[probe].key != HASH_DELETED &&
            ht->entries[probe].hash == h &&
            strcmp(ht->entries[probe].key, key) == 0) {
            return ht->entries[probe].value;
        }
    }
    
    return NULL;
}

static int hash_put(struct HashTable *ht, const char *key, void *value)
{
    unsigned long h;
    size_t idx;
    size_t i;
    size_t first_deleted;
    int found_deleted;
    
    if (ht->count + ht->deleted >= HASH_SIZE * 3 / 4) {
        return 0;  /* Table full */
    }
    
    h = hash_string(key);
    idx = h % HASH_SIZE;
    first_deleted = HASH_SIZE;
    found_deleted = 0;
    
    for (i = 0; i < HASH_SIZE; i++) {
        size_t probe = (idx + i) % HASH_SIZE;
        
        if (ht->entries[probe].key == NULL) {
            /* Insert here */
            size_t insert_idx = found_deleted ? first_deleted : probe;
            ht->entries[insert_idx].hash = h;
            ht->entries[insert_idx].key = key;
            ht->entries[insert_idx].value = value;
            ht->count++;
            if (found_deleted && insert_idx == first_deleted) {
                ht->deleted--;
            }
            return 1;
        }
        
        if (ht->entries[probe].key == HASH_DELETED) {
            if (!found_deleted) {
                first_deleted = probe;
                found_deleted = 1;
            }
            continue;
        }
        
        if (ht->entries[probe].hash == h &&
            strcmp(ht->entries[probe].key, key) == 0) {
            /* Update existing */
            ht->entries[probe].value = value;
            return 1;
        }
    }
    
    return 0;
}

/* ============================================================================
   SECTION 13: COROUTINE SIMULATION
   ============================================================================ */

struct Coroutine {
    int state;
    int value;
    int limit;
    union {
        struct { int a, b, temp; } fib;
        struct { int current, step; } range;
        struct { int n, result; } factorial;
    } locals;
};

#define CORO_BEGIN(c) switch((c)->state) { case 0:
#define CORO_YIELD(c, v) do { (c)->state = __LINE__; (c)->value = (v); return 1; case __LINE__:; } while(0)
#define CORO_END(c) } (c)->state = -1; return 0

static int coro_fibonacci(struct Coroutine *c)
{
    CORO_BEGIN(c);
    
    c->locals.fib.a = 0;
    c->locals.fib.b = 1;
    
    while (c->limit-- > 0) {
        CORO_YIELD(c, c->locals.fib.a);
        c->locals.fib.temp = c->locals.fib.a + c->locals.fib.b;
        c->locals.fib.a = c->locals.fib.b;
        c->locals.fib.b = c->locals.fib.temp;
    }
    
    CORO_END(c);
}

static int coro_range(struct Coroutine *c)
{
    CORO_BEGIN(c);
    
    c->locals.range.current = 0;
    c->locals.range.step = 1;
    
    while (c->locals.range.current < c->limit) {
        CORO_YIELD(c, c->locals.range.current);
        c->locals.range.current += c->locals.range.step;
    }
    
    CORO_END(c);
}

static int coro_factorial(struct Coroutine *c)
{
    CORO_BEGIN(c);
    
    c->locals.factorial.result = 1;
    c->locals.factorial.n = 1;
    
    while (c->locals.factorial.n <= c->limit) {
        c->locals.factorial.result *= c->locals.factorial.n;
        CORO_YIELD(c, c->locals.factorial.result);
        c->locals.factorial.n++;
    }
    
    CORO_END(c);
}

/* ============================================================================
   SECTION 14: VARIADIC FUNCTIONS (C89 STYLE)
   ============================================================================ */

/* Type-tagged variadic sum */
#define TAG_END    0
#define TAG_INT    1
#define TAG_LONG   2
#define TAG_DOUBLE 3
#define TAG_PTR    4

static double tagged_sum(int first_tag, ...)
{
    va_list args;
    double sum = 0.0;
    int tag;
    
    va_start(args, first_tag);
    tag = first_tag;
    
    while (tag != TAG_END) {
        switch (tag) {
        case TAG_INT:
            sum += va_arg(args, int);
            break;
        case TAG_LONG:
            sum += va_arg(args, long);
            break;
        case TAG_DOUBLE:
            sum += va_arg(args, double);
            break;
        case TAG_PTR:
            sum += (long)(size_t)va_arg(args, void *);
            break;
        }
        tag = va_arg(args, int);
    }
    
    va_end(args);
    return sum;
}

/* Custom printf subset */
static int mini_printf(const char *fmt, ...)
{
    va_list args;
    int count = 0;
    char c;
    
    va_start(args, fmt);
    
    while ((c = *fmt++) != '\0') {
        if (c == '%') {
            c = *fmt++;
            switch (c) {
            case 'd': {
                int val = va_arg(args, int);
                count += printf("%d", val);
                break;
            }
            case 'l': {
                if (*fmt == 'd') {
                    fmt++;
                    count += printf("%ld", va_arg(args, long));
                }
                break;
            }
            case 'f': {
                double val = va_arg(args, double);
                count += printf("%f", val);
                break;
            }
            case 's': {
                const char *val = va_arg(args, const char *);
                count += printf("%s", val ? val : "(null)");
                break;
            }
            case 'p': {
                void *val = va_arg(args, void *);
                count += printf("%p", val);
                break;
            }
            case 'x': {
                unsigned int val = va_arg(args, unsigned int);
                count += printf("%x", val);
                break;
            }
            case 'B': {
                /* Binary format (custom) */
                unsigned long val = va_arg(args, unsigned long);
                int i;
                for (i = 31; i >= 0; i--) {
                    putchar((val & (1UL << i)) ? '1' : '0');
                    count++;
                    if (i > 0 && i % 8 == 0) {
                        putchar('_');
                        count++;
                    }
                }
                break;
            }
            case '%':
                putchar('%');
                count++;
                break;
            default:
                putchar('%');
                putchar(c);
                count += 2;
            }
        } else {
            putchar(c);
            count++;
        }
    }
    
    va_end(args);
    return count;
}

/* ============================================================================
   SECTION 15: VIRTUAL MACHINE
   ============================================================================ */

#define VM_STACK_SIZE 256
#define VM_MEM_SIZE 1024
#define VM_REG_COUNT 16

struct VM {
    long regs[VM_REG_COUNT];
    long stack[VM_STACK_SIZE];
    long memory[VM_MEM_SIZE];
    size_t sp;
    size_t pc;
    int halted;
    int error;
};

struct Instruction {
    OpCode op;
    int r1, r2, r3;  /* Register operands */
    long imm;        /* Immediate value */
};

static void vm_init(struct VM *vm)
{
    size_t i;
    for (i = 0; i < VM_REG_COUNT; i++) vm->regs[i] = 0;
    for (i = 0; i < VM_STACK_SIZE; i++) vm->stack[i] = 0;
    for (i = 0; i < VM_MEM_SIZE; i++) vm->memory[i] = 0;
    vm->sp = 0;
    vm->pc = 0;
    vm->halted = 0;
    vm->error = 0;
}

static int vm_push(struct VM *vm, long value)
{
    if (vm->sp >= VM_STACK_SIZE) {
        vm->error = ERR_OVERFLOW;
        return 0;
    }
    vm->stack[vm->sp++] = value;
    return 1;
}

static int vm_pop(struct VM *vm, long *value)
{
    if (vm->sp == 0) {
        vm->error = ERR_UNDERFLOW;
        return 0;
    }
    *value = vm->stack[--vm->sp];
    return 1;
}

static int vm_execute(struct VM *vm, const struct Instruction *instr)
{
    long tmp;
    
    switch (instr->op) {
    case OP_NOP:
        break;
        
    case OP_LOAD:
        if ((size_t)instr->imm < VM_MEM_SIZE) {
            vm->regs[instr->r1] = vm->memory[instr->imm];
        } else {
            vm->error = ERR_BOUNDS;
        }
        break;
        
    case OP_STORE:
        if ((size_t)instr->imm < VM_MEM_SIZE) {
            vm->memory[instr->imm] = vm->regs[instr->r1];
        } else {
            vm->error = ERR_BOUNDS;
        }
        break;
        
    case OP_ADD:
        vm->regs[instr->r1] = vm->regs[instr->r2] + vm->regs[instr->r3];
        break;
        
    case OP_SUB:
        vm->regs[instr->r1] = vm->regs[instr->r2] - vm->regs[instr->r3];
        break;
        
    case OP_MUL:
        vm->regs[instr->r1] = vm->regs[instr->r2] * vm->regs[instr->r3];
        break;
        
    case OP_DIV:
        if (vm->regs[instr->r3] == 0) {
            vm->error = ERR_DIVZERO;
        } else {
            vm->regs[instr->r1] = vm->regs[instr->r2] / vm->regs[instr->r3];
        }
        break;
        
    case OP_MOD:
        if (vm->regs[instr->r3] == 0) {
            vm->error = ERR_DIVZERO;
        } else {
            vm->regs[instr->r1] = vm->regs[instr->r2] % vm->regs[instr->r3];
        }
        break;
        
    case OP_AND:
        vm->regs[instr->r1] = vm->regs[instr->r2] & vm->regs[instr->r3];
        break;
        
    case OP_OR:
        vm->regs[instr->r1] = vm->regs[instr->r2] | vm->regs[instr->r3];
        break;
        
    case OP_XOR:
        vm->regs[instr->r1] = vm->regs[instr->r2] ^ vm->regs[instr->r3];
        break;
        
    case OP_NOT:
        vm->regs[instr->r1] = ~vm->regs[instr->r2];
        break;
        
    case OP_NEG:
        vm->regs[instr->r1] = -vm->regs[instr->r2];
        break;
        
    case OP_SHL:
        vm->regs[instr->r1] = vm->regs[instr->r2] << vm->regs[instr->r3];
        break;
        
    case OP_SHR:
        vm->regs[instr->r1] = (unsigned long)vm->regs[instr->r2] >> vm->regs[instr->r3];
        break;
        
    case OP_JMP:
        vm->pc = (size_t)instr->imm;
        return 1;  /* Don't increment PC */
        
    case OP_JZ:
        if (vm->regs[instr->r1] == 0) {
            vm->pc = (size_t)instr->imm;
            return 1;
        }
        break;
        
    case OP_JNZ:
        if (vm->regs[instr->r1] != 0) {
            vm->pc = (size_t)instr->imm;
            return 1;
        }
        break;
        
    case OP_PUSH:
        vm_push(vm, vm->regs[instr->r1]);
        break;
        
    case OP_POP:
        vm_pop(vm, &tmp);
        vm->regs[instr->r1] = tmp;
        break;
        
    case OP_CALL:
        vm_push(vm, (long)vm->pc + 1);
        vm->pc = (size_t)instr->imm;
        return 1;
        
    case OP_RET:
        if (!vm_pop(vm, &tmp)) return 0;
        vm->pc = (size_t)tmp;
        return 1;
        
    case OP_HALT:
        vm->halted = 1;
        break;
        
    default:
        vm->error = ERR_BADOP;
    }
    
    vm->pc++;
    return vm->error == 0;
}

/* ============================================================================
   SECTION 16: MAIN FUNCTION
   ============================================================================ */

static const char *state_names[] = {
    "IDLE", "RUNNING", "PAUSED", "ERROR", "DONE"
};

int main(void)
{
    /* All declarations at the start (C89 requirement) */
    Ptr8 deep_ptr;
    int test_arr[16];
    int result_arr[16];
    long arr_sum;
    int i;
    int exc_code;
    unsigned long test_val;
    struct MemPool pool;
    int *pool_data;
    struct Machine machine;
    MachineEvent events[6];
    struct Parser parser;
    const char *expr;
    double expr_result;
    int expr_error;
    struct HashTable ht;
    int hash_vals[4];
    struct Coroutine fib_coro;
    double tagged_result;
    struct VM vm;
    struct Instruction program[5];
    struct DeepNest deep_struct;
    struct BitFieldStress bits;
    union TypePun pun;
    int composed;
    
    printf("=== C89/ANSI C COMPILER STRESS TEST ===\n\n");
    
    /* Test 1: X-Macro generated tables */
    printf("[1] X-Macro generated opcode table:\n");
    printf("    Opcodes defined: %d\n", OP_COUNT);
    printf("    OP_ADD name: \"%s\", args: %d\n", 
           opcode_names[OP_ADD], opcode_argc[OP_ADD]);
    printf("    OP_HALT name: \"%s\", args: %d\n\n",
           opcode_names[OP_HALT], opcode_argc[OP_HALT]);
    
    /* Test 2: Deep pointer chain */
    printf("[2] Deep pointer chain (8 levels):\n");
    deep_ptr = build_ptr_chain_8(42);
    if (deep_ptr) {
        printf("    Value through 8-level deref: %d\n\n", DEREF8(deep_ptr));
        free_ptr_chain_8(deep_ptr);
    }
    
    /* Test 3: Function pointer dispatch */
    printf("[3] Function pointer dispatch:\n");
    for (i = 0; i < 16; i++) {
        test_arr[i] = i + 1;
    }
    array_uop(result_arr, test_arr, 16, uop_sq);
    printf("    Square of 1..4: %d, %d, %d, %d\n",
           result_arr[0], result_arr[1], result_arr[2], result_arr[3]);
    
    composed = compose_unary(5, uop_neg, uop_sq);
    printf("    Composed neg(sq(5)): %d\n\n", composed);
    
    /* Test 4: Unrolled array sum */
    printf("[4] Unrolled array operations:\n");
    arr_sum = unrolled_sum(test_arr, 16);
    printf("    Sum of 1..16: %ld\n\n", arr_sum);
    
    /* Test 5: Duff's device */
    printf("[5] Duff's device memcpy:\n");
    {
        char src[] = "Hello, Duff!";
        char dst[20];
        memset(dst, 0, sizeof(dst));
        duff_memcpy(dst, src, strlen(src));
        printf("    Copied: \"%s\"\n\n", dst);
    }
    
    /* Test 6: Setjmp/longjmp exceptions */
    printf("[6] Exception handling (setjmp/longjmp):\n");
    exc_code = 0;
    TRY {
        printf("    Trying risky_divide(10, 2)...\n");
        printf("    Result: %d\n", risky_divide(10, 2));
        printf("    Trying risky_divide(10, 0)...\n");
        printf("    Result: %d\n", risky_divide(10, 0));
    } CATCH(exc_code) {
        printf("    Caught exception %d: %s\n", exc_code, 
               error_strings[exc_code]);
    } END_TRY;
    printf("\n");
    
    /* Test 7: Bit manipulation */
    printf("[7] Bit manipulation:\n");
    test_val = 0xDEADBEEFUL;
    mini_printf("    Original:  %B\n", test_val);
    mini_printf("    Reversed:  %B\n", reverse_bits_32(test_val));
    printf("    Popcount:  %d\n", popcount_32(test_val));
    printf("    CLZ:       %d\n", clz_32(test_val));
    printf("    CTZ:       %d\n", ctz_32(test_val));
    printf("    Gray code: 0x%08lX -> 0x%08lX\n", 
           5UL, to_gray(5UL));
    printf("    Morton(3,5): 0x%08lX\n\n", morton_encode(3, 5));
    
    /* Test 8: Memory pool */
    printf("[8] Memory pool allocator:\n");
    pool_init(&pool);
    pool_data = (int *)pool_alloc(&pool, 100 * sizeof(int));
    if (pool_data) {
        for (i = 0; i < 100; i++) pool_data[i] = i * i;
        printf("    Allocated 100 ints, pool_data[10] = %d\n", pool_data[10]);
        printf("    Total allocated: %lu bytes\n", 
               (unsigned long)pool.total_allocated);
        printf("    Block count: %lu\n", (unsigned long)pool.block_count);
    }
    pool_destroy(&pool);
    printf("\n");
    
    /* Test 9: State machine */
    printf("[9] State machine:\n");
    machine_init(&machine);
    events[0] = EV_START;
    events[1] = EV_PAUSE;
    events[2] = EV_RESUME;
    events[3] = EV_ERROR;
    events[4] = EV_RESET;
    events[5] = EV_START;
    
    printf("    Initial: %s\n", state_names[machine.state]);
    for (i = 0; i < 6; i++) {
        machine_dispatch(&machine, events[i]);
        printf("    After event %d: %s\n", events[i], state_names[machine.state]);
    }
    printf("\n");
    
    /* Test 10: Expression parser */
    printf("[10] Expression parser:\n");
    expr = "3 + 4 * 2 - 1";
    expr_result = evaluate_expression(expr, &expr_error);
    printf("    \"%s\" = %f\n", expr, expr_result);
    
    expr = "2 ^ 3 ^ 2";
    expr_result = evaluate_expression(expr, &expr_error);
    printf("    \"%s\" = %f\n", expr, expr_result);
    
    expr = "(1 + 2) * (3 + 4)";
    expr_result = evaluate_expression(expr, &expr_error);
    printf("    \"%s\" = %f\n\n", expr, expr_result);
    
    /* Test 11: Hash table */
    printf("[11] Hash table:\n");
    hash_init(&ht);
    hash_vals[0] = 100;
    hash_vals[1] = 200;
    hash_vals[2] = 300;
    hash_vals[3] = 400;
    hash_put(&ht, "alpha", &hash_vals[0]);
    hash_put(&ht, "beta", &hash_vals[1]);
    hash_put(&ht, "gamma", &hash_vals[2]);
    hash_put(&ht, "delta", &hash_vals[3]);
    printf("    hash[\"alpha\"] = %d\n", *(int *)hash_get(&ht, "alpha"));
    printf("    hash[\"gamma\"] = %d\n", *(int *)hash_get(&ht, "gamma"));
    printf("    hash[\"omega\"] = %s\n\n", 
           hash_get(&ht, "omega") ? "found" : "(nil)");
    
    /* Test 12: Coroutines */
    printf("[12] Coroutine (Fibonacci generator):\n");
    fib_coro.state = 0;
    fib_coro.limit = 10;
    printf("    ");
    while (coro_fibonacci(&fib_coro)) {
        printf("%d ", fib_coro.value);
    }
    printf("\n\n");
    
    /* Test 13: Variadic functions */
    printf("[13] Variadic functions:\n");
    tagged_result = tagged_sum(
        TAG_INT, 10,
        TAG_DOUBLE, 3.14,
        TAG_LONG, 1000L,
        TAG_INT, -5,
        TAG_END
    );
    printf("    Tagged sum: %f\n", tagged_result);
    mini_printf("    mini_printf test: %d + %f = %s\n", 42, 3.14, "success");
    printf("\n");
    
    /* Test 14: Virtual machine */
    printf("[14] Virtual machine:\n");
    vm_init(&vm);
    vm.regs[1] = 10;
    vm.regs[2] = 20;
    
    /* Program: R0 = R1 + R2; R3 = R0 * 2; HALT */
    program[0].op = OP_ADD;
    program[0].r1 = 0; program[0].r2 = 1; program[0].r3 = 2;
    
    program[1].op = OP_ADD;
    program[1].r1 = 3; program[1].r2 = 0; program[1].r3 = 0;
    
    program[2].op = OP_HALT;
    
    while (!vm.halted && !vm.error && vm.pc < 3) {
        vm_execute(&vm, &program[vm.pc]);
    }
    
    printf("    R0 = R1 + R2 = %ld\n", vm.regs[0]);
    printf("    R3 = R0 + R0 = %ld\n\n", vm.regs[3]);
    
    /* Test 15: Deep nested structure */
    printf("[15] Deep nested structure:\n");
    memset(&deep_struct, 0, sizeof(deep_struct));
    deep_struct.id = 123;
    deep_struct.content.nested.inner.value = 456;
    strcpy(deep_struct.content.nested.inner.tag, "TEST");
    deep_struct.position.x = 10;
    deep_struct.position.y = 20;
    deep_struct.position.z = 30;
    deep_struct.position.color.r = 255;
    printf("    deep.id = %d\n", deep_struct.id);
    printf("    deep.content.nested.inner.value = %d\n", 
           deep_struct.content.nested.inner.value);
    printf("    deep.position.color.r = %d\n\n", deep_struct.position.color.r);
    
    /* Test 16: Bit fields */
    printf("[16] Bit field structure:\n");
    memset(&bits, 0, sizeof(bits));
    bits.a = 1;
    bits.b = 3;
    bits.c = 7;
    bits.d = 31;
    bits.h = -8;  /* Signed bit field */
    printf("    bits.a (1 bit) = %u\n", bits.a);
    printf("    bits.b (2 bits) = %u\n", bits.b);
    printf("    bits.c (3 bits) = %u\n", bits.c);
    printf("    bits.d (5 bits) = %u\n", bits.d);
    printf("    bits.h (4 bits, signed) = %d\n\n", bits.h);
    
    /* Test 17: Type punning */
    printf("[17] Type punning union:\n");
    pun.f = 3.14159f;
    printf("    Float 3.14159 as bytes: ");
    for (i = 0; i < (int)sizeof(float); i++) {
        printf("%02X ", pun.bytes[i]);
    }
    printf("\n    As unsigned long: 0x%08lX\n\n", pun.ul & 0xFFFFFFFFUL);
    
    /* Test 18: Macro expansion */
    printf("[18] Macro expansion stress:\n");
    printf("    CAT6(a,b,c,d,e,f) = %s\n", XSTR(CAT6(a,b,c,d,e,f)));
    printf("    INC(INC(INC(5))) = %d\n", INC(INC(INC(5))));
    printf("    REP4(X) expands to: %s\n", XSTR(REP4(X)));
    printf("    ARRAY_SIZE of test_arr: %lu\n\n", 
           (unsigned long)ARRAY_SIZE(test_arr));
    
    printf("=== ALL TESTS COMPLETED ===\n");
    
    return 0;
}