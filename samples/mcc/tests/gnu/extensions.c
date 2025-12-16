/*
 * GNU Test: GNU Extensions
 * Tests GNU C extensions
 */

/* GNU: Statement expressions */
#define MAX(a, b) ({ \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a > _b ? _a : _b; \
})

/* GNU: __typeof__ */
int x = 10;
__typeof__(x) y = 20;

/* GNU: Zero-length arrays */
struct zero_array {
    int size;
    char data[0];
};

/* GNU: Case ranges */
int classify(int c)
{
    switch (c) {
        case 'a' ... 'z':
            return 1;  /* lowercase */
        case 'A' ... 'Z':
            return 2;  /* uppercase */
        case '0' ... '9':
            return 3;  /* digit */
        default:
            return 0;
    }
}

/* GNU: Labels as values */
void *label_ptr;

void jump_table(int n)
{
    static void *table[] = { &&label1, &&label2, &&label3 };
    if (n >= 0 && n < 3) {
        goto *table[n];
    }
    return;
    
label1:
    x = 1;
    return;
label2:
    x = 2;
    return;
label3:
    x = 3;
    return;
}

/* GNU: __attribute__ */
int deprecated_func(void) __attribute__((deprecated));
void noreturn_func(void) __attribute__((noreturn));
int pure_func(int x) __attribute__((pure));

struct __attribute__((packed)) packed_struct {
    char a;
    int b;
    char c;
};

struct __attribute__((aligned(16))) aligned_struct {
    int x;
    int y;
};

int main(void)
{
    int a = 5, b = 10;
    int m = MAX(a, b);
    
    int class_a = classify('a');
    int class_Z = classify('Z');
    int class_5 = classify('5');
    
    return m;
}
