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

/* MCC deliberately does not advertise labels-as-values/computed goto yet:
 * ANVIL IR has no block-address/indirect-branch primitive.  GNU attributes are
 * likewise excluded until record layout and call-site metadata can preserve
 * their semantics.  These constructs must be rejected, never token-skipped. */

int main(void)
{
    int a = 5, b = 10;
    int m = MAX(a, b);
    
    int class_a = classify('a');
    int class_Z = classify('Z');
    int class_5 = classify('5');
    
    return m;
}
