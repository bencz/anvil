#include <stdio.h>

struct Pair { int left; int right; };
struct Nested { struct Pair pair; int tail; };

static int update(volatile struct Pair *pair)
{
    pair->left += 5;
    pair->right *= 3;
    return pair->left + pair->right;
}

int main(void)
{
    volatile struct Pair observed;
    volatile struct Nested nested;
    observed.left = 7;
    observed.right = 11;
    if (update(&observed) != 45 || observed.left != 12 || observed.right != 33)
        return 1;

    struct Pair copy = observed;
    copy.left = 99;
    if (observed.left != 12 || copy.right != 33)
        return 2;

    nested.pair.left = 17;
    nested.pair.right = 25;
    nested.tail = 123;
    if (nested.pair.left + nested.pair.right != 42 || nested.tail != 123)
        return 3;

    observed = copy;
    if (observed.left != 99 || observed.right != 33)
        return 4;

    puts("volatile aggregate access passed");
    return 0;
}
