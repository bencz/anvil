#include <stdio.h>

static float values[12];
static double wide_values[6];

static void initialize(int seed)
{
    for (int index = 0; index < 8; index++)
        values[index] = (float)(seed + index) * 0.5f;

    for (int index = 0; index < 4; index++)
        wide_values[index] = (double)(seed - index) * 0.25;
}

static void calculate(void)
{
    for (int index = 0; index < 4; index++)
        values[index + 8] = values[index] * values[index + 4];

    for (int index = 0; index < 2; index++)
        wide_values[index + 4] = wide_values[index] + wide_values[index + 2];
}

int main(void)
{
    double total = 0;
    for (int seed = -5; seed < 12; seed++)
    {
        initialize(seed);
        calculate();
        for (int index = 0; index < 4; index++)
            total += values[index + 8];

        total += wide_values[4] + wide_values[5];
    }

    printf("%.4f\n", total);
    return 0;
}
