/* C89 requires every enumerator value to be representable as int. */
enum invalid_enum {
    INVALID_ENUM_VALUE = 2147483648
};

int main(void)
{
    return 0;
}
