#define PICK(a, b) ({ __typeof__(a) x_ = (a); __typeof__(b) y_ = (b); \
                      x_ > y_ ? x_ : y_; })

static int classify(int x)
{
    switch (x) {
        case 10 ... 19: return 1;
        case 20 ... 29: return 2;
        default: return 0;
    }
}

int main(void)
{
    double d = 2.5;
    int value = 9;
    int *ptr = &value;
    __typeof__(d) same_double = 3.5;
    __typeof__(ptr) same_pointer = ptr;
    if (PICK(4, 7) != 7) return 1;
    if (same_double != 3.5 || *same_pointer != 9) return 2;
    if (classify(15) != 1 || classify(25) != 2 || classify(3) != 0) return 3;
    return 0;
}
