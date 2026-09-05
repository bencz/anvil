#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*vector_function)(const void *, const void *, void *);
extern void vector_32_0(const void *, const void *, void *);
extern void vector_32_1(const void *, const void *, void *);
extern void vector_32_2(const void *, const void *, void *);
extern void vector_32_3(const void *, const void *, void *);
extern void vector_64_0(const void *, const void *, void *);
extern void vector_64_1(const void *, const void *, void *);
extern void vector_64_2(const void *, const void *, void *);
extern void vector_64_3(const void *, const void *, void *);
extern void slp_32_0(void);
extern void slp_32_1(void);
extern void slp_32_2(void);
extern void slp_32_3(void);
extern void slp_64_0(void);
extern void slp_64_1(void);
extern void slp_64_2(void);
extern void slp_64_3(void);
extern float slp_data32[12];
extern double slp_data64[6];

static double expected(double left, double right, unsigned operation)
{
    switch (operation)
    {
        case 0:
            return left + right;
        case 1:
            return left - right;
        case 2:
            return left * right;
        default:
            return left / right;
    }
}

int main(void)
{
    vector_function functions[] = { vector_32_0, vector_32_1, vector_32_2, vector_32_3, vector_64_0, vector_64_1, vector_64_2, vector_64_3 };
    void (*packed[])(void) = { slp_32_0, slp_32_1, slp_32_2, slp_32_3, slp_64_0, slp_64_1, slp_64_2, slp_64_3 };
    double patterns[] = { 0.0, -0.0, 1.25, -7.5, INFINITY, -INFINITY, NAN, 0.125 };
    for (unsigned function = 0; function < 8; function++)
    {
        for (unsigned pattern = 0; pattern < 64; pattern++)
        {
            double left64[4] = { 0 };
            double right64[4] = { 0 };
            double output64[4] = { 19, 19, 19, 19 };
            float left32[6] = { 0 };
            float right32[6] = { 0 };
            float output32[6] = { 19, 19, 19, 19, 19, 19 };
            unsigned lanes = function < 4 ? 4 : 2;
            for (unsigned lane = 0; lane < lanes; lane++)
            {
                double left = patterns[(pattern + lane) % 8];
                double right = patterns[(pattern / 8 + lane) % 8];
                if (function < 4)
                {
                    left32[lane + 1] = (float)left;
                    right32[lane + 1] = (float)right;
                    slp_data32[lane] = (float)left;
                    slp_data32[lane + lanes] = (float)right;
                }
                else
                {
                    left64[lane + 1] = left;
                    right64[lane + 1] = right;
                    slp_data64[lane] = left;
                    slp_data64[lane + lanes] = right;
                }
            }

            /* Offset by one element to exercise unaligned vector accesses. */
            functions[function](function < 4 ? (void *)(left32 + 1) : (void *)(left64 + 1),
                                function < 4 ? (void *)(right32 + 1) : (void *)(right64 + 1),
                                function < 4 ? (void *)(output32 + 1) : (void *)(output64 + 1));
            packed[function]();
            for (unsigned lane = 0; lane < lanes; lane++)
            {
                double left = patterns[(pattern + lane) % 8];
                double right = patterns[(pattern / 8 + lane) % 8];
                double reference = expected(left, right, function % 4);
                double actual = function < 4 ? output32[lane + 1] : output64[lane + 1];
                double vectorized = function < 4 ? slp_data32[lane + lanes * 2] : slp_data64[lane + lanes * 2];
                if (function < 4)
                    reference = (float)reference;

                if (!((isnan(reference) && isnan(actual)) || (reference == actual && (reference != 0 || signbit(reference) == signbit(actual)))))
                {
                    fprintf(stderr, "vector mismatch: function=%u pattern=%u lane=%u actual=%g reference=%g\n", function, pattern, lane, actual, reference);
                    return 1;
                }

                if (!((isnan(reference) && isnan(vectorized)) || (reference == vectorized && (reference != 0 || signbit(reference) == signbit(vectorized)))))
                {
                    fprintf(stderr, "SLP mismatch: function=%u pattern=%u lane=%u\n", function, pattern, lane);
                    return 1;
                }
            }

            if (output32[0] != 19 || output32[5] != 19 || output64[0] != 19 || output64[3] != 19)
                return 1;
        }
    }

    puts("1024 direct/SLP vector cases passed, including unaligned memory and register clobbers");
    return 0;
}
