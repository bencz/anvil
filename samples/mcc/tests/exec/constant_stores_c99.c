/* Constants stored to every width of field, through a pointer and through
 * an index: zero, small values, values a halfword immediate cannot hold and
 * the same constant stored again (one register may serve several stores). */
#include <stdio.h>

struct record {
    long long wide;
    long long other;
    int word;
    unsigned uword;
    short half;
    unsigned char byte;
    signed char sbyte;
};

static void clear(struct record *r)
{
    r->wide = 0;
    r->other = 0;
    r->word = 0;
    r->uword = 0;
    r->half = 0;
    r->byte = 0;
    r->sbyte = 0;
}

static void fill(struct record *r)
{
    r->wide = -3;
    r->other = 1LL << 40;
    r->word = 100000;
    r->uword = 4000000000u;
    r->half = -32768;
    r->byte = 200;
    r->sbyte = -100;
}

static void same(struct record *r, int *table, long i)
{
    r->word = 7;
    r->uword = 7;
    table[i] = 7;
    table[i + 1] = 0;
    table[i + 2] = -1;
    r->half = 7;
}

static void show(const struct record *r)
{
    printf("%lld %lld %d %u %d %u %d\n", r->wide, r->other, r->word, r->uword, r->half, r->byte, r->sbyte);
}

int main(void)
{
    struct record records[3];
    int table[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for (int i = 0; i < 3; i++) {
        fill(&records[i]);
        show(&records[i]);
    }
    clear(&records[1]);
    show(&records[1]);
    same(&records[2], table, 3);
    show(&records[2]);
    for (int i = 0; i < 8; i++)
        printf("%d ", table[i]);
    printf("\n");
    return 0;
}
