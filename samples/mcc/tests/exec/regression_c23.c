int global_value = 17;
bool global_truth = 2;
bool global_false = 0;

int main(void)
{
    int *p = nullptr;
    bool selected = true;
    bool converted = 6;
    if (!global_truth || global_false || !converted) return 4;
    if (p != nullptr) return 1;
    p = &global_value;
    if (p == nullptr || *p != 17) return 2;
    selected = false;
    return selected ? 3 : 0;
}
