/* Test C99 keywords */
inline int add(int a, int b) {
    return a + b;
}

int main(void) {
    _Bool flag = 1;
    int * restrict ptr = 0;
    return flag ? add(1, 2) : 0;
}
