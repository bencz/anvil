/*
 * C89 Test: Operators
 * Tests all C89 operators
 */

int main(void)
{
    int a = 10, b = 3, c;
    int *p;
    
    /* Arithmetic operators */
    c = a + b;      /* addition */
    c = a - b;      /* subtraction */
    c = a * b;      /* multiplication */
    c = a / b;      /* division */
    c = a % b;      /* modulo */
    c = -a;         /* unary minus */
    c = +a;         /* unary plus */
    
    /* Relational operators */
    c = a == b;     /* equal */
    c = a != b;     /* not equal */
    c = a < b;      /* less than */
    c = a > b;      /* greater than */
    c = a <= b;     /* less or equal */
    c = a >= b;     /* greater or equal */
    
    /* Logical operators */
    c = a && b;     /* logical and */
    c = a || b;     /* logical or */
    c = !a;         /* logical not */
    
    /* Bitwise operators */
    c = a & b;      /* bitwise and */
    c = a | b;      /* bitwise or */
    c = a ^ b;      /* bitwise xor */
    c = ~a;         /* bitwise not */
    c = a << 2;     /* left shift */
    c = a >> 1;     /* right shift */
    
    /* Assignment operators */
    c = a;
    c += b;
    c -= b;
    c *= b;
    c /= b;
    c %= b;
    c &= b;
    c |= b;
    c ^= b;
    c <<= 1;
    c >>= 1;
    
    /* Increment/decrement */
    c = a++;
    c = ++a;
    c = a--;
    c = --a;
    
    /* Ternary operator */
    c = a > b ? a : b;
    
    /* Comma operator */
    c = (a = 1, b = 2, a + b);
    
    /* Pointer operators */
    p = &a;         /* address-of */
    c = *p;         /* dereference */
    
    /* sizeof operator */
    c = sizeof(int);
    c = sizeof a;
    c = sizeof(a);
    
    /* Cast operator */
    c = (int)3.14;
    
    return 0;
}
