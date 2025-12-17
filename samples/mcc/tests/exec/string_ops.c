/* Test: String operations */

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

/* String length */
int my_strlen(char *s) {
    int len = 0;
    while (s[len] != '\0') {
        len = len + 1;
    }
    return len;
}

/* Compare two strings */
int my_strcmp(char *a, char *b) {
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return a[i] - b[i];
        }
        i = i + 1;
    }
    return a[i] - b[i];
}

/* Count occurrences of char in string */
int count_char(char *s, char c) {
    int count = 0;
    int i = 0;
    while (s[i] != '\0') {
        if (s[i] == c) {
            count = count + 1;
        }
        i = i + 1;
    }
    return count;
}

int main(void) {
    char hello[6];
    char world[6];
    
    /* Initialize strings manually */
    hello[0] = 'h'; hello[1] = 'e'; hello[2] = 'l';
    hello[3] = 'l'; hello[4] = 'o'; hello[5] = '\0';
    
    world[0] = 'w'; world[1] = 'o'; world[2] = 'r';
    world[3] = 'l'; world[4] = 'd'; world[5] = '\0';
    
    /* Test strlen */
    print_num(my_strlen(hello));  /* 5 */
    putchar('\n');
    
    /* Test strcmp (hello < world) */
    if (my_strcmp(hello, world) < 0) {
        print_num(1);
    } else {
        print_num(0);
    }
    putchar('\n');
    
    /* Test count_char */
    print_num(count_char(hello, 'l'));  /* 2 */
    putchar('\n');
    
    print_num(count_char(world, 'o'));  /* 1 */
    putchar('\n');
    
    return 0;
}
