/*
 * C11 Test: Anonymous Structs and Unions
 * Tests C11 anonymous struct/union members
 */

/* C11: Anonymous struct member */
struct outer {
    int id;
    struct {
        int x;
        int y;
    };  /* Anonymous struct */
};

/* C11: Anonymous union member */
struct variant {
    int type;
    union {
        int int_val;
        float float_val;
        char *str_val;
    };  /* Anonymous union */
};

/* Nested anonymous */
struct complex {
    int flags;
    struct {
        union {
            int i;
            float f;
        };
        int extra;
    };
};

int main(void)
{
    struct outer o;
    struct variant v;
    struct complex c;
    
    /* Access anonymous struct members directly */
    o.id = 1;
    o.x = 10;
    o.y = 20;
    
    /* Access anonymous union members directly */
    v.type = 1;
    v.int_val = 100;
    
    v.type = 2;
    v.float_val = 3.14f;
    
    /* Nested anonymous access */
    c.flags = 0xFF;
    c.i = 42;
    c.extra = 99;
    
    return o.x + o.y;
}
