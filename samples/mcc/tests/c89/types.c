/*
 * C89 Test: Basic Types
 * Tests all C89 basic types and type qualifiers
 */

/* Basic integer types */
char c;
signed char sc;
unsigned char uc;
short s;
short int si;
signed short ss;
unsigned short us;
int i;
signed int ssi;
unsigned int ui;
long l;
long int li;
signed long sl;
unsigned long ul;

/* Floating point types */
float f;
double d;
long double ld;

/* Type qualifiers */
const int ci = 10;
volatile int vi;
const volatile int cvi = 20;

/* Pointers */
int *pi;
const int *pci;
int * const cpi = 0;
const int * const cpci = 0;

/* Arrays */
int arr[10];
int arr2d[5][10];
char str[] = "hello";

/* Function declarations */
int func(int a, int b);
void vfunc(void);
int varargs(int n, ...);

int main(void)
{
    /* Local variables */
    int local = 42;
    const int clocal = 100;
    
    /* Pointer arithmetic */
    pi = &local;
    *pi = 50;
    
    /* Array access */
    arr[0] = 1;
    arr2d[0][0] = 2;
    
    return 0;
}
