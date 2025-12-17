/*
 * Bitwise Operations with Lookup Table
 * Tests: macros, lookup tables, nested macro expansion
 */

int putchar(int c);
int printf(const char *fmt, ...);

typedef unsigned short uint16_t;

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
    uint16_t a, b;

    /* Test a smaller range to keep test fast */
    for(a = 0; a < 1024; a++)
    {
      for(b = 0; b < 1024; b++)
      {    
        if ( AND(a,b) != (a&b) ) { printf( "AND error\n" ); return 1; }
        if ( IOR(a,b) != (a|b) ) { printf( "IOR error\n" ); return 1; }
        if ( XOR(a,b) != (a^b) ) { printf( "XOR error\n" ); return 1; }
      }
    }
    printf("OK\n");
    return 0;
}
