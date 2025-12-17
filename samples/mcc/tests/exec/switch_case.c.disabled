/* Test: Switch-case statements */

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

/* Return day name length */
int day_length(int day) {
    switch (day) {
        case 0: return 6;  /* Sunday */
        case 1: return 6;  /* Monday */
        case 2: return 7;  /* Tuesday */
        case 3: return 9;  /* Wednesday */
        case 4: return 8;  /* Thursday */
        case 5: return 6;  /* Friday */
        case 6: return 8;  /* Saturday */
        default: return 0;
    }
}

/* Grade to points */
int grade_points(char grade) {
    switch (grade) {
        case 'A':
            return 4;
        case 'B':
            return 3;
        case 'C':
            return 2;
        case 'D':
            return 1;
        case 'F':
            return 0;
        default:
            return -1;
    }
}

int main(void) {
    /* Test day_length */
    print_num(day_length(0));  /* 6 (Sunday) */
    putchar('\n');
    print_num(day_length(3));  /* 9 (Wednesday) */
    putchar('\n');
    print_num(day_length(7));  /* 0 (invalid) */
    putchar('\n');
    
    /* Test grade_points */
    print_num(grade_points('A'));  /* 4 */
    putchar('\n');
    print_num(grade_points('C'));  /* 2 */
    putchar('\n');
    print_num(grade_points('X'));  /* -1 */
    putchar('\n');
    
    return 0;
}
