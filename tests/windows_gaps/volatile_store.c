void two_writes(volatile int *address)
{
    *address = 1;
    *address = 2;
}

int volatile_local(void)
{
    volatile int value;
    value = 1;
    value = 2;
    return value;
}
