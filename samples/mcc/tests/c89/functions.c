/*
 * C89 Test: Functions
 * Tests function declarations, definitions, and calls
 */

/* Function prototypes */
int add(int a, int b);
void swap(int *a, int *b);
int factorial(int n);
int max(int a, int b);

/* Variadic function */
int sum(int count, ...);

/* Static function */
static int helper(int x)
{
    return x * 2;
}

/* Function with no parameters */
int get_value(void)
{
    return 42;
}

/* Function returning pointer */
int *get_ptr(int *arr, int index)
{
    return &arr[index];
}

/* Function with array parameter */
int array_sum(int arr[], int size)
{
    int i, total = 0;
    for (i = 0; i < size; i++) {
        total = total + arr[i];
    }
    return total;
}

/* Function with struct parameter */
struct point {
    int x, y;
};

int point_distance(struct point p1, struct point p2)
{
    int dx = p2.x - p1.x;
    int dy = p2.y - p1.y;
    return dx * dx + dy * dy;
}

/* Recursive function */
int factorial(int n)
{
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

/* Function definitions */
int add(int a, int b)
{
    return a + b;
}

void swap(int *a, int *b)
{
    int temp = *a;
    *a = *b;
    *b = temp;
}

int max(int a, int b)
{
    return a > b ? a : b;
}

int main(void)
{
    int x = 5, y = 10;
    int arr[5] = {1, 2, 3, 4, 5};
    struct point p1 = {0, 0};
    struct point p2 = {3, 4};
    int result;
    
    /* Function calls */
    result = add(x, y);
    result = max(x, y);
    result = factorial(5);
    result = get_value();
    result = helper(x);
    result = array_sum(arr, 5);
    result = point_distance(p1, p2);
    
    /* Pointer parameter */
    swap(&x, &y);
    
    /* Pointer return */
    int *p = get_ptr(arr, 2);
    result = *p;
    
    return 0;
}
