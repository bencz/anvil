#define type_name(x) _Generic((x), \
    int: "int", \
    float: "float", \
    double: "double", \
    default: "unknown")

int main(void) {
    int i = 0;
    float f = 0.0f;
    return 0;
}
