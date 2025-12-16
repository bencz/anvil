/*
 * C11 Test: _Generic Selection
 * Tests C11 generic selection expression
 */

#define type_name(x) _Generic((x), \
    int: "int", \
    float: "float", \
    double: "double", \
    char *: "char *", \
    default: "unknown")

#define abs_val(x) _Generic((x), \
    int: abs_int, \
    float: abs_float, \
    double: abs_double)(x)

int abs_int(int x) { return x < 0 ? -x : x; }
float abs_float(float x) { return x < 0 ? -x : x; }
double abs_double(double x) { return x < 0 ? -x : x; }

int main(void)
{
    int i = -5;
    float f = -3.14f;
    double d = -2.718;
    char *s = "hello";
    
    /* _Generic for type dispatch */
    const char *ti = type_name(i);
    const char *tf = type_name(f);
    const char *td = type_name(d);
    const char *ts = type_name(s);
    
    /* _Generic for function dispatch */
    int ai = abs_val(i);
    float af = abs_val(f);
    double ad = abs_val(d);
    
    return ai;
}
