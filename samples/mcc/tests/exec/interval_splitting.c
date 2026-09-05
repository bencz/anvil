#include <stdio.h>

static volatile double inputs[12] = { 1.25, 2.5, 3.75, 4.25, 5.5, 6.75, 7.25, 8.5, 9.75, 10.25, 11.5, 12.75 };

static double calculate(int seed)
{
    double a = inputs[0] + seed;
    double b = inputs[1] + seed;
    double c = inputs[2] + seed;
    double d = inputs[3] + seed;
    double e = inputs[4] + seed;
    double f = inputs[5] + seed;
    double g = inputs[6] + seed;
    double h = inputs[7] + seed;
    double i = inputs[8] + seed;
    double j = inputs[9] + seed;
    double k = inputs[10] + seed;
    double l = inputs[11] + seed;
    printf("before %.2f %.2f\n", a + c + e + g + i + k, b + d + f + h + j + l);
    double first = a * b + c * d + e * f + g * h + i * j + k * l;
    printf("between %.2f\n", first);
    double second = a + b + c + d + e + f + g + h + i + j + k + l;
    return first + second;
}

int main(void)
{
    double total = 0;
    for (int seed = -3; seed <= 3; seed++)
        total += calculate(seed);

    printf("total %.2f\n", total);
    return 0;
}
