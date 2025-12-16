/*
 * C11 Test: _Static_assert
 * Tests C11 static assertions
 */

/* File scope static assertions */
_Static_assert(sizeof(char) == 1, "char must be 1 byte");
_Static_assert(sizeof(short) >= 2, "short must be at least 2 bytes");
_Static_assert(sizeof(int) >= 2, "int must be at least 2 bytes");
_Static_assert(sizeof(long) >= 4, "long must be at least 4 bytes");

/* Static assert with type sizes */
_Static_assert(sizeof(int) <= sizeof(long), "int must not be larger than long");
_Static_assert(sizeof(float) == 4, "float must be 4 bytes");
_Static_assert(sizeof(double) == 8, "double must be 8 bytes");

/* Static assert in struct */
struct checked_struct {
    int x;
    int y;
    _Static_assert(sizeof(int) == 4, "int must be 4 bytes for this struct");
};

/* Static assert with expressions */
#define ARRAY_SIZE 100
_Static_assert(ARRAY_SIZE > 0, "array size must be positive");
_Static_assert(ARRAY_SIZE <= 1000, "array size must not exceed 1000");

int main(void)
{
    /* Block scope static assertions */
    _Static_assert(1 + 1 == 2, "math is broken");
    _Static_assert(sizeof(int) * 2 == sizeof(long long), "unexpected type sizes");
    
    int arr[ARRAY_SIZE];
    _Static_assert(sizeof(arr) == ARRAY_SIZE * sizeof(int), "array size mismatch");
    
    return 0;
}
