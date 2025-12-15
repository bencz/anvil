/* Control flow test for MCC */

int factorial(int n)
{
    int result;
    
    result = 1;
    while (n > 1) {
        result = result * n;
        n = n - 1;
    }
    return result;
}

int fibonacci(int n)
{
    int a;
    int b;
    int temp;
    int i;
    
    if (n <= 1) {
        return n;
    }
    
    a = 0;
    b = 1;
    for (i = 2; i <= n; i = i + 1) {
        temp = a + b;
        a = b;
        b = temp;
    }
    return b;
}

int is_even(int n)
{
    if (n % 2 == 0) {
        return 1;
    } else {
        return 0;
    }
}

int main(void)
{
    int f;
    int fib;
    
    f = factorial(5);
    fib = fibonacci(10);
    
    return is_even(f);
}
