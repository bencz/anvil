/*
 * C99 Test: New Types
 * Tests C99 new types and features
 */

/* C99 long long type */
long long ll = 9223372036854775807LL;
unsigned long long ull = 18446744073709551615ULL;
long long int lli = -9223372036854775807LL;

/* C99 _Bool type */
_Bool b1 = 0;
_Bool b2 = 1;
_Bool b3 = 100;  /* Converts to 1 */

/* C99 restrict qualifier */
void copy(int * restrict dest, const int * restrict src, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        dest[i] = src[i];
    }
}

/* C99 inline functions */
inline int square(int x)
{
    return x * x;
}

static inline int cube(int x)
{
    return x * x * x;
}

/* C99 flexible array member */
struct flex_array {
    int size;
    int data[];  /* Flexible array member */
};

/* C99 designated initializers */
struct point {
    int x, y, z;
};

int main(void)
{
    long long a = 1000000000000LL;
    unsigned long long b = 2000000000000ULL;
    _Bool flag = 1;
    
    /* Designated initializers for struct */
    struct point p1 = {.x = 10, .y = 20, .z = 30};
    struct point p2 = {.z = 100, .x = 50};  /* y is 0 */
    
    /* Designated initializers for array */
    int arr[10] = {[0] = 1, [5] = 5, [9] = 9};
    int arr2[5] = {[2] = 100, [4] = 200};
    
    /* Mixed designated and positional */
    int arr3[5] = {1, 2, [4] = 5};
    
    /* Compound literals */
    struct point *pp = &(struct point){100, 200, 300};
    int *ip = (int[]){1, 2, 3, 4, 5};
    
    /* inline function calls */
    int sq = square(5);
    int cb = cube(3);
    
    /* _Bool operations */
    flag = a > b;
    flag = !flag;
    
    return 0;
}
