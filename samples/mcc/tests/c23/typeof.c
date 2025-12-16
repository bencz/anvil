/*
 * C23 Test: typeof and typeof_unqual
 * Tests C23 typeof operators
 */

int global_int = 42;
const int const_int = 100;

int main(void)
{
    int x = 10;
    const int cx = 20;
    double d = 3.14;
    
    /* C23: typeof operator */
    typeof(x) y = 30;           /* int y */
    typeof(d) e = 2.718;        /* double e */
    typeof(global_int) z = 50;  /* int z */
    
    /* typeof with expression */
    typeof(x + 1) sum = x + y;  /* int sum */
    
    /* C23: typeof_unqual removes qualifiers */
    typeof_unqual(cx) mutable_copy = cx;  /* int (not const int) */
    mutable_copy = 200;  /* OK, not const */
    
    typeof_unqual(const_int) another = 300;
    another = 400;  /* OK */
    
    return y + z;
}
