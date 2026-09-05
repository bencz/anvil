struct Small
{
    int value;
};

extern int native_small(struct Small value);

int main(void)
{
    struct Small value;
    value.value = 42;

    return native_small(value) == 42 ? 0 : 1;
}
