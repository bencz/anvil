/* Test: Nested structures */

int putchar(int c);

void print_digit(int n) {
    putchar('0' + n);
}

void print_num(int n) {
    if (n < 0) {
        putchar('-');
        n = -n;
    }
    if (n >= 10) {
        print_num(n / 10);
    }
    print_digit(n % 10);
}

struct Point {
    int x;
    int y;
};

struct Rectangle {
    struct Point top_left;
    struct Point bottom_right;
};

int rect_width(struct Rectangle *r) {
    return r->bottom_right.x - r->top_left.x;
}

int rect_height(struct Rectangle *r) {
    return r->bottom_right.y - r->top_left.y;
}

int rect_area(struct Rectangle *r) {
    return rect_width(r) * rect_height(r);
}

int main(void) {
    struct Rectangle r;
    
    /* Initialize rectangle from (2,3) to (10,8) */
    r.top_left.x = 2;
    r.top_left.y = 3;
    r.bottom_right.x = 10;
    r.bottom_right.y = 8;
    
    /* Width: 10-2 = 8 */
    print_num(rect_width(&r));
    putchar('\n');
    
    /* Height: 8-3 = 5 */
    print_num(rect_height(&r));
    putchar('\n');
    
    /* Area: 8*5 = 40 */
    print_num(rect_area(&r));
    putchar('\n');
    
    return 0;
}
