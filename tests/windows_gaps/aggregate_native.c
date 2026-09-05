#include <stdio.h>

struct Small
{
    int value;
};

int native_small(struct Small value)
{
    printf("small aggregate: expected 42, received %d\n", value.value);
    return value.value;
}
