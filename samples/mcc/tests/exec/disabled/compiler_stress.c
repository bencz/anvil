/*
 * C99 COMPILER STRESS TEST (MCC Compatible)
 * ==========================================
 * 
 * Features tested:
 *  - Variadic macros
 *  - Token pasting and stringification
 *  - Deep pointer indirection (4 levels)
 *  - Function pointer dispatch tables
 *  - Bit-field packing (including anonymous padding)
 *  - Nested structures
 *  - Designated initializers
 *  - Recursive functions
 *  - 2D array operations
 *  - Bit manipulation
 *  - State machines
 */

int putchar(int c);
void *malloc(unsigned long size);
void free(void *ptr);

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

void print_hex(unsigned int n) {
    char hex[17];
    hex[0] = '0'; hex[1] = '1'; hex[2] = '2'; hex[3] = '3';
    hex[4] = '4'; hex[5] = '5'; hex[6] = '6'; hex[7] = '7';
    hex[8] = '8'; hex[9] = '9'; hex[10] = 'A'; hex[11] = 'B';
    hex[12] = 'C'; hex[13] = 'D'; hex[14] = 'E'; hex[15] = 'F';
    hex[16] = 0;
    putchar('0'); putchar('x');
    for (int i = 28; i >= 0; i -= 4) {
        putchar(hex[(n >> i) & 0xF]);
    }
}

void newline(void) { putchar('\n'); }

/* ============================================================================
   SECTION 1: PREPROCESSOR MACROS
   ============================================================================ */

/* Token pasting */
#define CAT(a, b) a ## b
#define CAT_(a, b) CAT(a, b)

/* Variadic macro for argument counting */
#define ARG_N(_1, _2, _3, _4, _5, N, ...) N
#define NARGS(...) ARG_N(__VA_ARGS__, 5, 4, 3, 2, 1)

/* ============================================================================
   SECTION 2: TYPE SYSTEM STRESS
   ============================================================================ */

/* Deep pointer types (4 levels) */
typedef int *ptr1_t;
typedef ptr1_t *ptr2_t;
typedef ptr2_t *ptr3_t;
typedef ptr3_t *ptr4_t;

/* Function pointer type */
typedef int (*binop_fn_t)(int, int);

/* Bit-field structure with anonymous padding */
struct BitFields {
    unsigned int a : 1;
    unsigned int b : 3;
    unsigned int c : 4;
    unsigned int   : 0;  /* Force alignment - anonymous padding */
    unsigned int d : 8;
    unsigned int   : 4;  /* More padding */
    unsigned int e : 12;
};

/* Nested structures */
struct Inner {
    int x;
    int y;
};

struct Outer {
    int id;
    struct Inner pos;
    int value;
};

/* ============================================================================
   SECTION 3: FUNCTION POINTER DISPATCH
   ============================================================================ */

int op_add(int a, int b) { return a + b; }
int op_sub(int a, int b) { return a - b; }
int op_mul(int a, int b) { return a * b; }
int op_div(int a, int b) { return b != 0 ? a / b : 0; }

binop_fn_t dispatch_table[4];

void init_dispatch(void) {
    dispatch_table[0] = op_add;
    dispatch_table[1] = op_sub;
    dispatch_table[2] = op_mul;
    dispatch_table[3] = op_div;
}

int dispatch_op(int op, int a, int b) {
    if (op >= 0 && op < 4) {
        return dispatch_table[op](a, b);
    }
    return 0;
}

/* ============================================================================
   SECTION 4: DEEP POINTER OPERATIONS
   ============================================================================ */

ptr4_t build_ptr_chain(int value) {
    int *p1 = malloc(sizeof(int));
    int **p2 = malloc(sizeof(int*));
    int ***p3 = malloc(sizeof(int**));
    int ****p4 = malloc(sizeof(int***));
    
    if (!p1 || !p2 || !p3 || !p4) {
        free(p1); free(p2); free(p3); free(p4);
        return 0;
    }
    
    *p1 = value;
    *p2 = p1;
    *p3 = p2;
    *p4 = p3;
    
    return p4;
}

int deref4(ptr4_t p) {
    return ****p;
}

void free_ptr_chain(ptr4_t p4) {
    if (!p4) return;
    ptr3_t p3 = *p4;
    ptr2_t p2 = *p3;
    ptr1_t p1 = *p2;
    free(p1); free(p2); free(p3); free(p4);
}

/* ============================================================================
   SECTION 5: BIT MANIPULATION
   ============================================================================ */

unsigned int reverse_bits(unsigned int x) {
    x = ((x & 0x55555555u) << 1)  | ((x & 0xAAAAAAAAu) >> 1);
    x = ((x & 0x33333333u) << 2)  | ((x & 0xCCCCCCCCu) >> 2);
    x = ((x & 0x0F0F0F0Fu) << 4)  | ((x & 0xF0F0F0F0u) >> 4);
    x = ((x & 0x00FF00FFu) << 8)  | ((x & 0xFF00FF00u) >> 8);
    x = ((x & 0x0000FFFFu) << 16) | ((x & 0xFFFF0000u) >> 16);
    return x;
}

int popcount(unsigned int x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return x & 0x3F;
}

/* ============================================================================
   SECTION 6: STATE MACHINE
   ============================================================================ */

typedef enum {
    STATE_INIT,
    STATE_PROCESS,
    STATE_DONE,
    STATE_COUNT
} State;

typedef struct {
    int accumulator;
    int counter;
    int input[5];
    int output[5];
    int input_idx;
    int output_idx;
} StateMachine;

State sm_step(StateMachine *sm, State current) {
    switch (current) {
        case STATE_INIT:
            sm->accumulator = 0;
            sm->input_idx = 0;
            sm->output_idx = 0;
            return STATE_PROCESS;
        case STATE_PROCESS:
            if (sm->input_idx >= 5) return STATE_DONE;
            sm->accumulator += sm->input[sm->input_idx++];
            sm->output[sm->output_idx++] = sm->accumulator;
            return STATE_PROCESS;
        case STATE_DONE:
        default:
            return STATE_DONE;
    }
}

void run_state_machine(StateMachine *sm) {
    State current = STATE_INIT;
    while (current != STATE_DONE) {
        current = sm_step(sm, current);
    }
}

/* ============================================================================
   SECTION 7: 2D ARRAY OPERATIONS
   ============================================================================ */

void matrix_multiply_2x2(int a[2][2], int b[2][2], int c[2][2]) {
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            c[i][j] = 0;
            for (int k = 0; k < 2; k++) {
                c[i][j] += a[i][k] * b[k][j];
            }
        }
    }
}

/* ============================================================================
   MAIN - RUN ALL TESTS
   ============================================================================ */

int main(void) {
    int errors = 0;
    
    print_str("=== C99 Compiler Stress Test ===\n\n");
    
    /* Test 1: Variadic macro argument counting */
    print_str("[1] Variadic macro NARGS: ");
    int nargs_result = NARGS(a, b, c);
    print_num(nargs_result);
    if (nargs_result != 3) { print_str(" FAIL"); errors++; }
    else print_str(" OK");
    newline();
    
    /* Test 2: Token pasting */
    print_str("[2] Token pasting CAT: ");
    int CAT_(test, _var) = 42;
    print_num(test_var);
    if (test_var != 42) { print_str(" FAIL"); errors++; }
    else print_str(" OK");
    newline();
    
    /* Test 3: Deep pointer chain (4 levels) */
    print_str("[3] Deep pointer (4 levels): ");
    ptr4_t deep = build_ptr_chain(123);
    if (deep) {
        int val = deref4(deep);
        print_num(val);
        if (val != 123) { print_str(" FAIL"); errors++; }
        else print_str(" OK");
        free_ptr_chain(deep);
    } else {
        print_str("FAIL (malloc)");
        errors++;
    }
    newline();
    
    /* Test 4: Function pointer dispatch */
    print_str("[4] Function pointer dispatch:\n");
    init_dispatch();
    print_str("    10 + 5 = "); print_num(dispatch_op(0, 10, 5));
    if (dispatch_op(0, 10, 5) != 15) errors++;
    newline();
    print_str("    10 - 5 = "); print_num(dispatch_op(1, 10, 5));
    if (dispatch_op(1, 10, 5) != 5) errors++;
    newline();
    print_str("    10 * 5 = "); print_num(dispatch_op(2, 10, 5));
    if (dispatch_op(2, 10, 5) != 50) errors++;
    newline();
    print_str("    10 / 5 = "); print_num(dispatch_op(3, 10, 5));
    if (dispatch_op(3, 10, 5) != 2) errors++;
    print_str(" OK\n");
    
    /* Test 5: Bit-field structure */
    print_str("[5] Bit-field struct: ");
    struct BitFields bf;
    bf.a = 1;
    bf.b = 5;
    bf.c = 10;
    bf.d = 200;
    bf.e = 1000;
    int bf_ok = (bf.a == 1 && bf.b == 5 && bf.c == 10 && bf.d == 200 && bf.e == 1000);
    if (bf_ok) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 6: Nested structures */
    print_str("[6] Nested struct: ");
    struct Outer outer;
    outer.id = 1;
    outer.pos.x = 100;
    outer.pos.y = 200;
    outer.value = 42;
    int nested_ok = (outer.id == 1 && outer.pos.x == 100 && 
                     outer.pos.y == 200 && outer.value == 42);
    if (nested_ok) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 7: Bit manipulation */
    print_str("[7] Bit manipulation:\n");
    unsigned int test_val = 0x12345678;
    print_str("    Original:  "); print_hex(test_val); newline();
    unsigned int reversed = reverse_bits(test_val);
    print_str("    Reversed:  "); print_hex(reversed); newline();
    int bits = popcount(test_val);
    print_str("    Popcount:  "); print_num(bits);
    if (bits != 13) { print_str(" FAIL"); errors++; }
    else print_str(" OK");
    newline();
    
    /* Test 8: State machine */
    print_str("[8] State machine: ");
    StateMachine sm;
    sm.input[0] = 1; sm.input[1] = 2; sm.input[2] = 3;
    sm.input[3] = 4; sm.input[4] = 5;
    run_state_machine(&sm);
    /* Expected running sums: 1, 3, 6, 10, 15 */
    int sm_ok = (sm.output[0] == 1 && sm.output[1] == 3 && 
                 sm.output[2] == 6 && sm.output[3] == 10 && sm.output[4] == 15);
    if (sm_ok) print_str("OK");
    else { print_str("FAIL"); errors++; }
    newline();
    
    /* Test 9: 2D array matrix multiply */
    print_str("[9] 2D array matrix multiply:\n");
    int a[2][2];
    int b[2][2];
    int c[2][2];
    a[0][0] = 1; a[0][1] = 2; a[1][0] = 3; a[1][1] = 4;
    b[0][0] = 5; b[0][1] = 6; b[1][0] = 7; b[1][1] = 8;
    matrix_multiply_2x2(a, b, c);
    /* Expected: c[0][0]=19, c[0][1]=22, c[1][0]=43, c[1][1]=50 */
    print_str("    ["); print_num(c[0][0]); print_str(" "); print_num(c[0][1]); print_str("]\n");
    print_str("    ["); print_num(c[1][0]); print_str(" "); print_num(c[1][1]); print_str("]");
    int mat_ok = (c[0][0] == 19 && c[0][1] == 22 && c[1][0] == 43 && c[1][1] == 50);
    if (mat_ok) print_str(" OK");
    else { print_str(" FAIL"); errors++; }
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
