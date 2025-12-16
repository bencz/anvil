/*
 * C99 Test: Literals
 * Tests C99 literal features
 */

int main(void)
{
    /* C99: Hexadecimal floating-point literals */
    double hex_float1 = 0x1.0p0;      /* 1.0 */
    double hex_float2 = 0x1.8p1;      /* 3.0 */
    double hex_float3 = 0xA.Bp-2;     /* 2.671875 */
    float hex_floatf = 0x1.0p0f;
    
    /* C99: Long long literals */
    long long ll1 = 1234567890123456789LL;
    long long ll2 = -9223372036854775807LL;
    unsigned long long ull1 = 18446744073709551615ULL;
    
    /* Mixed case suffixes */
    long long ll3 = 100ll;
    long long ll4 = 100Ll;
    unsigned long long ull2 = 100uLL;
    unsigned long long ull3 = 100ULL;
    
    /* C99: __func__ predefined identifier */
    const char *func_name = __func__;
    
    return 0;
}

void test_func(void)
{
    /* __func__ in different function */
    const char *name = __func__;  /* "test_func" */
}
