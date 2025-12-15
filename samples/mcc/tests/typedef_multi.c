/*
 * Test for multiple typedef names and arrays in structs
 */

/* Multiple typedef names */
typedef int INT, *PINT, **PPINT;
typedef char CHAR, *PCHAR, *STRING;

/* Typedef struct with multiple names */
typedef struct point {
    int x;
    int y;
} Point, *PPoint, **PPPoint;

/* Struct with array fields */
typedef struct {
    int data[10];
    char name[32];
    int *ptr;
    int count;
} ArrayStruct;

/* Test function using multiple typedef names */
INT test_typedef(INT a, PINT p)
{
    *p = a * 2;
    return a + *p;
}

/* Test function using struct with arrays */
int test_array_struct(ArrayStruct *s)
{
    s->data[0] = 100;
    s->data[1] = 200;
    s->count = 2;
    return s->data[0] + s->data[1];
}

/* Test function using PPoint */
int test_ppoint(PPoint p)
{
    p->x = 10;
    p->y = 20;
    return p->x + p->y;
}

int main(void)
{
    INT result = 0;
    INT value = 5;
    PINT ptr = &value;
    
    /* Test multiple typedef names */
    result = result + test_typedef(10, ptr);
    
    /* Test PPoint */
    Point pt;
    result = result + test_ppoint(&pt);
    
    /* Expected: 30 + 30 = 60 */
    return result;
}
