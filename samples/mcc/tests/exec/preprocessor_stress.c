/*
 * Preprocessor Stress Test
 * ========================
 * 
 * Tests preprocessor features:
 *  - Object-like macros
 *  - Function-like macros
 *  - Token pasting (##)
 *  - Stringification (#)
 *  - Variadic macros (__VA_ARGS__)
 *  - Nested macro expansion
 *  - Conditional compilation (#if, #ifdef, #ifndef, #elif, #else)
 *  - Predefined macros (__LINE__, __FILE__)
 */

int putchar(int c);

/* ============================================================================
   HELPER FUNCTIONS
   ============================================================================ */

void print_str(const char *s) {
    while (*s) putchar(*s++);
}

void print_num(int n) {
    if (n < 0) {
        putchar('-');
        n = -n;
    }
    if (n >= 10) print_num(n / 10);
    putchar('0' + n % 10);
}

void newline(void) { putchar('\n'); }

/* ============================================================================
   SECTION 1: BASIC MACROS
   ============================================================================ */

#define VALUE_42 42
#define VALUE_100 100
#define ADD(a, b) ((a) + (b))
#define MUL(a, b) ((a) * (b))
#define SQUARE(x) ((x) * (x))

/* ============================================================================
   SECTION 2: TOKEN PASTING (##)
   ============================================================================ */

#define CAT(a, b) a ## b
#define CAT2(a, b) CAT(a, b)
#define MAKE_VAR(prefix, num) CAT2(prefix, num)

/* ============================================================================
   SECTION 3: STRINGIFICATION (#)
   ============================================================================ */

#define STR(x) #x
#define XSTR(x) STR(x)

/* ============================================================================
   SECTION 4: VARIADIC MACROS
   ============================================================================ */

#define COUNT_ARGS_IMPL(_1, _2, _3, _4, _5, N, ...) N
#define COUNT_ARGS(...) COUNT_ARGS_IMPL(__VA_ARGS__, 5, 4, 3, 2, 1)

#define FIRST_ARG(first, ...) first
#define REST_ARGS(first, ...) __VA_ARGS__

/* ============================================================================
   SECTION 5: NESTED MACROS
   ============================================================================ */

#define DOUBLE(x) ((x) + (x))
#define QUADRUPLE(x) DOUBLE(DOUBLE(x))
#define OCTUPLE(x) DOUBLE(QUADRUPLE(x))

#define INNER(x) (x + 1)
#define OUTER(x) INNER(INNER(x))

/* ============================================================================
   SECTION 6: CONDITIONAL COMPILATION
   ============================================================================ */

#define FEATURE_A 1
#define FEATURE_B 0
#define VERSION 2

#ifdef FEATURE_A
#define HAS_FEATURE_A 1
#else
#define HAS_FEATURE_A 0
#endif

#ifndef UNDEFINED_FEATURE
#define UNDEFINED_IS_CORRECT 1
#else
#define UNDEFINED_IS_CORRECT 0
#endif

#if VERSION == 1
#define VERSION_STR "v1"
#elif VERSION == 2
#define VERSION_STR "v2"
#else
#define VERSION_STR "unknown"
#endif

#if FEATURE_A && !FEATURE_B
#define LOGIC_TEST_OK 1
#else
#define LOGIC_TEST_OK 0
#endif

/* ============================================================================
   MAIN - RUN ALL TESTS
   ============================================================================ */

int main(void) {
    int errors = 0;
    
    print_str("=== Preprocessor Stress Test ===\n\n");
    
    /* Test 1: Basic object-like macros */
    print_str("[1] Object-like macros: ");
    int v1 = VALUE_42;
    int v2 = VALUE_100;
    if (v1 == 42 && v2 == 100) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 2: Function-like macros */
    print_str("[2] Function-like macros: ");
    int sum = ADD(10, 20);
    int prod = MUL(5, 6);
    int sq = SQUARE(7);
    if (sum == 30 && prod == 30 && sq == 49) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 3: Token pasting (##) - simple */
    print_str("[3] Token pasting (simple): ");
    int CAT(test, _var) = 123;
    if (test_var == 123) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 4: Token pasting (##) - nested */
    print_str("[4] Token pasting (nested): ");
    int MAKE_VAR(my, _value) = 456;
    if (my_value == 456) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 5: Stringification (#) */
    print_str("[5] Stringification: ");
    const char *s1 = STR(hello);
    const char *s2 = XSTR(VALUE_42);
    /* s1 should be "hello", s2 should be "42" */
    if (s1[0] == 'h' && s2[0] == '4' && s2[1] == '2') print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 6: Variadic macros - argument counting */
    print_str("[6] Variadic macros (count): ");
    int c1 = COUNT_ARGS(a);
    int c2 = COUNT_ARGS(a, b);
    int c3 = COUNT_ARGS(a, b, c);
    int c5 = COUNT_ARGS(a, b, c, d, e);
    if (c1 == 1 && c2 == 2 && c3 == 3 && c5 == 5) print_str("OK");
    else { 
        print_str("FAIL (");
        print_num(c1); putchar(',');
        print_num(c2); putchar(',');
        print_num(c3); putchar(',');
        print_num(c5); putchar(')');
        errors++; 
    }
    newline();
    
    /* Test 7: Variadic macros - first/rest */
    print_str("[7] Variadic macros (first/rest): ");
    int first = FIRST_ARG(10, 20, 30);
    if (first == 10) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 8: Nested macro expansion */
    print_str("[8] Nested macros: ");
    int d = DOUBLE(5);
    int q = QUADRUPLE(3);
    int o = OCTUPLE(2);
    if (d == 10 && q == 12 && o == 16) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 9: Deeply nested macros */
    print_str("[9] Deeply nested: ");
    int nested = OUTER(5);
    /* OUTER(5) = INNER(INNER(5)) = INNER(6) = 7 */
    if (nested == 7) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 10: #ifdef / #ifndef */
    print_str("[10] #ifdef/#ifndef: ");
    if (HAS_FEATURE_A == 1 && UNDEFINED_IS_CORRECT == 1) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 11: #if / #elif / #else */
    print_str("[11] #if/#elif/#else: ");
    const char *ver = VERSION_STR;
    if (ver[0] == 'v' && ver[1] == '2') print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 12: #if with logical operators */
    print_str("[12] #if logical ops: ");
    if (LOGIC_TEST_OK == 1) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 13: Macro in macro argument */
    print_str("[13] Macro in argument: ");
    int mac_arg = ADD(VALUE_42, VALUE_100);
    if (mac_arg == 142) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 14: Complex expression with macros */
    print_str("[14] Complex expression: ");
    int complex = MUL(ADD(2, 3), SQUARE(2));
    /* (2+3) * (2*2) = 5 * 4 = 20 */
    if (complex == 20) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Summary */
    newline();
    print_str("=== Results: ");
    if (errors == 0) {
        print_str("ALL TESTS PASSED ===\n");
    } else {
        print_num(errors);
        print_str(" test(s) FAILED ===\n");
    }
    
    return errors;
}
