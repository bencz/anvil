/*
 * Struct and preprocessor test for MCC
 */

#include <stddef.h>

#define MAX_POINTS 10
#define SQUARE(x) ((x) * (x))

/* Point structure */
struct point {
    int x;
    int y;
};

/* Rectangle structure */
struct rect {
    struct point top_left;
    struct point bottom_right;
};

/* Calculate distance squared between two points */
int distance_sq(struct point *p1, struct point *p2)
{
    int dx = p2->x - p1->x;
    int dy = p2->y - p1->y;
    return SQUARE(dx) + SQUARE(dy);
}

/* Calculate area of rectangle */
int rect_area(struct rect *r)
{
    int width = r->bottom_right.x - r->top_left.x;
    int height = r->bottom_right.y - r->top_left.y;
    return width * height;
}

/* Initialize a point */
void point_init(struct point *p, int x, int y)
{
    p->x = x;
    p->y = y;
}

int main(void)
{
    struct point p1;
    struct point p2;
    struct rect r;
    int dist;
    int area;
    
    point_init(&p1, 0, 0);
    point_init(&p2, 3, 4);
    
    dist = distance_sq(&p1, &p2);
    
    r.top_left.x = 0;
    r.top_left.y = 0;
    r.bottom_right.x = 10;
    r.bottom_right.y = 5;
    
    area = rect_area(&r);
    
    return dist + area;
}
