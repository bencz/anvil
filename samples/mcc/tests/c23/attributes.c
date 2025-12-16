/*
 * C23 Test: Attributes
 * Tests C23 standard attributes
 */

/* C23: [[deprecated]] attribute */
[[deprecated]] int old_function(void);
[[deprecated("use new_function instead")]] int legacy_function(void);

/* C23: [[nodiscard]] attribute */
[[nodiscard]] int must_use_result(void);
[[nodiscard("error code must be checked")]] int get_error(void);

/* C23: [[maybe_unused]] attribute */
[[maybe_unused]] static int unused_var = 0;
void func([[maybe_unused]] int unused_param) { }

/* C23: [[noreturn]] attribute */
[[noreturn]] void abort_now(void);

/* C23: [[fallthrough]] in switch */
int process(int x)
{
    int result = 0;
    switch (x) {
        case 1:
            result = 10;
            [[fallthrough]];
        case 2:
            result += 20;
            break;
        case 3:
            result = 30;
            break;
        default:
            result = -1;
    }
    return result;
}

int main(void)
{
    int r = process(1);
    return r;
}
