/* Test: structs */
int putchar(int c);

struct Point {
    int x;
    int y;
};

int main(void) {
    struct Point p;
    p.x = 3;
    p.y = 5;
    
    /* Print x and y */
    putchar('0' + p.x);
    putchar('0' + p.y);
    putchar('\n');
    
    /* Sum */
    int sum = p.x + p.y;
    putchar('0' + sum);
    putchar('\n');
    
    return 0;
}
