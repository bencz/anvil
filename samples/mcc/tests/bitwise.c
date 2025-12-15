/* Bitwise operations test for MCC */

int bit_ops(int a, int b)
{
    int c = a & b;
    int d = a | b;
    int e = a ^ b;
    int f = ~a;
    return c + d + e + f;
}

int shifts(int a, int n)
{
    int left = a << n;
    int right = a >> n;
    return left + right;
}

int main(void)
{
    int x;
    int y;
    int result;
    
    x = 0x0F;
    y = 0xF0;
    
    result = bit_ops(x, y);
    result = shifts(x, 2);
    
    return result;
}
