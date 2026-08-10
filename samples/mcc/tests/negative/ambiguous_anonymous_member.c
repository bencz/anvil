struct ambiguous {
    struct { int value; };
    struct { int value; };
};

int main(void)
{
    struct ambiguous object;
    return object.value;
}
