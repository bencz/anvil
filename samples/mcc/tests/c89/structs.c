/*
 * C89 Test: Structures and Unions
 * Tests struct, union, and enum declarations
 */

/* Forward declaration */
struct forward;

/* Basic struct */
struct point {
    int x;
    int y;
};

/* Struct with typedef */
typedef struct {
    int width;
    int height;
} rect_t;

/* Nested struct */
struct nested {
    struct point origin;
    rect_t size;
};

/* Struct with pointers */
struct node {
    int value;
    struct node *next;
};

/* Union */
union data {
    int i;
    float f;
    char c;
};

/* Tagged union pattern */
struct variant {
    int type;
    union {
        int int_val;
        float float_val;
        char *str_val;
    } value;
};

/* Enum */
enum color {
    RED,
    GREEN,
    BLUE
};

/* Enum with explicit values */
enum status {
    OK = 0,
    ERROR = -1,
    PENDING = 1
};

/* Bitfields */
struct flags {
    unsigned int flag1 : 1;
    unsigned int flag2 : 1;
    unsigned int value : 6;
};

int main(void)
{
    struct point p1;
    struct point p2 = {10, 20};
    rect_t r = {100, 200};
    struct nested n;
    struct node head;
    union data d;
    struct variant v;
    enum color c;
    enum status s;
    struct flags f;
    
    /* Member access */
    p1.x = 5;
    p1.y = 10;
    
    /* Nested member access */
    n.origin.x = 0;
    n.origin.y = 0;
    n.size.width = 50;
    n.size.height = 50;
    
    /* Pointer to struct */
    struct point *pp = &p1;
    pp->x = 15;
    pp->y = 25;
    
    /* Union usage */
    d.i = 42;
    d.f = 3.14f;
    d.c = 'A';
    
    /* Tagged union */
    v.type = 1;
    v.value.int_val = 100;
    
    /* Enum usage */
    c = GREEN;
    s = OK;
    
    /* Bitfield usage */
    f.flag1 = 1;
    f.flag2 = 0;
    f.value = 42;
    
    /* Linked list */
    head.value = 1;
    head.next = 0;
    
    return 0;
}
