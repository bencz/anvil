/*
 * Advanced types test for MCC
 * Tests: typedef, nested structs, unions, complex type combinations
 */

/* Test 1: Simple typedef */
typedef int INT;
typedef unsigned int UINT;
typedef long LONG;

/* Test 2: Typedef pointer */
typedef int *PINT;
typedef char *STRING;

/* Test 3: Typedef struct with multiple names */
typedef struct point {
    int x;
    int y;
} Point, *PPoint;

/* Test 4: Typedef struct without tag (anonymous) with multiple names */
typedef struct {
    int width;
    int height;
} Size, *PSize;

/* Test 5: Multiple typedef aliases */
typedef int Integer, Int32, *PInteger;

/* Test 6: Struct with nested struct (using typedef) */
typedef struct {
    Point top_left;
    Point bottom_right;
} Rect;

/* Test 7: Struct with array field */
typedef struct {
    int data[10];
    char name[32];
    int count;
} ArrayStruct;

/* Test 8: Struct with union inside */
typedef struct {
    int type;
    union {
        int i;
        float f;
        char c;
    } value;
} Variant;

/* Test 7: Union with struct inside */
typedef union {
    struct {
        int x;
        int y;
    } point;
    struct {
        int w;
        int h;
    } size;
} PointOrSize;

/* Test 8: Deeply nested struct */
typedef struct {
    struct {
        struct {
            int value;
        } inner;
    } middle;
} DeepNested;

/* Test 9: Struct with pointer to self (linked list) */
typedef struct node {
    int data;
    struct node *next;
} Node;

/* Function using simple typedef */
INT add_ints(INT a, INT b)
{
    return a + b;
}

/* Function using typedef struct */
int point_sum(Point *p)
{
    return p->x + p->y;
}

/* Function using nested struct */
int rect_area(Rect *r)
{
    int w = r->bottom_right.x - r->top_left.x;
    int h = r->bottom_right.y - r->top_left.y;
    return w * h;
}

/* Function using struct with union */
int get_variant_int(Variant *v)
{
    if (v->type == 0) {
        return v->value.i;
    }
    return 0;
}

/* Function using union with struct */
int get_point_x(PointOrSize *p)
{
    return p->point.x;
}

/* Function using deeply nested struct */
int get_deep_value(DeepNested *d)
{
    return d->middle.inner.value;
}

/* Function with linked list node */
int node_sum(Node *head)
{
    int sum = 0;
    Node *current = head;
    while (current) {
        sum = sum + current->data;
        current = current->next;
    }
    return sum;
}

/* Main function */
int main(void)
{
    INT result = 0;
    
    /* Test simple typedef */
    INT a = 10;
    INT b = 20;
    result = result + add_ints(a, b);
    
    /* Test typedef struct */
    Point pt;
    pt.x = 5;
    pt.y = 7;
    result = result + point_sum(&pt);
    
    /* Test nested struct */
    Rect r;
    r.top_left.x = 0;
    r.top_left.y = 0;
    r.bottom_right.x = 10;
    r.bottom_right.y = 5;
    result = result + rect_area(&r);
    
    /* Test struct with union */
    Variant v;
    v.type = 0;
    v.value.i = 42;
    result = result + get_variant_int(&v);
    
    /* Test union with struct */
    PointOrSize pos;
    pos.point.x = 100;
    pos.point.y = 200;
    result = result + get_point_x(&pos);
    
    /* Test deeply nested */
    DeepNested deep;
    deep.middle.inner.value = 7;
    result = result + get_deep_value(&deep);
    
    /* Expected: 30 + 12 + 50 + 42 + 100 + 7 = 241 */
    return result;
}
