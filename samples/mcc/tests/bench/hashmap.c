/* Open-addressing hash table: multiplicative hashing, probing, pointer
 * chasing through structs. */
#include <stdio.h>
#include <stdlib.h>

typedef struct { unsigned key; int value; int used; } slot_t;

#define CAPACITY (1u << 20)
static slot_t slots[CAPACITY];

static slot_t *find(unsigned key)
{
    unsigned index = (key * 2654435761u) & (CAPACITY - 1u);
    while (slots[index].used && slots[index].key != key)
        index = (index + 1u) & (CAPACITY - 1u);
    return &slots[index];
}

int main(void)
{
    unsigned state = 7u;
    long hits = 0;
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < 600000; i++) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            slot_t *s = find(state & 0x7ffffu);
            if (s->used) {
                s->value++;
                hits++;
            } else {
                s->used = 1;
                s->key = state & 0x7ffffu;
                s->value = 1;
            }
        }
    }
    long total = 0;
    for (unsigned i = 0; i < CAPACITY; i++)
        total += slots[i].used ? slots[i].value : 0;
    printf("hashmap %ld %ld\n", hits, total);
    return 0;
}
