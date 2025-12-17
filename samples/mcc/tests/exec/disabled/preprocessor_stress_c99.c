/*
 * EXTREME C99 PREPROCESSOR STRESS TEST
 * =====================================
 * 
 * Compile with: gcc -std=c99 -E preprocessor_stress.c > expanded.c
 *               gcc -std=c99 -Wall -Wextra -pedantic preprocessor_stress.c -o preprocessor_stress
 * 
 * Tests:
 *  - Deep recursive macro expansion
 *  - Deferred expansion tricks
 *  - Token pasting chains
 *  - Stringification edge cases
 *  - Variadic macros with complex patterns
 *  - Argument counting (0 to 64+)
 *  - MAP/FOREACH macro patterns
 *  - Preprocessor arithmetic
 *  - Conditional compilation complexity
 *  - X-Macro code generation
 *  - Self-referential macro detection
 *  - Blue paint (macro recursion blocking)
 *  - CHAOS PP style techniques
 *  - Computed macro names
 *  - Boolean logic in preprocessor
 *  - Tuple manipulation
 *  - Sequence iteration
 */

int putchar(int c);

/* ============================================================================
   HELPER FUNCTIONS (minimal runtime)
   ============================================================================ */

static void print_str(const char *s) {
    while (*s) putchar(*s++);
}

static void print_int(int n) {
    char buf[16];
    int i = 0;
    int neg = 0;
    
    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) buf[i++] = '0';
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    if (neg) buf[i++] = '-';
    while (i > 0) putchar(buf[--i]);
}

static void print_hex(unsigned int n) {
    const char *hex = "0123456789ABCDEF";
    int i;
    print_str("0x");
    for (i = 28; i >= 0; i -= 4) {
        putchar(hex[(n >> i) & 0xF]);
    }
}

static void newline(void) { putchar('\n'); }

static void print_ok(void) { print_str("OK"); }
static void print_fail(void) { print_str("FAIL"); }

/* ============================================================================
   SECTION 1: FUNDAMENTAL BUILDING BLOCKS
   ============================================================================ */

/* Empty and identity */
#define PP_EMPTY()
#define PP_IDENTITY(...) __VA_ARGS__
#define PP_EAT(...)
#define PP_EXPAND(...) __VA_ARGS__
#define PP_OBSTRUCT(...) __VA_ARGS__ PP_DEFER1(PP_EMPTY)()

/* Deferred expansion - critical for recursion */
#define PP_DEFER1(m) m PP_EMPTY()
#define PP_DEFER2(m) m PP_EMPTY PP_EMPTY()()
#define PP_DEFER3(m) m PP_EMPTY PP_EMPTY PP_EMPTY()()()
#define PP_DEFER4(m) m PP_EMPTY PP_EMPTY PP_EMPTY PP_EMPTY()()()()

/* Multi-level expansion */
#define PP_EVAL(...)  PP_EVAL1(PP_EVAL1(PP_EVAL1(__VA_ARGS__)))
#define PP_EVAL1(...) PP_EVAL2(PP_EVAL2(PP_EVAL2(__VA_ARGS__)))
#define PP_EVAL2(...) PP_EVAL3(PP_EVAL3(PP_EVAL3(__VA_ARGS__)))
#define PP_EVAL3(...) PP_EVAL4(PP_EVAL4(PP_EVAL4(__VA_ARGS__)))
#define PP_EVAL4(...) PP_EVAL5(PP_EVAL5(PP_EVAL5(__VA_ARGS__)))
#define PP_EVAL5(...) __VA_ARGS__

/* Even deeper evaluation for extreme cases */
#define PP_EVAL_DEEP(...)  PP_EVAL_D1(PP_EVAL_D1(PP_EVAL_D1(PP_EVAL_D1(__VA_ARGS__))))
#define PP_EVAL_D1(...) PP_EVAL_D2(PP_EVAL_D2(PP_EVAL_D2(PP_EVAL_D2(__VA_ARGS__))))
#define PP_EVAL_D2(...) PP_EVAL_D3(PP_EVAL_D3(PP_EVAL_D3(PP_EVAL_D3(__VA_ARGS__))))
#define PP_EVAL_D3(...) PP_EVAL_D4(PP_EVAL_D4(PP_EVAL_D4(PP_EVAL_D4(__VA_ARGS__))))
#define PP_EVAL_D4(...) __VA_ARGS__

/* ============================================================================
   SECTION 2: TOKEN PASTING MACHINERY
   ============================================================================ */

#define PP_CAT(a, b) a ## b
#define PP_CAT_(a, b) PP_CAT(a, b)
#define PP_CAT3(a, b, c) PP_CAT_(PP_CAT_(a, b), c)
#define PP_CAT4(a, b, c, d) PP_CAT_(PP_CAT3(a, b, c), d)
#define PP_CAT5(a, b, c, d, e) PP_CAT_(PP_CAT4(a, b, c, d), e)
#define PP_CAT6(a, b, c, d, e, f) PP_CAT_(PP_CAT5(a, b, c, d, e), f)
#define PP_CAT7(a, b, c, d, e, f, g) PP_CAT_(PP_CAT6(a, b, c, d, e, f), g)
#define PP_CAT8(a, b, c, d, e, f, g, h) PP_CAT_(PP_CAT7(a, b, c, d, e, f, g), h)

/* Stringification */
#define PP_STR(x) #x
#define PP_XSTR(x) PP_STR(x)

/* ============================================================================
   SECTION 3: BOOLEAN LOGIC
   ============================================================================ */

/* Probe mechanism for detection */
#define PP_PROBE() ~, 1
#define PP_IS_PROBE(...) PP_SECOND(__VA_ARGS__, 0)

#define PP_FIRST(a, ...) a
#define PP_SECOND(a, b, ...) b
#define PP_THIRD(a, b, c, ...) c

/* NOT operation */
#define PP_NOT(x) PP_IS_PROBE(PP_CAT_(PP_NOT_, x))
#define PP_NOT_0 PP_PROBE()

/* Boolean conversion */
#define PP_BOOL(x) PP_NOT(PP_NOT(x))

/* Complement */
#define PP_COMPL(x) PP_CAT_(PP_COMPL_, x)
#define PP_COMPL_0 1
#define PP_COMPL_1 0

/* Binary operations */
#define PP_AND(a, b) PP_CAT3(PP_AND_, a, b)
#define PP_AND_00 0
#define PP_AND_01 0
#define PP_AND_10 0
#define PP_AND_11 1

#define PP_OR(a, b) PP_CAT3(PP_OR_, a, b)
#define PP_OR_00 0
#define PP_OR_01 1
#define PP_OR_10 1
#define PP_OR_11 1

#define PP_XOR(a, b) PP_CAT3(PP_XOR_, a, b)
#define PP_XOR_00 0
#define PP_XOR_01 1
#define PP_XOR_10 1
#define PP_XOR_11 0

#define PP_NAND(a, b) PP_COMPL(PP_AND(a, b))
#define PP_NOR(a, b) PP_COMPL(PP_OR(a, b))
#define PP_XNOR(a, b) PP_COMPL(PP_XOR(a, b))

/* Implication: a -> b = !a || b */
#define PP_IMPLIES(a, b) PP_OR(PP_COMPL(a), b)

/* ============================================================================
   SECTION 4: CONDITIONAL MACROS
   ============================================================================ */

#define PP_IF(cond) PP_CAT_(PP_IF_, PP_BOOL(cond))
#define PP_IF_0(t, f) f
#define PP_IF_1(t, f) t

#define PP_IF_ELSE(cond) PP_CAT_(PP_IF_ELSE_, PP_BOOL(cond))
#define PP_IF_ELSE_0(...) PP_IF_ELSE_0_ELSE
#define PP_IF_ELSE_1(...) __VA_ARGS__ PP_IF_ELSE_1_ELSE
#define PP_IF_ELSE_0_ELSE(...) __VA_ARGS__
#define PP_IF_ELSE_1_ELSE(...)

/* When/Unless */
#define PP_WHEN(cond) PP_IF(cond)(PP_EXPAND, PP_EAT)
#define PP_UNLESS(cond) PP_IF(cond)(PP_EAT, PP_EXPAND)

/* ============================================================================
   SECTION 5: ARGUMENT COUNTING (0 to 64)
   ============================================================================ */

#define PP_ARG64( \
    _1,  _2,  _3,  _4,  _5,  _6,  _7,  _8,  _9,  _10, \
    _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, \
    _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, \
    _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, \
    _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, \
    _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, \
    _61, _62, _63, _64, N, ...) N

#define PP_RSEQ64() \
    64, 63, 62, 61, 60, 59, 58, 57, 56, 55, \
    54, 53, 52, 51, 50, 49, 48, 47, 46, 45, \
    44, 43, 42, 41, 40, 39, 38, 37, 36, 35, \
    34, 33, 32, 31, 30, 29, 28, 27, 26, 25, \
    24, 23, 22, 21, 20, 19, 18, 17, 16, 15, \
    14, 13, 12, 11, 10,  9,  8,  7,  6,  5, \
     4,  3,  2,  1,  0

#define PP_NARG_(...) PP_ARG64(__VA_ARGS__)
#define PP_NARG(...) PP_NARG_(__VA_ARGS__, PP_RSEQ64())

/* Check for zero arguments (tricky!) */
#define PP_HAS_COMMA(...) PP_ARG64(__VA_ARGS__, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 0)

#define PP_TRIGGER_PAREN_(...) ,
#define PP_IS_EMPTY_CASE_0001 ,

#define PP_IS_EMPTY(...) \
    PP_HAS_COMMA(PP_CAT5( \
        PP_IS_EMPTY_CASE_, \
        PP_HAS_COMMA(__VA_ARGS__), \
        PP_HAS_COMMA(PP_TRIGGER_PAREN_ __VA_ARGS__), \
        PP_HAS_COMMA(__VA_ARGS__ (/*empty*/)), \
        PP_HAS_COMMA(PP_TRIGGER_PAREN_ __VA_ARGS__ (/*empty*/)) \
    ))

/* ============================================================================
   SECTION 6: PREPROCESSOR ARITHMETIC
   ============================================================================ */

/* Increment (0-255) */
#define PP_INC(n) PP_CAT_(PP_INC_, n)
#define PP_INC_0 1
#define PP_INC_1 2
#define PP_INC_2 3
#define PP_INC_3 4
#define PP_INC_4 5
#define PP_INC_5 6
#define PP_INC_6 7
#define PP_INC_7 8
#define PP_INC_8 9
#define PP_INC_9 10
#define PP_INC_10 11
#define PP_INC_11 12
#define PP_INC_12 13
#define PP_INC_13 14
#define PP_INC_14 15
#define PP_INC_15 16
#define PP_INC_16 17
#define PP_INC_17 18
#define PP_INC_18 19
#define PP_INC_19 20
#define PP_INC_20 21
#define PP_INC_21 22
#define PP_INC_22 23
#define PP_INC_23 24
#define PP_INC_24 25
#define PP_INC_25 26
#define PP_INC_26 27
#define PP_INC_27 28
#define PP_INC_28 29
#define PP_INC_29 30
#define PP_INC_30 31
#define PP_INC_31 32
#define PP_INC_32 33

/* Decrement (0-32) */
#define PP_DEC(n) PP_CAT_(PP_DEC_, n)
#define PP_DEC_0 0
#define PP_DEC_1 0
#define PP_DEC_2 1
#define PP_DEC_3 2
#define PP_DEC_4 3
#define PP_DEC_5 4
#define PP_DEC_6 5
#define PP_DEC_7 6
#define PP_DEC_8 7
#define PP_DEC_9 8
#define PP_DEC_10 9
#define PP_DEC_11 10
#define PP_DEC_12 11
#define PP_DEC_13 12
#define PP_DEC_14 13
#define PP_DEC_15 14
#define PP_DEC_16 15
#define PP_DEC_17 16
#define PP_DEC_18 17
#define PP_DEC_19 18
#define PP_DEC_20 19
#define PP_DEC_21 20
#define PP_DEC_22 21
#define PP_DEC_23 22
#define PP_DEC_24 23
#define PP_DEC_25 24
#define PP_DEC_26 25
#define PP_DEC_27 26
#define PP_DEC_28 27
#define PP_DEC_29 28
#define PP_DEC_30 29
#define PP_DEC_31 30
#define PP_DEC_32 31

/* Comparison */
#define PP_IS_ZERO(n) PP_NOT(PP_BOOL(n))
#define PP_IS_NONZERO(n) PP_BOOL(n)

/* Equality check (limited range) */
#define PP_CHECK_N(n) PP_CHECK_N_(n)
#define PP_CHECK_N_(n) PP_CAT_(CHECK_RESULT_, n)
#define CHECK_RESULT_PP_PROBE() 1
#define PP_CHECK(...) PP_CHECK_N(PP_PROBE __VA_ARGS__ ())

#define PP_EQUAL(a, b) PP_CAT_(PP_EQUAL_, PP_CAT_(a, _##b))
#define PP_EQUAL_0_0 1
#define PP_EQUAL_1_1 1
#define PP_EQUAL_2_2 1
#define PP_EQUAL_3_3 1
#define PP_EQUAL_4_4 1
#define PP_EQUAL_5_5 1
#define PP_EQUAL_6_6 1
#define PP_EQUAL_7_7 1
#define PP_EQUAL_8_8 1
#define PP_EQUAL_9_9 1
#define PP_EQUAL_10_10 1

/* Default to 0 for non-matching */
#define PP_EQUAL_0_1 0
#define PP_EQUAL_0_2 0
#define PP_EQUAL_1_0 0
#define PP_EQUAL_1_2 0
#define PP_EQUAL_2_0 0
#define PP_EQUAL_2_1 0

/* ============================================================================
   SECTION 7: TUPLE OPERATIONS
   ============================================================================ */

#define PP_TUPLE_SIZE(tuple) PP_NARG tuple
#define PP_TUPLE_ELEM(n, tuple) PP_CAT_(PP_TUPLE_ELEM_, n) tuple

#define PP_TUPLE_ELEM_0(e0, ...) e0
#define PP_TUPLE_ELEM_1(e0, e1, ...) e1
#define PP_TUPLE_ELEM_2(e0, e1, e2, ...) e2
#define PP_TUPLE_ELEM_3(e0, e1, e2, e3, ...) e3
#define PP_TUPLE_ELEM_4(e0, e1, e2, e3, e4, ...) e4
#define PP_TUPLE_ELEM_5(e0, e1, e2, e3, e4, e5, ...) e5
#define PP_TUPLE_ELEM_6(e0, e1, e2, e3, e4, e5, e6, ...) e6
#define PP_TUPLE_ELEM_7(e0, e1, e2, e3, e4, e5, e6, e7, ...) e7

#define PP_TUPLE_HEAD(tuple) PP_TUPLE_ELEM_0 tuple
#define PP_TUPLE_TAIL(tuple) PP_TUPLE_TAIL_ tuple
#define PP_TUPLE_TAIL_(e0, ...) (__VA_ARGS__)

/* Tuple to variadic and back */
#define PP_TUPLE_TO_SEQ(tuple) PP_IDENTITY tuple
#define PP_SEQ_TO_TUPLE(...) (__VA_ARGS__)

/* ============================================================================
   SECTION 8: MAP/FOREACH MACROS
   ============================================================================ */

/* Map termination detection */
#define PP_MAP_END(...)
#define PP_MAP_OUT

#define PP_MAP_GET_END2() 0, PP_MAP_END
#define PP_MAP_GET_END1(...) PP_MAP_GET_END2
#define PP_MAP_GET_END(...) PP_MAP_GET_END1

#define PP_MAP_NEXT0(test, next, ...) next PP_MAP_OUT
#define PP_MAP_NEXT1(test, next) PP_MAP_NEXT0(test, next, 0)
#define PP_MAP_NEXT(test, next) PP_MAP_NEXT1(PP_MAP_GET_END test, next)

/* Basic MAP - applies f to each argument */
#define PP_MAP0(f, x, peek, ...) f(x) PP_MAP_NEXT(peek, PP_MAP1)(f, peek, __VA_ARGS__)
#define PP_MAP1(f, x, peek, ...) f(x) PP_MAP_NEXT(peek, PP_MAP0)(f, peek, __VA_ARGS__)
#define PP_MAP(f, ...) PP_EVAL(PP_MAP1(f, __VA_ARGS__, ()()(), ()()(), ()()(), 0))

/* MAP with comma separator */
#define PP_MAP_LIST_NEXT1(test, next) PP_MAP_NEXT0(test, PP_MAP_COMMA next, 0)
#define PP_MAP_LIST_NEXT(test, next) PP_MAP_LIST_NEXT1(PP_MAP_GET_END test, next)
#define PP_MAP_COMMA ,

#define PP_MAP_LIST0(f, x, peek, ...) f(x) PP_MAP_LIST_NEXT(peek, PP_MAP_LIST1)(f, peek, __VA_ARGS__)
#define PP_MAP_LIST1(f, x, peek, ...) f(x) PP_MAP_LIST_NEXT(peek, PP_MAP_LIST0)(f, peek, __VA_ARGS__)
#define PP_MAP_LIST(f, ...) PP_EVAL(PP_MAP_LIST1(f, __VA_ARGS__, ()()(), ()()(), ()()(), 0))

/* MAP with index */
#define PP_MAP_IDX0(f, i, x, peek, ...) f(i, x) PP_MAP_NEXT(peek, PP_MAP_IDX1)(f, PP_INC(i), peek, __VA_ARGS__)
#define PP_MAP_IDX1(f, i, x, peek, ...) f(i, x) PP_MAP_NEXT(peek, PP_MAP_IDX0)(f, PP_INC(i), peek, __VA_ARGS__)
#define PP_MAP_IDX(f, ...) PP_EVAL(PP_MAP_IDX1(f, 0, __VA_ARGS__, ()()(), ()()(), ()()(), 0))

/* MAP with two arguments per iteration */
#define PP_MAP2_0(f, x, y, peek, ...) f(x, y) PP_MAP_NEXT(peek, PP_MAP2_1)(f, peek, __VA_ARGS__)
#define PP_MAP2_1(f, x, y, peek, ...) f(x, y) PP_MAP_NEXT(peek, PP_MAP2_0)(f, peek, __VA_ARGS__)
#define PP_MAP_PAIRS(f, ...) PP_EVAL(PP_MAP2_1(f, __VA_ARGS__, ()()(), ()()(), ()()(), 0))

/* ============================================================================
   SECTION 9: REPEAT/LOOP MACROS
   ============================================================================ */

/* Repeat N times */
#define PP_REPEAT_0(m, ...)
#define PP_REPEAT_1(m, ...) m(__VA_ARGS__)
#define PP_REPEAT_2(m, ...) PP_REPEAT_1(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_3(m, ...) PP_REPEAT_2(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_4(m, ...) PP_REPEAT_3(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_5(m, ...) PP_REPEAT_4(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_6(m, ...) PP_REPEAT_5(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_7(m, ...) PP_REPEAT_6(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_8(m, ...) PP_REPEAT_7(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_9(m, ...) PP_REPEAT_8(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_10(m, ...) PP_REPEAT_9(m, __VA_ARGS__) m(__VA_ARGS__)
#define PP_REPEAT_16(m, ...) PP_REPEAT_8(m, __VA_ARGS__) PP_REPEAT_8(m, __VA_ARGS__)
#define PP_REPEAT_32(m, ...) PP_REPEAT_16(m, __VA_ARGS__) PP_REPEAT_16(m, __VA_ARGS__)

#define PP_REPEAT(n, m, ...) PP_CAT_(PP_REPEAT_, n)(m, __VA_ARGS__)

/* Repeat with counter */
#define PP_REPEAT_I_0(m, d)
#define PP_REPEAT_I_1(m, d) m(0, d)
#define PP_REPEAT_I_2(m, d) PP_REPEAT_I_1(m, d) m(1, d)
#define PP_REPEAT_I_3(m, d) PP_REPEAT_I_2(m, d) m(2, d)
#define PP_REPEAT_I_4(m, d) PP_REPEAT_I_3(m, d) m(3, d)
#define PP_REPEAT_I_5(m, d) PP_REPEAT_I_4(m, d) m(4, d)
#define PP_REPEAT_I_6(m, d) PP_REPEAT_I_5(m, d) m(5, d)
#define PP_REPEAT_I_7(m, d) PP_REPEAT_I_6(m, d) m(6, d)
#define PP_REPEAT_I_8(m, d) PP_REPEAT_I_7(m, d) m(7, d)
#define PP_REPEAT_I_9(m, d) PP_REPEAT_I_8(m, d) m(8, d)
#define PP_REPEAT_I_10(m, d) PP_REPEAT_I_9(m, d) m(9, d)

#define PP_REPEAT_I(n, m, d) PP_CAT_(PP_REPEAT_I_, n)(m, d)

/* ============================================================================
   SECTION 10: X-MACRO CODE GENERATION
   ============================================================================ */

/* Color definitions */
#define COLOR_LIST \
    X(RED,     0xFF0000, "Red") \
    X(GREEN,   0x00FF00, "Green") \
    X(BLUE,    0x0000FF, "Blue") \
    X(YELLOW,  0xFFFF00, "Yellow") \
    X(CYAN,    0x00FFFF, "Cyan") \
    X(MAGENTA, 0xFF00FF, "Magenta") \
    X(WHITE,   0xFFFFFF, "White") \
    X(BLACK,   0x000000, "Black")

/* Generate enum */
typedef enum {
#define X(name, val, str) COLOR_##name,
    COLOR_LIST
#undef X
    COLOR_COUNT
} Color;

/* Generate value table */
static const unsigned int color_values[] = {
#define X(name, val, str) val,
    COLOR_LIST
#undef X
};

/* Generate name table */
static const char *color_names[] = {
#define X(name, val, str) str,
    COLOR_LIST
#undef X
};

/* Error codes with severity */
#define ERROR_LIST \
    X(OK,           0, 0, "Success") \
    X(WARN_MINOR,   1, 1, "Minor warning") \
    X(WARN_MAJOR,   2, 1, "Major warning") \
    X(ERR_INPUT,    3, 2, "Input error") \
    X(ERR_MEMORY,   4, 2, "Memory error") \
    X(ERR_IO,       5, 2, "I/O error") \
    X(ERR_FATAL,    6, 3, "Fatal error")

typedef enum {
#define X(name, code, sev, msg) ERR_##name = code,
    ERROR_LIST
#undef X
} ErrorCode;

static const int error_severity[] = {
#define X(name, code, sev, msg) sev,
    ERROR_LIST
#undef X
};

static const char *error_messages[] = {
#define X(name, code, sev, msg) msg,
    ERROR_LIST
#undef X
};

/* ============================================================================
   SECTION 11: COMPUTED MACRO NAMES
   ============================================================================ */

/* Type-specific operations */
#define OP_ADD_int(a, b) ((a) + (b))
#define OP_ADD_float(a, b) ((a) + (b))
#define OP_SUB_int(a, b) ((a) - (b))
#define OP_SUB_float(a, b) ((a) - (b))
#define OP_MUL_int(a, b) ((a) * (b))
#define OP_MUL_float(a, b) ((a) * (b))

#define TYPED_OP(op, type, a, b) PP_CAT4(OP_, op, _, type)(a, b)

/* Version-specific behavior */
#define VERSION 2
#define BEHAVIOR_1_init() 100
#define BEHAVIOR_2_init() 200
#define BEHAVIOR_3_init() 300
#define GET_INIT_VALUE() PP_CAT3(BEHAVIOR_, VERSION, _init)()

/* Platform detection simulation */
#define PLATFORM_linux 1
#define PLATFORM_windows 2
#define PLATFORM_macos 3

#define CURRENT_PLATFORM linux
#define PLATFORM_ID PP_CAT_(PLATFORM_, CURRENT_PLATFORM)

/* ============================================================================
   SECTION 12: RECURSIVE WHILE LOOP
   ============================================================================ */

/* While loop implementation using deferred expansion */
#define PP_WHILE(pred, op, state) \
    PP_WHILE_INDIRECT()(pred, op, state)

#define PP_WHILE_INDIRECT() PP_WHILE_

#define PP_WHILE_(pred, op, state) \
    PP_IF(pred(state)) \
    ( \
        PP_OBSTRUCT(PP_WHILE_INDIRECT)()(pred, op, op(state)), \
        state \
    )

/* Example: count down to zero */
#define PRED_NONZERO(state) PP_BOOL(PP_TUPLE_ELEM(0, state))
#define OP_COUNTDOWN(state) (PP_DEC(PP_TUPLE_ELEM(0, state)), PP_INC(PP_TUPLE_ELEM(1, state)))

/* ============================================================================
   SECTION 13: STRINGIFICATION EDGE CASES
   ============================================================================ */

#define MULTILINE_STR(x) #x
#define STR_WITH_HASH(x) PP_STR(x ## _hash)
#define DOUBLE_STR(x) PP_STR(PP_STR(x))
#define EXPANDED_STR(x) PP_XSTR(x)

/* Stringify expressions */
#define EXPR_STR(a, op, b) PP_XSTR(a op b)

/* ============================================================================
   SECTION 14: PREDEFINED MACRO MANIPULATION
   ============================================================================ */

#define LOCATION_STR __FILE__ ":" PP_XSTR(__LINE__)
#define FUNC_INFO __func__

#define COMPILE_TIME_ASSERT(expr, msg) \
    typedef char PP_CAT_(static_assert_, __LINE__)[(expr) ? 1 : -1]

/* ============================================================================
   SECTION 15: COMPLEX CONDITIONAL COMPILATION
   ============================================================================ */

#define FEATURE_A 1
#define FEATURE_B 1
#define FEATURE_C 0
#define DEBUG_LEVEL 2

#if defined(FEATURE_A) && FEATURE_A
    #define HAS_A 1
#else
    #define HAS_A 0
#endif

#if FEATURE_A && FEATURE_B && !FEATURE_C
    #define CONFIG_ABC 1
#else
    #define CONFIG_ABC 0
#endif

#if DEBUG_LEVEL >= 2
    #define DEBUG_PRINT(msg) print_str("[DEBUG] " msg "\n")
#elif DEBUG_LEVEL == 1
    #define DEBUG_PRINT(msg) print_str("[INFO] " msg "\n")
#else
    #define DEBUG_PRINT(msg)
#endif

#if (FEATURE_A || FEATURE_B) && !(FEATURE_A && FEATURE_B && FEATURE_C)
    #define COMPLEX_CONDITION 1
#else
    #define COMPLEX_CONDITION 0
#endif

/* Nested #if */
#if FEATURE_A
    #if FEATURE_B
        #if !FEATURE_C
            #define NESTED_RESULT 1
        #else
            #define NESTED_RESULT 2
        #endif
    #else
        #define NESTED_RESULT 3
    #endif
#else
    #define NESTED_RESULT 0
#endif

/* ============================================================================
   SECTION 16: VA_OPT SIMULATION (C99 compatible)
   ============================================================================ */

/* Simulate __VA_OPT__ for C99 */
#define PP_HAS_ARGS(...) PP_BOOL(PP_NARG(__VA_ARGS__))
#define PP_OPT(has, ...) PP_CAT_(PP_OPT_, has)(__VA_ARGS__)
#define PP_OPT_0(...)
#define PP_OPT_1(...) __VA_ARGS__

/* Conditional comma */
#define PP_COMMA_IF(cond) PP_IF(cond)(PP_COMMA, PP_EMPTY)()
#define PP_COMMA ,

/* ============================================================================
   SECTION 17: AUTOMATIC STRUCT GENERATION
   ============================================================================ */

/* Generate struct fields */
#define FIELD(type, name) type name;
#define STRUCT_FIELDS(name, ...) struct name { PP_MAP(FIELD, __VA_ARGS__) }

/* Generate getter/setter */
#define GETTER(type, name) \
    static type get_##name(const void *obj) { \
        return ((const struct Container *)obj)->name; \
    }

#define SETTER(type, name) \
    static void set_##name(void *obj, type val) { \
        ((struct Container *)obj)->name = val; \
    }

/* Field list */
#define CONTAINER_FIELDS \
    (int, x) \
    (int, y) \
    (int, z)

/* ============================================================================
   SECTION 18: ENUM STRING CONVERSION
   ============================================================================ */

/* Macro to create enum and string array simultaneously */
#define ENUM_CASE(name) case name: return #name;

#define DECLARE_ENUM_WITH_STRINGS(EnumName, ...) \
    typedef enum { __VA_ARGS__ } EnumName; \
    static const char* EnumName##_to_string(EnumName e) { \
        switch(e) { \
            PP_MAP(ENUM_CASE, __VA_ARGS__) \
            default: return "UNKNOWN"; \
        } \
    }

/* ============================================================================
   SECTION 19: MACRO OVERLOADING BY ARGUMENT COUNT
   ============================================================================ */

#define OVERLOAD_1(a) ((a) * 2)
#define OVERLOAD_2(a, b) ((a) + (b))
#define OVERLOAD_3(a, b, c) ((a) + (b) + (c))
#define OVERLOAD_4(a, b, c, d) ((a) + (b) + (c) + (d))

#define OVERLOAD_DISPATCH(n) PP_CAT_(OVERLOAD_, n)
#define OVERLOAD(...) OVERLOAD_DISPATCH(PP_NARG(__VA_ARGS__))(__VA_ARGS__)

/* ============================================================================
   SECTION 20: COMPILE-TIME DATA STRUCTURES
   ============================================================================ */

/* Pair operations */
#define PP_PAIR(a, b) (a, b)
#define PP_PAIR_FIRST(p) PP_TUPLE_ELEM(0, p)
#define PP_PAIR_SECOND(p) PP_TUPLE_ELEM(1, p)
#define PP_PAIR_SWAP(p) PP_PAIR(PP_PAIR_SECOND(p), PP_PAIR_FIRST(p))

/* Simple list (as nested pairs) */
#define PP_NIL ()
#define PP_CONS(h, t) ((h), t)
#define PP_HEAD(list) PP_TUPLE_ELEM(0, list)
#define PP_TAIL(list) PP_TUPLE_ELEM(1, list)

/* ============================================================================
   SECTION 21: TEST HELPER MACROS
   ============================================================================ */

#define TEST_SECTION(n, name) \
    print_str("[" #n "] " name ": ")

#define TEST_ASSERT(cond) \
    do { \
        if (cond) { print_ok(); } \
        else { print_fail(); errors++; } \
        newline(); \
    } while(0)

#define TEST_ASSERT_EQ(a, b) TEST_ASSERT((a) == (b))

#define TEST_PRINT_VAL(name, val) \
    do { \
        print_str("    " name " = "); \
        print_int(val); \
        newline(); \
    } while(0)

/* ============================================================================
   MAIN TEST FUNCTION
   ============================================================================ */

/* Helper macros for tests */
#define SQUARE(x) ((x) * (x))
#define ADD_ONE(x) (x + 1)
#define MAKE_PLUS(a, b) a + b
#define TO_DECL(type, name) type name;
#define INDEXED_VAR(i, x) int PP_CAT_(var_, i) = x;

int main(void) {
    int errors = 0;
    
    print_str("=== EXTREME C99 PREPROCESSOR STRESS TEST ===\n\n");
    
    /* Test 1: Basic macros */
    TEST_SECTION(1, "Basic macros");
    {
        int a = 5;
        int sq = SQUARE(a);
        int inc = ADD_ONE(10);
        TEST_ASSERT(sq == 25 && inc == 11);
    }
    
    /* Test 2: Token pasting */
    TEST_SECTION(2, "Token pasting (CAT)");
    {
        int PP_CAT_(test, _var) = 123;
        int PP_CAT3(a, b, c) = 456;
        int PP_CAT6(v, a, r, i, a, b) = 789;
        TEST_ASSERT(test_var == 123 && abc == 456 && variab == 789);
    }
    
    /* Test 3: Stringification */
    TEST_SECTION(3, "Stringification");
    {
        const char *s1 = PP_STR(hello);
        const char *s2 = PP_XSTR(SQUARE(x));
        /* s1 = "hello", s2 = "((x) * (x))" */
        TEST_ASSERT(s1[0] == 'h' && s2[0] == '(' && s2[1] == '(');
    }
    
    /* Test 4: Argument counting */
    TEST_SECTION(4, "Argument counting");
    {
        int n1 = PP_NARG(a);
        int n3 = PP_NARG(a, b, c);
        int n5 = PP_NARG(a, b, c, d, e);
        int n10 = PP_NARG(1,2,3,4,5,6,7,8,9,10);
        TEST_ASSERT(n1 == 1 && n3 == 3 && n5 == 5 && n10 == 10);
    }
    
    /* Test 5: Boolean logic */
    TEST_SECTION(5, "Boolean logic");
    {
        int not0 = PP_NOT(0);
        int not1 = PP_NOT(1);
        int bool0 = PP_BOOL(0);
        int bool5 = PP_BOOL(5);
        int and_11 = PP_AND(1, 1);
        int and_10 = PP_AND(1, 0);
        int or_00 = PP_OR(0, 0);
        int or_01 = PP_OR(0, 1);
        int xor_01 = PP_XOR(0, 1);
        int xor_11 = PP_XOR(1, 1);
        TEST_ASSERT(not0 == 1 && not1 == 0 && bool0 == 0 && bool5 == 1 &&
                   and_11 == 1 && and_10 == 0 && or_00 == 0 && or_01 == 1 &&
                   xor_01 == 1 && xor_11 == 0);
    }
    
    /* Test 6: Conditionals */
    TEST_SECTION(6, "Conditional macros");
    {
        int if_true = PP_IF(1)(100, 200);
        int if_false = PP_IF(0)(100, 200);
        int when_yes = PP_WHEN(1)(42);
        int compl_0 = PP_COMPL(0);
        int compl_1 = PP_COMPL(1);
        TEST_ASSERT(if_true == 100 && if_false == 200 && 
                   when_yes == 42 && compl_0 == 1 && compl_1 == 0);
    }
    
    /* Test 7: Increment/Decrement */
    TEST_SECTION(7, "Preprocessor arithmetic");
    {
        int inc5 = PP_INC(5);
        int inc15 = PP_INC(15);
        int dec10 = PP_DEC(10);
        int dec1 = PP_DEC(1);
        int inc_inc_0 = PP_INC(PP_INC(0));
        TEST_ASSERT(inc5 == 6 && inc15 == 16 && dec10 == 9 && 
                   dec1 == 0 && inc_inc_0 == 2);
    }
    
    /* Test 8: Tuple operations */
    TEST_SECTION(8, "Tuple operations");
    {
        int size = PP_TUPLE_SIZE((a, b, c, d));
        int e0 = PP_TUPLE_ELEM(0, (10, 20, 30));
        int e1 = PP_TUPLE_ELEM(1, (10, 20, 30));
        int e2 = PP_TUPLE_ELEM(2, (10, 20, 30));
        TEST_ASSERT(size == 4 && e0 == 10 && e1 == 20 && e2 == 30);
    }
    
    /* Test 9: MAP macro */
    TEST_SECTION(9, "MAP macro");
    {
        /* MAP applies ADD_ONE to each element */
        int arr[] = { PP_MAP_LIST(ADD_ONE, 1, 2, 3, 4, 5) };
        /* Should be: 2, 3, 4, 5, 6 */
        TEST_ASSERT(arr[0] == 2 && arr[1] == 3 && arr[2] == 4 && 
                   arr[3] == 5 && arr[4] == 6);
    }
    
    /* Test 10: REPEAT macro */
    TEST_SECTION(10, "REPEAT macro");
    {
        int count = 0;
        #define INC_COUNT(x) count++;
        PP_REPEAT(5, INC_COUNT, dummy)
        #undef INC_COUNT
        TEST_ASSERT(count == 5);
    }
    
    /* Test 11: X-Macro enum */
    TEST_SECTION(11, "X-Macro enum");
    {
        TEST_ASSERT(COLOR_RED == 0 && COLOR_COUNT == 8 &&
                   color_values[COLOR_RED] == 0xFF0000 &&
                   color_values[COLOR_GREEN] == 0x00FF00);
    }
    
    /* Test 12: X-Macro with multiple fields */
    TEST_SECTION(12, "X-Macro multi-field");
    {
        TEST_ASSERT(ERR_OK == 0 && ERR_ERR_FATAL == 6 &&
                   error_severity[ERR_WARN_MINOR] == 1 &&
                   error_severity[ERR_ERR_MEMORY] == 2);
    }
    
    /* Test 13: Computed macro names */
    TEST_SECTION(13, "Computed macro names");
    {
        int add_int = TYPED_OP(ADD, int, 10, 20);
        int mul_int = TYPED_OP(MUL, int, 5, 6);
        int init_val = GET_INIT_VALUE();
        TEST_ASSERT(add_int == 30 && mul_int == 30 && init_val == 200);
    }
    
    /* Test 14: Deep token pasting */
    TEST_SECTION(14, "Deep token pasting");
    {
        #define A1 1
        #define A2 2
        #define A12 12
        int v1 = PP_CAT_(A, 1);
        int v2 = PP_CAT_(A, 2);
        int v12 = PP_CAT_(A, 12);
        TEST_ASSERT(v1 == 1 && v2 == 2 && v12 == 12);
    }
    
    /* Test 15: Conditional compilation results */
    TEST_SECTION(15, "Conditional compilation");
    {
        TEST_ASSERT(HAS_A == 1 && CONFIG_ABC == 1 && 
                   COMPLEX_CONDITION == 1 && NESTED_RESULT == 1);
    }
    
    /* Test 16: Overloading by arg count */
    TEST_SECTION(16, "Macro overloading");
    {
        int o1 = OVERLOAD(5);           /* 5 * 2 = 10 */
        int o2 = OVERLOAD(3, 4);        /* 3 + 4 = 7 */
        int o3 = OVERLOAD(1, 2, 3);     /* 1 + 2 + 3 = 6 */
        int o4 = OVERLOAD(1, 2, 3, 4);  /* 1 + 2 + 3 + 4 = 10 */
        TEST_ASSERT(o1 == 10 && o2 == 7 && o3 == 6 && o4 == 10);
    }
    
    /* Test 17: Pair operations */
    TEST_SECTION(17, "Pair operations");
    {
        #define MY_PAIR (10, 20)
        int first = PP_PAIR_FIRST(MY_PAIR);
        int second = PP_PAIR_SECOND(MY_PAIR);
        #undef MY_PAIR
        TEST_ASSERT(first == 10 && second == 20);
    }
    
    /* Test 18: Platform detection simulation */
    TEST_SECTION(18, "Platform detection");
    {
        int plat_id = PLATFORM_ID;
        TEST_ASSERT(plat_id == 1);  /* linux = 1 */
    }
    
    /* Test 19: MAP with index */
    TEST_SECTION(19, "MAP with index");
    {
        PP_MAP_IDX(INDEXED_VAR, 100, 200, 300)
        TEST_ASSERT(var_0 == 100 && var_1 == 200 && var_2 == 300);
    }
    
    /* Test 20: Complex nested expansion */
    TEST_SECTION(20, "Complex nested expansion");
    {
        #define OUTER(x) INNER(x)
        #define INNER(x) ((x) + 1)
        #define DEEP3(x) OUTER(OUTER(OUTER(x)))
        int deep = DEEP3(5);
        /* DEEP3(5) = OUTER(OUTER(OUTER(5))) = OUTER(OUTER(6)) = OUTER(7) = 8 */
        #undef OUTER
        #undef INNER
        #undef DEEP3
        TEST_ASSERT(deep == 8);
    }
    
    /* Test 21: String expression */
    TEST_SECTION(21, "String expressions");
    {
        const char *expr = EXPR_STR(2, +, 3);
        TEST_ASSERT(expr[0] == '2' && expr[2] == '+' && expr[4] == '3');
    }
    
    /* Test 22: Empty check (advanced) */
    TEST_SECTION(22, "Advanced detection");
    {
        int has_comma_yes = PP_HAS_COMMA(a, b);
        int has_comma_no = PP_HAS_COMMA(single);
        TEST_ASSERT(has_comma_yes == 1 && has_comma_no == 0);
    }
    
    /* Test 23: REPEAT with index */
    TEST_SECTION(23, "REPEAT with index");
    {
        int sum = 0;
        #define ADD_IDX(i, d) sum += i;
        PP_REPEAT_I(5, ADD_IDX, dummy)
        #undef ADD_IDX
        /* 0 + 1 + 2 + 3 + 4 = 10 */
        TEST_ASSERT(sum == 10);
    }
    
    /* Test 24: Deeply expanded string */
    TEST_SECTION(24, "Deep stringification");
    {
        #define VAL 42
        #define INDIRECT_VAL VAL
        const char *s = PP_XSTR(INDIRECT_VAL);
        #undef VAL
        #undef INDIRECT_VAL
        TEST_ASSERT(s[0] == '4' && s[1] == '2');
    }
    
    /* Test 25: Large argument count */
    TEST_SECTION(25, "Large arg count");
    {
        int n20 = PP_NARG(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20);
        int n30 = PP_NARG(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,
                         21,22,23,24,25,26,27,28,29,30);
        TEST_ASSERT(n20 == 20 && n30 == 30);
    }
    
    /* Test 26: NAND, NOR, XNOR gates */
    TEST_SECTION(26, "NAND/NOR/XNOR");
    {
        int nand_11 = PP_NAND(1, 1);
        int nand_10 = PP_NAND(1, 0);
        int nor_00 = PP_NOR(0, 0);
        int nor_01 = PP_NOR(0, 1);
        int xnor_11 = PP_XNOR(1, 1);
        int xnor_10 = PP_XNOR(1, 0);
        TEST_ASSERT(nand_11 == 0 && nand_10 == 1 &&
                   nor_00 == 1 && nor_01 == 0 &&
                   xnor_11 == 1 && xnor_10 == 0);
    }
    
    /* Test 27: Implication */
    TEST_SECTION(27, "Implication (->)");
    {
        /* a -> b is equivalent to !a || b */
        int impl_00 = PP_IMPLIES(0, 0);  /* 1 */
        int impl_01 = PP_IMPLIES(0, 1);  /* 1 */
        int impl_10 = PP_IMPLIES(1, 0);  /* 0 */
        int impl_11 = PP_IMPLIES(1, 1);  /* 1 */
        TEST_ASSERT(impl_00 == 1 && impl_01 == 1 && 
                   impl_10 == 0 && impl_11 == 1);
    }
    
    /* Test 28: Color X-Macro strings */
    TEST_SECTION(28, "X-Macro strings");
    {
        const char *red = color_names[COLOR_RED];
        const char *blue = color_names[COLOR_BLUE];
        TEST_ASSERT(red[0] == 'R' && blue[0] == 'B');
    }
    
    /* Test 29: Chained increments */
    TEST_SECTION(29, "Chained arithmetic");
    {
        /* PP_INC/DEC only work with literal numbers, not variables */
        int v1 = PP_INC(PP_INC(PP_INC(PP_INC(PP_INC(0)))));
        int v2 = PP_DEC(PP_DEC(PP_DEC(10)));
        TEST_ASSERT(v1 == 5 && v2 == 7);
    }
    
    /* Test 30: MAP on empty-ish input */
    TEST_SECTION(30, "Edge cases");
    {
        /* Single element */
        int single[] = { PP_MAP_LIST(ADD_ONE, 99) };
        TEST_ASSERT(single[0] == 100);
    }
    
    /* Summary */
    newline();
    print_str("=== Results: ");
    if (errors == 0) {
        print_str("ALL 30 TESTS PASSED ===\n");
    } else {
        print_int(errors);
        print_str(" test(s) FAILED ===\n");
    }
    
    /* Print some generated values for visual verification */
    newline();
    print_str("=== Generated Values ===\n");
    print_str("Colors defined: "); print_int(COLOR_COUNT); newline();
    print_str("RED value: "); print_hex(color_values[COLOR_RED]); newline();
    print_str("GREEN value: "); print_hex(color_values[COLOR_GREEN]); newline();
    print_str("BLUE value: "); print_hex(color_values[COLOR_BLUE]); newline();
    print_str("Platform ID: "); print_int(PLATFORM_ID); newline();
    print_str("Version init: "); print_int(GET_INIT_VALUE()); newline();
    
    return errors;
}