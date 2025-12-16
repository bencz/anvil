/*
 * C89 Test: Typedef
 * Tests typedef declarations
 */

/* Basic typedef */
typedef int INT;
typedef unsigned int UINT;
typedef char *STRING;

/* Multiple typedef names */
typedef int INT32, *PINT32, **PPINT32;

/* Typedef for pointers */
typedef int *int_ptr;
typedef const char *const_str;

/* Typedef for arrays */
typedef int int_array[10];
typedef char name_t[32];

/* Typedef for function pointers */
typedef int (*binary_op)(int, int);
typedef void (*callback)(void);

/* Typedef for struct */
typedef struct {
    int x, y;
} Point;

typedef struct node {
    int value;
    struct node *next;
} Node, *NodePtr;

/* Typedef for enum */
typedef enum {
    FALSE = 0,
    TRUE = 1
} Boolean;

/* Typedef for union */
typedef union {
    int i;
    float f;
} Number;

/* Function using typedef */
int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }

int main(void)
{
    INT i = 10;
    UINT u = 20;
    STRING s = "hello";
    INT32 x = 100;
    PINT32 px = &x;
    PPINT32 ppx = &px;
    int_ptr ip;
    int_array arr;
    name_t name;
    Point p = {5, 10};
    Node n;
    NodePtr np;
    Boolean b = TRUE;
    Number num;
    binary_op op;
    
    /* Use typedef types */
    i = i + 1;
    *px = 200;
    **ppx = 300;
    
    arr[0] = 1;
    arr[9] = 10;
    
    name[0] = 'A';
    
    p.x = 15;
    p.y = 25;
    
    n.value = 42;
    n.next = 0;
    np = &n;
    
    b = FALSE;
    
    num.i = 100;
    num.f = 3.14f;
    
    /* Function pointer */
    op = add;
    i = op(5, 3);
    op = sub;
    i = op(10, 4);
    
    return 0;
}
