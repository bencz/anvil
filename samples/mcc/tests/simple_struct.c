/* Simple struct test */

struct point {
    int x;
    int y;
};

int get_x(struct point *p)
{
    return p->x;
}

void set_point(struct point *p, int x, int y)
{
    p->x = x;
    p->y = y;
}

int main(void)
{
    struct point pt;
    
    set_point(&pt, 10, 20);
    
    return get_x(&pt);
}
