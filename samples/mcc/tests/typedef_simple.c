/*
 * Simple typedef test
 */

/* Basic typedef */
typedef int INT;
typedef int *PINT;

/* Typedef struct with single name */
typedef struct point {
    int x;
    int y;
} Point;

/* Use typedef */
INT add(INT a, INT b)
{
    return a + b;
}

int get_x(Point *p)
{
    return p->x;
}

int main(void)
{
    INT x = 10;
    INT y = 20;
    Point pt;
    
    pt.x = 5;
    pt.y = 7;
    
    return add(x, y) + get_x(&pt);
}
