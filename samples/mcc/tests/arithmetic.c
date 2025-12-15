/* Arithmetic operations test for MCC */

int add(int a, int b)
{
    return a + b;
}

int sub(int a, int b)
{
    return a - b;
}

int mul(int a, int b)
{
    return a * b;
}

int div_test(int a, int b)
{
    return a / b;
}

int mod(int a, int b)
{
    return a % b;
}

int main(void)
{
    int x;
    int y;
    int result;
    
    x = 10;
    y = 3;
    
    result = add(x, y);
    result = sub(x, y);
    result = mul(x, y);
    result = div_test(x, y);
    result = mod(x, y);
    
    return result;
}
