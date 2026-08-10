/* Target-width constant evaluation and inferred array bounds. */

unsigned char narrowed = (unsigned char)300;
signed char negative = (signed char)255;
unsigned int wrapped = (unsigned int)-1;
unsigned long long wide_wrapped = (unsigned long long)-1;
int lazy_and = 0 && (1 / 0);
int lazy_or = 1 || (1 / 0);
char inferred[] = "abc";

int main(void)
{
    if (narrowed != 44) return 1;
    if (negative != -1) return 2;
    if (wrapped != 4294967295U) return 3;
    if (wide_wrapped != 18446744073709551615ULL) return 4;
    if (lazy_and != 0 || lazy_or != 1) return 5;
    if (sizeof(inferred) != 4) return 6;
    if (inferred[0] != 'a' || inferred[3] != '\0') return 7;
    return 0;
}
