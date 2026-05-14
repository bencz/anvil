/* Pass a struct by value and return a struct by value. Small structs only
 * — the ABI for large structs varies and isn't the point of this test. */

int putchar(int c);

typedef struct {
    int x;
    int y;
} Point;

static Point make_point(int x, int y) {
    Point p;
    p.x = x;
    p.y = y;
    return p;
}

static int sum_point(Point p) {
    return p.x + p.y;
}

int main(void) {
    Point p = make_point(2, 3);
    int s = sum_point(p);  /* 5 */
    putchar('0' + s);
    putchar('\n');
    return 0;
}
