/*
 * EXTREME C99 COMPILER STRESS TEST
 * =================================
 * 
 * This code is designed to push C99 compilers to their limits.
 * Compile with: gcc -std=c99 -Wall -Wextra -pedantic -O2 compiler_stress_c99.c -lm
 * 
 * Features tested:
 *  - Deep recursive macro expansion (up to preprocessor limits)
 *  - Variadic macros with complex expansions
 *  - Variable Length Arrays (VLA) in nested scopes
 *  - Complex number arithmetic (C99 _Complex)
 *  - Designated initializers with deep nesting
 *  - Compound literals in expressions
 *  - Function pointer matrices and dispatch tables
 *  - Bit-field packing edge cases
 *  - Flexible array members
 *  - restrict pointers
 *  - inline functions with complex bodies
 *  - Long long arithmetic edge cases
 *  - Hexadecimal floating-point literals
 *  - Variadic function implementations
 *  - Computed values in initializers
 *  - Deep pointer indirection (up to 8 levels)
 *  - Struct/union type punning
 *  - Recursive data structures
 *  - Token pasting stress
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <complex.h>
#include <stdbool.h>
#include <limits.h>
#include <float.h>

/* ============================================================================
   SECTION 1: PREPROCESSOR TORTURE
   ============================================================================ */

/* Basic building blocks */
#define EMPTY()
#define DEFER(id) id EMPTY()
#define OBSTRUCT(...) __VA_ARGS__ DEFER(EMPTY)()

/* Multi-level expansion */
#define EVAL(...)  EVAL1(EVAL1(EVAL1(__VA_ARGS__)))
#define EVAL1(...) EVAL2(EVAL2(EVAL2(__VA_ARGS__)))
#define EVAL2(...) EVAL3(EVAL3(EVAL3(__VA_ARGS__)))
#define EVAL3(...) EVAL4(EVAL4(EVAL4(__VA_ARGS__)))
#define EVAL4(...) EVAL5(EVAL5(EVAL5(__VA_ARGS__)))
#define EVAL5(...) __VA_ARGS__

/* Token pasting chains */
#define CAT(a, b) a ## b
#define CAT_(a, b) CAT(a, b)
#define CAT3(a, b, c) CAT_(CAT_(a, b), c)
#define CAT4(a, b, c, d) CAT_(CAT3(a, b, c), d)
#define CAT5(a, b, c, d, e) CAT_(CAT4(a, b, c, d), e)
#define CAT6(a, b, c, d, e, f) CAT_(CAT5(a, b, c, d, e), f)

/* Stringification */
#define STR(x) #x
#define XSTR(x) STR(x)
#define STR_CAT(a, b) XSTR(a) XSTR(b)

/* Argument counting (up to 64 args) */
#define ARG_N( \
    _1,  _2,  _3,  _4,  _5,  _6,  _7,  _8,  _9,  _10, \
    _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, \
    _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, \
    _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, \
    _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, \
    _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, \
    _61, _62, _63, N, ...) N

#define RSEQ_N() \
    63, 62, 61, 60, 59, 58, 57, 56, 55, 54, \
    53, 52, 51, 50, 49, 48, 47, 46, 45, 44, \
    43, 42, 41, 40, 39, 38, 37, 36, 35, 34, \
    33, 32, 31, 30, 29, 28, 27, 26, 25, 24, \
    23, 22, 21, 20, 19, 18, 17, 16, 15, 14, \
    13, 12, 11, 10,  9,  8,  7,  6,  5,  4, \
     3,  2,  1,  0

#define NARGS_(...) ARG_N(__VA_ARGS__)
#define NARGS(...) NARGS_(__VA_ARGS__, RSEQ_N())

/* Conditional macros */
#define FIRST(a, ...) a
#define SECOND(a, b, ...) b
#define IS_PROBE(...) SECOND(__VA_ARGS__, 0)
#define PROBE() ~, 1

#define NOT(x) IS_PROBE(CAT_(NOT_, x))
#define NOT_0 PROBE()

#define BOOL(x) NOT(NOT(x))
#define IF_ELSE(condition) CAT_(IF_, BOOL(condition))
#define IF_1(...) __VA_ARGS__ IF_1_ELSE
#define IF_0(...)             IF_0_ELSE
#define IF_1_ELSE(...)
#define IF_0_ELSE(...) __VA_ARGS__

/* Recursive macro iteration (MAP) */
#define MAP_END(...)
#define MAP_OUT
#define MAP_COMMA ,

#define MAP_GET_END2() 0, MAP_END
#define MAP_GET_END1(...) MAP_GET_END2
#define MAP_GET_END(...) MAP_GET_END1
#define MAP_NEXT0(test, next, ...) next MAP_OUT
#define MAP_NEXT1(test, next) MAP_NEXT0(test, next, 0)
#define MAP_NEXT(test, next)  MAP_NEXT1(MAP_GET_END test, next)

#define MAP0(f, x, peek, ...) f(x) MAP_NEXT(peek, MAP1)(f, peek, __VA_ARGS__)
#define MAP1(f, x, peek, ...) f(x) MAP_NEXT(peek, MAP0)(f, peek, __VA_ARGS__)

#define MAP_LIST_NEXT1(test, next) MAP_NEXT0(test, MAP_COMMA next, 0)
#define MAP_LIST_NEXT(test, next)  MAP_LIST_NEXT1(MAP_GET_END test, next)

#define MAP_LIST0(f, x, peek, ...) f(x) MAP_LIST_NEXT(peek, MAP_LIST1)(f, peek, __VA_ARGS__)
#define MAP_LIST1(f, x, peek, ...) f(x) MAP_LIST_NEXT(peek, MAP_LIST0)(f, peek, __VA_ARGS__)

#define MAP(f, ...) EVAL(MAP1(f, __VA_ARGS__, ()()(), ()()(), ()()(), 0))
#define MAP_LIST(f, ...) EVAL(MAP_LIST1(f, __VA_ARGS__, ()()(), ()()(), ()()(), 0))

/* Generate repeated patterns */
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
#define REP(N, X) CAT_(REP, N)(X)

/* ============================================================================
   SECTION 2: TYPE SYSTEM STRESS
   ============================================================================ */

/* Deep pointer types */
typedef int *ptr1_t;
typedef ptr1_t *ptr2_t;
typedef ptr2_t *ptr3_t;
typedef ptr3_t *ptr4_t;
typedef ptr4_t *ptr5_t;
typedef ptr5_t *ptr6_t;
typedef ptr6_t *ptr7_t;
typedef ptr7_t *ptr8_t;

/* Function pointer types with increasing complexity */
typedef int (*fn0_t)(void);
typedef fn0_t (*fn1_t)(int);
typedef fn1_t (*fn2_t)(int, int);
typedef fn2_t (*fn3_t)(int, int, int);
typedef fn3_t (*fn4_t)(fn0_t);
typedef fn4_t (*fn5_t)(fn1_t, fn2_t);

/* Array of function pointers returning function pointers */
typedef int (*(*fn_matrix_t)[4])(int);

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
    unsigned int h : 17;
    signed int   i : 4;
    signed int   j : 12;
    unsigned int k : 1;
    unsigned int   : 5;  /* Padding bits */
    unsigned int l : 8;
};

/* Nested anonymous structures and unions */
struct DeepNesting {
    int level0;
    struct {
        int level1a;
        union {
            int level2a_int;
            double level2a_double;
            struct {
                char level3a;
                short level3b;
                struct {
                    int level4a;
                    struct {
                        long level5a;
                        union {
                            float level6_float;
                            int level6_int;
                            struct {
                                char level7[8];
                                struct {
                                    int level8_final;
                                } deepest;
                            } level7_struct;
                        } level6;
                    } level5;
                } level4;
            } level3;
        } level2;
        int level1b;
    } level1;
};

/* Flexible array member with complex base */
struct FlexibleMonster {
    size_t count;
    struct {
        int type;
        union {
            int i;
            double d;
            void *p;
        } value;
    } metadata;
    double complex data[];  /* C99 flexible array member */
};

/* Recursive structure definition */
struct TreeNode;
typedef struct TreeNode TreeNode;
struct TreeNode {
    int value;
    TreeNode *left;
    TreeNode *right;
    TreeNode *parent;
    TreeNode **children;  /* Dynamic array of children */
    size_t child_count;
    int (*comparator)(const TreeNode *, const TreeNode *);
    void (*destructor)(TreeNode *);
};

/* ============================================================================
   SECTION 3: COMPLEX NUMBER TORTURE (C99)
   ============================================================================ */

/* Complex number function matrix */
typedef double complex (*complex_binop_t)(double complex, double complex);

static inline double complex cx_add(double complex a, double complex b) {
    return a + b;
}

static inline double complex cx_sub(double complex a, double complex b) {
    return a - b;
}

static inline double complex cx_mul(double complex a, double complex b) {
    return a * b;
}

static inline double complex cx_div(double complex a, double complex b) {
    return a / b;
}

static inline double complex cx_pow_int(double complex base, int exp) {
    double complex result = 1.0;
    int neg = exp < 0;
    exp = neg ? -exp : exp;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return neg ? 1.0 / result : result;
}

/* Mandelbrot iteration with complex arithmetic */
static inline int mandelbrot_escape(double complex c, int max_iter) {
    double complex z = 0;
    for (int i = 0; i < max_iter; i++) {
        z = z * z + c;
        if (cabs(z) > 2.0) return i;
    }
    return max_iter;
}

/* Complex polynomial evaluator using Horner's method */
static double complex poly_eval_complex(
    const double complex *restrict coeffs,
    size_t degree,
    double complex x
) {
    double complex result = coeffs[degree];
    for (size_t i = degree; i > 0; i--) {
        result = result * x + coeffs[i - 1];
    }
    return result;
}

/* ============================================================================
   SECTION 4: VLA STRESS (C99)
   ============================================================================ */

/* Matrix operations with VLAs */
static void matrix_multiply_vla(
    size_t m, size_t n, size_t p,
    const double a[restrict m][n],
    const double b[restrict n][p],
    double c[restrict m][p]
) {
    for (size_t i = 0; i < m; i++) {
        for (size_t j = 0; j < p; j++) {
            c[i][j] = 0.0;
            for (size_t k = 0; k < n; k++) {
                c[i][j] += a[i][k] * b[k][j];
            }
        }
    }
}

/* Nested VLA with complex indexing */
static double nested_vla_computation(size_t n) {
    /* 3D VLA */
    double (*cube)[n][n] = malloc(n * sizeof(*cube));
    if (!cube) return -1.0;
    
    /* Initialize with complex pattern */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            for (size_t k = 0; k < n; k++) {
                cube[i][j][k] = sin((double)(i * n * n + j * n + k));
            }
        }
    }
    
    /* Compute something complex */
    double result = 0.0;
    for (size_t i = 0; i < n; i++) {
        /* Inner VLA */
        double row_sums[n];
        for (size_t j = 0; j < n; j++) {
            row_sums[j] = 0.0;
            for (size_t k = 0; k < n; k++) {
                row_sums[j] += cube[i][j][k] * cube[(i+1)%n][k][j];
            }
        }
        for (size_t j = 0; j < n; j++) {
            result += row_sums[j];
        }
    }
    
    free(cube);
    return result;
}

/* ============================================================================
   SECTION 5: FUNCTION POINTER DISPATCH SYSTEM
   ============================================================================ */

/* Operation types */
typedef enum {
    OP_NOP = 0,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_SHL,
    OP_SHR,
    OP_NEG,
    OP_NOT,
    OP_COUNT
} OpCode;

/* Instruction structure */
typedef struct {
    OpCode op;
    int32_t operands[3];
    uint8_t flags;
} Instruction;

/* Virtual machine state */
typedef struct VMState VMState;

/* Operation handler type */
typedef int (*op_handler_t)(VMState *, const Instruction *);

struct VMState {
    int64_t registers[16];
    int64_t *memory;
    size_t memory_size;
    size_t pc;
    uint32_t flags;
    op_handler_t handlers[OP_COUNT];
    void (*trace_callback)(const VMState *, const Instruction *);
};

/* Macro-generated operation handlers */
#define DEFINE_BINARY_OP(name, op) \
    static int handle_##name(VMState *vm, const Instruction *instr) { \
        vm->registers[instr->operands[0]] = \
            vm->registers[instr->operands[1]] op \
            vm->registers[instr->operands[2]]; \
        return 0; \
    }

#define DEFINE_UNARY_OP(name, op) \
    static int handle_##name(VMState *vm, const Instruction *instr) { \
        vm->registers[instr->operands[0]] = \
            op vm->registers[instr->operands[1]]; \
        return 0; \
    }

static int handle_nop(VMState *vm, const Instruction *instr) {
    (void)vm; (void)instr;
    return 0;
}

DEFINE_BINARY_OP(add, +)
DEFINE_BINARY_OP(sub, -)
DEFINE_BINARY_OP(mul, *)
DEFINE_BINARY_OP(div, /)
DEFINE_BINARY_OP(mod, %)
DEFINE_BINARY_OP(and, &)
DEFINE_BINARY_OP(or,  |)
DEFINE_BINARY_OP(xor, ^)
DEFINE_BINARY_OP(shl, <<)
DEFINE_BINARY_OP(shr, >>)
DEFINE_UNARY_OP(neg, -)
DEFINE_UNARY_OP(not, ~)

/* Handler table initializer using designated initializers */
#define INIT_VM_HANDLERS(vm) do { \
    (vm)->handlers[OP_NOP] = handle_nop; \
    (vm)->handlers[OP_ADD] = handle_add; \
    (vm)->handlers[OP_SUB] = handle_sub; \
    (vm)->handlers[OP_MUL] = handle_mul; \
    (vm)->handlers[OP_DIV] = handle_div; \
    (vm)->handlers[OP_MOD] = handle_mod; \
    (vm)->handlers[OP_AND] = handle_and; \
    (vm)->handlers[OP_OR]  = handle_or;  \
    (vm)->handlers[OP_XOR] = handle_xor; \
    (vm)->handlers[OP_SHL] = handle_shl; \
    (vm)->handlers[OP_SHR] = handle_shr; \
    (vm)->handlers[OP_NEG] = handle_neg; \
    (vm)->handlers[OP_NOT] = handle_not; \
} while(0)

/* ============================================================================
   SECTION 6: COMPOUND LITERAL AND DESIGNATED INITIALIZER STRESS
   ============================================================================ */

/* Deeply nested designated initializer */
static struct DeepNesting create_deep_struct(int seed) {
    return (struct DeepNesting) {
        .level0 = seed,
        .level1 = {
            .level1a = seed * 2,
            .level2 = {
                .level3 = {
                    .level3a = (char)(seed & 0xFF),
                    .level3b = (short)(seed & 0xFFFF),
                    .level4 = {
                        .level4a = seed * 3,
                        .level5 = {
                            .level5a = (long)seed * seed,
                            .level6 = {
                                .level7_struct = {
                                    .level7 = { 
                                        [0] = 'D', [1] = 'E', [2] = 'E', [3] = 'P',
                                        [4] = 'N', [5] = 'E', [6] = 'S', [7] = 'T'
                                    },
                                    .deepest = {
                                        .level8_final = seed * 8
                                    }
                                }
                            }
                        }
                    }
                }
            },
            .level1b = seed / 2
        }
    };
}

/* Array of compound literals */
static const int *get_primes_table(size_t *out_count) {
    static const int primes[] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29,
        31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
        73, 79, 83, 89, 97, 101, 103, 107, 109, 113
    };
    *out_count = sizeof(primes) / sizeof(primes[0]);
    return primes;
}

/* Compound literal in expression context */
#define MAKE_POINT(x, y) ((struct { int x, y; }){ .x = (x), .y = (y) })
#define POINT_DIST_SQ(p) ((p).x * (p).x + (p).y * (p).y)

/* ============================================================================
   SECTION 7: VARIADIC FUNCTION STRESS
   ============================================================================ */

/* Type-safe variadic sum using compound literals and macros */
typedef struct {
    enum { T_INT, T_DOUBLE, T_LONG, T_PTR } type;
    union {
        int i;
        double d;
        long l;
        void *p;
    } value;
} TypedValue;

#define TV_INT(x)    ((TypedValue){ .type = T_INT,    .value.i = (x) })
#define TV_DOUBLE(x) ((TypedValue){ .type = T_DOUBLE, .value.d = (x) })
#define TV_LONG(x)   ((TypedValue){ .type = T_LONG,   .value.l = (x) })
#define TV_PTR(x)    ((TypedValue){ .type = T_PTR,    .value.p = (x) })
#define TV_END       ((TypedValue){ .type = -1 })

static double sum_typed_values(TypedValue first, ...) {
    va_list args;
    va_start(args, first);
    
    double sum = 0.0;
    TypedValue current = first;
    
    while (current.type != -1) {
        switch (current.type) {
            case T_INT:    sum += current.value.i; break;
            case T_DOUBLE: sum += current.value.d; break;
            case T_LONG:   sum += current.value.l; break;
            case T_PTR:    sum += (intptr_t)current.value.p; break;
        }
        current = va_arg(args, TypedValue);
    }
    
    va_end(args);
    return sum;
}

/* Printf-like format parser with custom specifiers */
static int custom_printf(const char *restrict fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    int count = 0;
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd': {
                    int val = va_arg(args, int);
                    count += printf("%d", val);
                    break;
                }
                case 'f': {
                    double val = va_arg(args, double);
                    count += printf("%f", val);
                    break;
                }
                case 'c': {
                    /* Complex number custom specifier */
                    double complex val = va_arg(args, double complex);
                    count += printf("(%g%+gi)", creal(val), cimag(val));
                    break;
                }
                case 'B': {
                    /* Binary output custom specifier */
                    unsigned int val = va_arg(args, unsigned int);
                    for (int i = 31; i >= 0; i--) {
                        putchar((val & (1u << i)) ? '1' : '0');
                        count++;
                        if (i % 8 == 0 && i > 0) { putchar('_'); count++; }
                    }
                    break;
                }
                case '%':
                    putchar('%');
                    count++;
                    break;
                default:
                    putchar('%');
                    putchar(*fmt);
                    count += 2;
            }
        } else {
            putchar(*fmt);
            count++;
        }
        fmt++;
    }
    
    va_end(args);
    return count;
}

/* ============================================================================
   SECTION 8: DEEP POINTER CHAIN OPERATIONS
   ============================================================================ */

/* Build N-level pointer chain */
#define BUILD_PTR_CHAIN(n, val) build_ptr_chain_##n(val)

static ptr8_t build_ptr_chain_8(int value) {
    int *p1 = malloc(sizeof(int));
    int **p2 = malloc(sizeof(int*));
    int ***p3 = malloc(sizeof(int**));
    int ****p4 = malloc(sizeof(int***));
    int *****p5 = malloc(sizeof(int****));
    int ******p6 = malloc(sizeof(int*****));
    int *******p7 = malloc(sizeof(int******));
    int ********p8 = malloc(sizeof(int*******));
    
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

/* Dereference N levels */
#define DEREF1(p) (*(p))
#define DEREF2(p) (*(*(p)))
#define DEREF3(p) (*(*(*(p))))
#define DEREF4(p) (*(*(*(*(p)))))
#define DEREF5(p) (*(*(*(*(*(p))))))
#define DEREF6(p) (*(*(*(*(*(*(p)))))))
#define DEREF7(p) (*(*(*(*(*(*(*(p))))))))
#define DEREF8(p) (*(*(*(*(*(*(*(*(p)))))))))

static void free_ptr_chain_8(ptr8_t p8) {
    if (!p8) return;
    ptr7_t p7 = *p8;
    ptr6_t p6 = *p7;
    ptr5_t p5 = *p6;
    ptr4_t p4 = *p5;
    ptr3_t p3 = *p4;
    ptr2_t p2 = *p3;
    ptr1_t p1 = *p2;
    
    free(p1); free(p2); free(p3); free(p4);
    free(p5); free(p6); free(p7); free(p8);
}

/* ============================================================================
   SECTION 9: EXPRESSION TEMPLATE EMULATION
   ============================================================================ */

/* Lazy evaluation through function composition */
typedef struct Expr Expr;
typedef double (*eval_fn_t)(const Expr *);

struct Expr {
    eval_fn_t eval;
    union {
        double constant;
        struct { const Expr *left, *right; } binary;
        struct { const Expr *operand; } unary;
    } data;
};

static double eval_const(const Expr *e) { return e->data.constant; }
static double eval_add(const Expr *e) { 
    return e->data.binary.left->eval(e->data.binary.left) + 
           e->data.binary.right->eval(e->data.binary.right); 
}
static double eval_mul(const Expr *e) { 
    return e->data.binary.left->eval(e->data.binary.left) * 
           e->data.binary.right->eval(e->data.binary.right); 
}
static double eval_neg(const Expr *e) {
    return -e->data.unary.operand->eval(e->data.unary.operand);
}
static double eval_sin(const Expr *e) {
    return sin(e->data.unary.operand->eval(e->data.unary.operand));
}

#define EXPR_CONST(v) ((Expr){ .eval = eval_const, .data.constant = (v) })
#define EXPR_ADD(l, r) ((Expr){ .eval = eval_add, .data.binary = { &(l), &(r) } })
#define EXPR_MUL(l, r) ((Expr){ .eval = eval_mul, .data.binary = { &(l), &(r) } })
#define EXPR_NEG(x) ((Expr){ .eval = eval_neg, .data.unary = { &(x) } })
#define EXPR_SIN(x) ((Expr){ .eval = eval_sin, .data.unary = { &(x) } })

/* ============================================================================
   SECTION 10: BIT MANIPULATION STRESS
   ============================================================================ */

/* Bit reversal */
static inline uint32_t reverse_bits_32(uint32_t x) {
    x = ((x & 0x55555555u) << 1)  | ((x & 0xAAAAAAAAu) >> 1);
    x = ((x & 0x33333333u) << 2)  | ((x & 0xCCCCCCCCu) >> 2);
    x = ((x & 0x0F0F0F0Fu) << 4)  | ((x & 0xF0F0F0F0u) >> 4);
    x = ((x & 0x00FF00FFu) << 8)  | ((x & 0xFF00FF00u) >> 8);
    x = ((x & 0x0000FFFFu) << 16) | ((x & 0xFFFF0000u) >> 16);
    return x;
}

/* Population count without __builtin_popcount */
static inline int popcount_32(uint32_t x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return x & 0x3F;
}

/* Find first set bit */
static inline int ffs_32(uint32_t x) {
    if (x == 0) return 0;
    int n = 1;
    if ((x & 0x0000FFFFu) == 0) { n += 16; x >>= 16; }
    if ((x & 0x000000FFu) == 0) { n += 8;  x >>= 8;  }
    if ((x & 0x0000000Fu) == 0) { n += 4;  x >>= 4;  }
    if ((x & 0x00000003u) == 0) { n += 2;  x >>= 2;  }
    return n - (x & 1);
}

/* Morton code encoding (Z-order curve) */
static inline uint64_t morton_encode_2d(uint32_t x, uint32_t y) {
    uint64_t result = 0;
    for (int i = 0; i < 32; i++) {
        result |= ((uint64_t)((x >> i) & 1) << (2 * i));
        result |= ((uint64_t)((y >> i) & 1) << (2 * i + 1));
    }
    return result;
}

/* ============================================================================
   SECTION 11: HEXADECIMAL FLOAT LITERALS (C99)
   ============================================================================ */

static const double hex_float_constants[] = {
    0x1.0p0,          /* 1.0 */
    0x1.8p0,          /* 1.5 */
    0x1.0p-1,         /* 0.5 */
    0x1.0p10,         /* 1024.0 */
    0x1.fffffffffffffp+1023,  /* Near DBL_MAX */
    0x1.0p-1022,      /* DBL_MIN normalized */
    0x0.0000000000001p-1022,  /* Subnormal */
    0x1.921fb54442d18p+1,     /* Pi */
    0x1.5bf0a8b145769p+1,     /* E */
    -0x1.0p0,         /* -1.0 */
};

/* ============================================================================
   SECTION 12: RECURSIVE DESCENT EXPRESSION PARSER
   ============================================================================ */

typedef struct {
    const char *input;
    size_t pos;
    int error;
} Parser;

static double parse_expr(Parser *p);
static double parse_term(Parser *p);
static double parse_factor(Parser *p);
static double parse_primary(Parser *p);

static void skip_whitespace(Parser *p) {
    while (p->input[p->pos] == ' ' || p->input[p->pos] == '\t') {
        p->pos++;
    }
}

static double parse_number(Parser *p) {
    double result = 0.0;
    double fraction = 0.1;
    int has_dot = 0;
    
    while (1) {
        char c = p->input[p->pos];
        if (c >= '0' && c <= '9') {
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
    return result;
}

static double parse_primary(Parser *p) {
    skip_whitespace(p);
    char c = p->input[p->pos];
    
    if (c == '(') {
        p->pos++;
        double result = parse_expr(p);
        skip_whitespace(p);
        if (p->input[p->pos] == ')') p->pos++;
        else p->error = 1;
        return result;
    } else if ((c >= '0' && c <= '9') || c == '.') {
        return parse_number(p);
    } else if (c == '-') {
        p->pos++;
        return -parse_factor(p);
    }
    p->error = 1;
    return 0.0;
}

static double parse_factor(Parser *p) {
    double base = parse_primary(p);
    skip_whitespace(p);
    if (p->input[p->pos] == '^') {
        p->pos++;
        double exp = parse_factor(p);
        return pow(base, exp);
    }
    return base;
}

static double parse_term(Parser *p) {
    double left = parse_factor(p);
    while (1) {
        skip_whitespace(p);
        char op = p->input[p->pos];
        if (op == '*') {
            p->pos++;
            left *= parse_factor(p);
        } else if (op == '/') {
            p->pos++;
            left /= parse_factor(p);
        } else {
            break;
        }
    }
    return left;
}

static double parse_expr(Parser *p) {
    double left = parse_term(p);
    while (1) {
        skip_whitespace(p);
        char op = p->input[p->pos];
        if (op == '+') {
            p->pos++;
            left += parse_term(p);
        } else if (op == '-') {
            p->pos++;
            left -= parse_term(p);
        } else {
            break;
        }
    }
    return left;
}

static double evaluate_expression(const char *expr) {
    Parser p = { .input = expr, .pos = 0, .error = 0 };
    double result = parse_expr(&p);
    return p.error ? NAN : result;
}

/* ============================================================================
   SECTION 13: COROUTINE-LIKE STATE MACHINE
   ============================================================================ */

typedef struct {
    int state;
    int value;
    int limit;
    int step;
} Generator;

#define GEN_BEGIN(g) switch((g)->state) { case 0:
#define GEN_YIELD(g, v) do { (g)->state = __LINE__; (g)->value = (v); return 1; case __LINE__:; } while(0)
#define GEN_END(g) } (g)->state = -1; return 0

static int fibonacci_generator(Generator *g) {
    static int a, b, temp;
    GEN_BEGIN(g);
    a = 0; b = 1;
    while (g->limit-- > 0) {
        GEN_YIELD(g, a);
        temp = a + b;
        a = b;
        b = temp;
    }
    GEN_END(g);
}

static int range_generator(Generator *g) {
    static int current;
    GEN_BEGIN(g);
    current = 0;
    while (current < g->limit) {
        GEN_YIELD(g, current);
        current += g->step;
    }
    GEN_END(g);
}

/* ============================================================================
   SECTION 14: SELF-MODIFYING JUMP TABLE
   ============================================================================ */

typedef enum {
    STATE_INIT,
    STATE_PROCESS,
    STATE_ACCUMULATE,
    STATE_OUTPUT,
    STATE_CLEANUP,
    STATE_DONE,
    STATE_COUNT
} State;

typedef State (*state_fn_t)(void *ctx);

typedef struct {
    int accumulator;
    int counter;
    int input[10];
    int output[10];
    size_t input_idx;
    size_t output_idx;
    state_fn_t transitions[STATE_COUNT];
} StateMachine;

static State sm_init(void *ctx) {
    StateMachine *sm = ctx;
    sm->accumulator = 0;
    sm->counter = 0;
    sm->input_idx = 0;
    sm->output_idx = 0;
    return STATE_PROCESS;
}

static State sm_process(void *ctx) {
    StateMachine *sm = ctx;
    if (sm->input_idx >= 10) return STATE_CLEANUP;
    sm->counter = sm->input[sm->input_idx++];
    return STATE_ACCUMULATE;
}

static State sm_accumulate(void *ctx) {
    StateMachine *sm = ctx;
    sm->accumulator += sm->counter;
    return STATE_OUTPUT;
}

static State sm_output(void *ctx) {
    StateMachine *sm = ctx;
    if (sm->output_idx < 10) {
        sm->output[sm->output_idx++] = sm->accumulator;
    }
    return STATE_PROCESS;
}

static State sm_cleanup(void *ctx) {
    (void)ctx;
    return STATE_DONE;
}

static State sm_done(void *ctx) {
    (void)ctx;
    return STATE_DONE;
}

static void run_state_machine(StateMachine *sm) {
    sm->transitions[STATE_INIT] = sm_init;
    sm->transitions[STATE_PROCESS] = sm_process;
    sm->transitions[STATE_ACCUMULATE] = sm_accumulate;
    sm->transitions[STATE_OUTPUT] = sm_output;
    sm->transitions[STATE_CLEANUP] = sm_cleanup;
    sm->transitions[STATE_DONE] = sm_done;
    
    State current = STATE_INIT;
    while (current != STATE_DONE) {
        current = sm->transitions[current](sm);
    }
}

/* ============================================================================
   SECTION 15: MAIN FUNCTION - ORCHESTRATE ALL TESTS
   ============================================================================ */

int main(void) {
    printf("=== C99 COMPILER STRESS TEST ===\n\n");
    
    /* Test 1: Macro argument counting */
    printf("[1] Macro argument counting:\n");
    printf("    NARGS(a,b,c,d,e) = %d\n", NARGS(a,b,c,d,e));
    printf("    NARGS(x) = %d\n", NARGS(x));
    printf("    NARGS(1,2,3,4,5,6,7,8,9,10) = %d\n\n", NARGS(1,2,3,4,5,6,7,8,9,10));
    
    /* Test 2: Deep pointer chains */
    printf("[2] Deep pointer chain (8 levels):\n");
    ptr8_t deep_ptr = build_ptr_chain_8(42);
    if (deep_ptr) {
        printf("    Value through 8-level deref: %d\n\n", DEREF8(deep_ptr));
        free_ptr_chain_8(deep_ptr);
    }
    
    /* Test 3: Complex numbers */
    printf("[3] Complex number operations:\n");
    double complex z1 = 3.0 + 4.0 * I;
    double complex z2 = 1.0 - 2.0 * I;
    custom_printf("    z1 = %c\n", z1);
    custom_printf("    z2 = %c\n", z2);
    custom_printf("    z1 * z2 = %c\n", cx_mul(z1, z2));
    custom_printf("    z1^3 = %c\n\n", cx_pow_int(z1, 3));
    
    /* Test 4: Mandelbrot escape time */
    printf("[4] Mandelbrot escape times:\n");
    double complex points[] = { 0.0, 0.5 + 0.5*I, -0.5 + 0.5*I, -2.0 };
    for (size_t i = 0; i < sizeof(points)/sizeof(points[0]); i++) {
        int escape = mandelbrot_escape(points[i], 100);
        printf("    Point (%.1f, %.1f): escape = %d\n", 
               creal(points[i]), cimag(points[i]), escape);
    }
    printf("\n");
    
    /* Test 5: VLA matrix multiplication */
    printf("[5] VLA matrix multiplication (3x3):\n");
    {
        size_t n = 3;
        double a[3][3] = {{1,2,3},{4,5,6},{7,8,9}};
        double b[3][3] = {{9,8,7},{6,5,4},{3,2,1}};
        double c[3][3];
        matrix_multiply_vla(n, n, n, a, b, c);
        printf("    Result[0][0] = %.0f, Result[1][1] = %.0f, Result[2][2] = %.0f\n\n",
               c[0][0], c[1][1], c[2][2]);
    }
    
    /* Test 6: Nested VLA computation */
    printf("[6] Nested VLA computation (n=5):\n");
    printf("    Result = %f\n\n", nested_vla_computation(5));
    
    /* Test 7: Deep nested structure */
    printf("[7] Deep nested structure creation:\n");
    struct DeepNesting deep = create_deep_struct(123);
    printf("    level0 = %d\n", deep.level0);
    printf("    level8_final = %d\n\n", 
           deep.level1.level2.level3.level4.level5.level6.level7_struct.deepest.level8_final);
    
    /* Test 8: Variadic typed sum */
    printf("[8] Variadic typed value sum:\n");
    double sum = sum_typed_values(
        TV_INT(10),
        TV_DOUBLE(3.14),
        TV_LONG(1000L),
        TV_INT(-5),
        TV_DOUBLE(2.86),
        TV_END
    );
    printf("    Sum = %f\n\n", sum);
    
    /* Test 9: Expression tree evaluation */
    printf("[9] Expression tree evaluation:\n");
    Expr c1 = EXPR_CONST(3.0);
    Expr c2 = EXPR_CONST(4.0);
    Expr add = EXPR_ADD(c1, c2);
    Expr c3 = EXPR_CONST(2.0);
    Expr mul = EXPR_MUL(add, c3);
    printf("    (3 + 4) * 2 = %f\n\n", mul.eval(&mul));
    
    /* Test 10: Bit manipulation */
    printf("[10] Bit manipulation:\n");
    uint32_t test_val = 0xDEADBEEF;
    custom_printf("    Original:  %B\n", test_val);
    custom_printf("    Reversed:  %B\n", reverse_bits_32(test_val));
    printf("    Popcount:  %d\n", popcount_32(test_val));
    printf("    First set: %d\n\n", ffs_32(test_val));
    
    /* Test 11: Morton encoding */
    printf("[11] Morton encoding (Z-order):\n");
    printf("    morton(3, 5) = 0x%016llX\n\n", 
           (unsigned long long)morton_encode_2d(3, 5));
    
    /* Test 12: Hex float constants */
    printf("[12] Hexadecimal float constants:\n");
    printf("    0x1.0p0 = %f\n", hex_float_constants[0]);
    printf("    0x1.921fb54442d18p+1 (pi) = %.15f\n\n", hex_float_constants[7]);
    
    /* Test 13: Expression parser */
    printf("[13] Expression parser:\n");
    const char *expr = "3 + 4 * 2 - 1";
    printf("    \"%s\" = %f\n\n", expr, evaluate_expression(expr));
    
    /* Test 14: Generator/coroutine */
    printf("[14] Fibonacci generator:\n");
    Generator fib = { .state = 0, .limit = 10 };
    printf("    ");
    while (fibonacci_generator(&fib)) {
        printf("%d ", fib.value);
    }
    printf("\n\n");
    
    /* Test 15: State machine */
    printf("[15] State machine:\n");
    StateMachine sm = {0};
    for (int i = 0; i < 10; i++) sm.input[i] = i + 1;
    run_state_machine(&sm);
    printf("    Running sums: ");
    for (size_t i = 0; i < sm.output_idx; i++) {
        printf("%d ", sm.output[i]);
    }
    printf("\n\n");
    
    /* Test 16: VM execution */
    printf("[16] Virtual machine execution:\n");
    VMState vm = {0};
    INIT_VM_HANDLERS(&vm);
    vm.registers[1] = 10;
    vm.registers[2] = 20;
    Instruction instr = { .op = OP_ADD, .operands = {0, 1, 2} };
    vm.handlers[instr.op](&vm, &instr);
    printf("    R0 = R1 + R2 = %lld + %lld = %lld\n\n",
           (long long)vm.registers[1], (long long)vm.registers[2], (long long)vm.registers[0]);
    
    printf("=== ALL TESTS COMPLETED ===\n");
    
    return 0;
}