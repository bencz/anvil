/*
 * C23 Test: New Keywords
 * Tests C23 new keywords and features
 */

/* C23: true and false as keywords */
int flag1 = true;
int flag2 = false;

/* C23: nullptr constant */
int *null_ptr = nullptr;
void *void_ptr = nullptr;

/* C23: bool as keyword (not _Bool) */
bool b1 = true;
bool b2 = false;

/* C23: static_assert without underscore */
static_assert(sizeof(int) >= 4, "int must be at least 4 bytes");
static_assert(true, "true must be true");

/* C23: alignof without underscore */
int alignment = alignof(double);

int main(void)
{
    bool flag = true;
    int *p = nullptr;
    
    if (flag == true) {
        flag = false;
    }
    
    if (p == nullptr) {
        p = &alignment;
    }
    
    static_assert(sizeof(bool) >= 1, "bool must be at least 1 byte");
    
    return flag ? 1 : 0;
}
