/* AND, OR and XOR through a 16 x 16 lookup table of nibbles: loops over
 * 16-bit values, divisions and remainders by 16, table loads.  The inner
 * bound restarts for every a and comes from a volatile, so the checks run. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define XOR(a,b) (a + b - 2*AND(a,b))
#define IOR(a,b) XOR(XOR(a,b),AND(a,b))

static const uint16_t andlookup[256] = {
#define C4(a,b) ((a)&(b)), ((a)&(b+1)), ((a)&(b+2)), ((a)&(b+3))
#define L(a) C4(a,0), C4(a,4), C4(a,8), C4(a,12)
#define L4(a) L(a), L(a+1), L(a+2), L(a+3)
    L4(0), L4(4), L4(8), L4(12)
#undef C4
#undef L
#undef L4
};

uint16_t AND(uint16_t a, uint16_t b)
{
    uint16_t r=0, i;

    for ( i = 0; i < 16; i += 4 )
    {
            r = r/16 + andlookup[(a%16)*16+(b%16)]*4096;
            a /= 16;
            b /= 16;
    }
    return r;
}

int main( void )
{
    static volatile uint16_t limit = 8000;
    uint16_t a = 0, b = 0, n = limit;

    for(;a<n;a++)
    {
      for(b = 0;b<n;b++)
      {
        if ( AND(a,b) != (a&b) ) return printf( "AND error\n" );
        if ( IOR(a,b) != (a|b) ) return printf( "IOR error\n" );
        if ( XOR(a,b) != (a^b) ) return printf( "XOR error\n" );
      }
    }
    printf("andlut ok\n");
    return 0;
}
