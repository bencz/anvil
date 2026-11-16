/* Narrow values loaded and widened to 64 bits by their own signedness, by
 * the other one, or through a chain of conversions (char to int to long
 * long, short to unsigned to long long): one extending load where the
 * target has it, and the C conversions' values in every case. */
#include <stdio.h>

static const signed char bytes[] = { 0, 1, -1, 127, -128, 64, -65 };
static const unsigned char ubytes[] = { 0, 1, 255, 128, 127, 200 };
static const short halves[] = { 0, 1, -1, 32767, -32768, 1000, -1000 };
static const unsigned short uhalves[] = { 0, 1, 65535, 32768, 32767, 40000 };
static const int words[] = { 0, 1, -1, 2147483647, -2147483647 - 1, 123456789, -123456789 };
static const unsigned uwords[] = { 0, 1, 4294967295u, 2147483648u, 2147483647u, 3000000000u };

static long long sign_byte(const signed char *p) { return *p; }
static unsigned long long zero_byte(const unsigned char *p) { return *p; }
static long long sign_half(const short *p) { return *p; }
static unsigned long long zero_half(const unsigned short *p) { return *p; }
static long long sign_word(const int *p) { return *p; }
static unsigned long long zero_word(const unsigned *p) { return *p; }
static unsigned long long byte_as_unsigned(const signed char *p) { return (unsigned char)*p; }
static long long chained(const signed char *p) { int v = *p; return (long long)v; }
static unsigned long long through_unsigned(const short *p) { unsigned v = (unsigned)*p; return v; }
static long long indexed(const long long *table, const signed char *p) { return table[*p + 128]; }

int main(void)
{
    static long long table[256];
    for (int i = 0; i < 256; i++)
        table[i] = (long long)(i - 128) * 1000003;
    unsigned long long sum = 0;
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        printf("b %lld %llu %lld %lld\n", sign_byte(&bytes[i]), byte_as_unsigned(&bytes[i]), chained(&bytes[i]), indexed(table, &bytes[i]));
        sum = sum * 31 + (unsigned long long)sign_byte(&bytes[i]);
    }
    for (unsigned i = 0; i < sizeof(ubytes); i++)
        printf("ub %llu\n", zero_byte(&ubytes[i]));
    for (unsigned i = 0; i < sizeof(halves) / sizeof(halves[0]); i++) {
        printf("h %lld %llu\n", sign_half(&halves[i]), through_unsigned(&halves[i]));
        sum = sum * 31 + through_unsigned(&halves[i]);
    }
    for (unsigned i = 0; i < sizeof(uhalves) / sizeof(uhalves[0]); i++)
        printf("uh %llu\n", zero_half(&uhalves[i]));
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        printf("w %lld\n", sign_word(&words[i]));
        sum = sum * 31 + (unsigned long long)sign_word(&words[i]);
    }
    for (unsigned i = 0; i < sizeof(uwords) / sizeof(uwords[0]); i++)
        printf("uw %llu\n", zero_word(&uwords[i]));
    printf("sum %llu\n", sum);
    return 0;
}
