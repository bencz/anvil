int printf(const char *fmt, ...);

void show(int a, int b) {
    printf("a=%d, b=%d\n", a, b);
}

int main(void) {
    show(1, 2);
    return 0;
}
