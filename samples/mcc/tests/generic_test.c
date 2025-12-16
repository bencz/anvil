#define type_name(x) _Generic((x), \
    int: "int", \
    float: "float", \
    double: "double", \
    default: "unknown")

int main(void) {
    int i = 0;
    float f = 0.0f;
    const char *s1 = type_name(i);
    const char *s2 = type_name(f);
    return 0;
}
