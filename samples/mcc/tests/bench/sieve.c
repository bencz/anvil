/* Sieve of Eratosthenes, repeated: byte stores, strided loops. */
#include <stdio.h>
#include <string.h>

#define LIMIT 4000000
static unsigned char composite[LIMIT + 1];

int main(void)
{
    long total = 0;
    for (int rep = 0; rep < 10; rep++) {
        memset(composite, 0, sizeof(composite));
        int count = 0;
        for (int i = 2; i <= LIMIT; i++) {
            if (composite[i])
                continue;
            count++;
            for (long j = (long)i * i; j <= LIMIT; j += i)
                composite[j] = 1;
        }
        total += count;
    }
    printf("sieve %ld\n", total);
    return 0;
}
