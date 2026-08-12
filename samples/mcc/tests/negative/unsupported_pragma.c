#pragma pack(push, 1)
struct would_have_changed_abi {
    char tag;
    int value;
};
#pragma pack(pop)

int main(void) { return 0; }
