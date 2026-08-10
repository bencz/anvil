static int calls;

static int init_once(void)
{
    calls = calls + 1;
    return 99;
}

static void touch(int *p)
{
    *p = 4;
}

static int test_alias_and_call(void)
{
    int x = 1;
    int *p = &x;
    *p = 2;
    if (x != 2) return 1;
    x = 1;
    touch(&x);
    return x == 4 ? 0 : 2;
}

static int test_loop_control(void)
{
    int i = 0;
    int hits = 0;

    while (i < 3) {
        i = i + 1;
        do {
            if (i == 2) continue;
            hits = hits + i;
        } while (0);
        hits = hits + 10;
    }
    if (hits != 34) return 3;

    i = 0;
    hits = 0;
    while (i < 3) {
        i = i + 1;
        do {
            if (i > 0) break;
        } while (0);
        hits = hits + 1;
    }
    return hits == 3 ? 0 : 4;
}

static int test_for_scope(void)
{
    int x = 7;
    calls = 0;
    for (int x = init_once(); 0; x = x + 1) {
        calls = 100;
    }
    return x == 7 && calls == 1 ? 0 : 5;
}

static int test_goto_target(void)
{
    goto target;
    return 6;
target:
    return 0;
}

int main(void)
{
    int result = test_alias_and_call();
    if (result) return result;
    result = test_loop_control();
    if (result) return result;
    result = test_for_scope();
    if (result) return result;
    return test_goto_target();
}
