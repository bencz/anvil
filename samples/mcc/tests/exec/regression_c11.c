struct outer {
    int tag;
    struct { int x; int y; };
};

static int as_int(int x) { return x; }
static double as_double(double x) { return x; }

#define dispatch(x) _Generic((x), int: as_int, double: as_double)(x)

int main(void)
{
    struct outer o;
    double positive = 3.0;
    double negative = -2.0;
    double minus_zero = -0.0;
    double plus_zero = 0.0;
    double nan;

    nan = plus_zero / plus_zero;
    o.tag = 1;
    o.x = 20;
    o.y = 22;

    if (o.x + o.y != 42) return 1;
    if (dispatch(7) != 7 || dispatch(2.5) != 2.5) return 2;
    if (!(negative < plus_zero) || !(positive > plus_zero)) return 3;
    if (!(negative <= negative) || !(positive >= positive)) return 4;
    if (!(minus_zero == plus_zero) || minus_zero < plus_zero ||
        minus_zero > plus_zero) return 5;
    if (nan == nan || !(nan != nan)) return 6;
    if (nan < plus_zero || nan <= plus_zero ||
        nan > plus_zero || nan >= plus_zero) return 7;
    if (!nan || plus_zero) return 8;
    return 0;
}
