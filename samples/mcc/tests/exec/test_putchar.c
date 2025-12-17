int putchar(int c);

void show(int a, int b) {
    putchar('0' + a);
    putchar('0' + b);
    putchar('\n');
}

int main(void) {
    show(1, 2);
    return 0;
}
