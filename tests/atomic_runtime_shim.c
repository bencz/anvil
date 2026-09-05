#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform/threads.h"

typedef uint64_t (*atomic_function)(void *, uint64_t, uint64_t);
typedef struct {
    atomic_function function;
    unsigned size;
    unsigned operation;
    unsigned order;
} atomic_case;

#include "atomic_cases.h"

typedef struct {
    uint64_t counter;
    uint64_t sums[4];
} concurrent_case;

static void increment(void *opaque, size_t index)
{
    concurrent_case *state = opaque;
    uint64_t sum = 0;
    for (unsigned iteration = 0; iteration < 50000; iteration++)
        sum += atomic_case_3_3_0(&state->counter, 1, 0);

    state->sums[index] = sum;
}

static void increment_with_compare(void *opaque, size_t index)
{
    concurrent_case *state = opaque;
    uint64_t sum = 0;
    uint64_t expected = atomic_case_3_0_0(&state->counter, 0, 0);
    for (unsigned iteration = 0; iteration < 50000; iteration++)
    {
        for (;;)
        {
            uint64_t old = atomic_case_3_8_4(&state->counter, expected + 1, expected);
            if (old == expected)
            {
                sum += old;
                expected = old + 1;
                break;
            }

            expected = old;
        }
    }

    state->sums[index] = sum;
}

typedef struct {
    uint32_t ready;
    unsigned payload;
    bool failed;
} publication_case;

static void publish(void *opaque, size_t index)
{
    publication_case *state = opaque;
    for (unsigned iteration = 1; iteration <= 20000; iteration++)
    {
        uint64_t wanted = index == 0 ? 0 : 1;
        while (atomic_case_2_0_1(&state->ready, 0, 0) != wanted)
        {
        }

        if (index == 0)
            state->payload = iteration;
        else if (state->payload != iteration)
            state->failed = true;

        atomic_case_2_1_2(&state->ready, wanted ^ 1, 0);
    }
}

int main(void)
{
    uint64_t inputs[32];
    uint64_t sum = 0;
    for (unsigned index = 0; index < 32; index++)
    {
        inputs[index] = UINT64_C(0x13931a187f) + index * 17;
        sum += inputs[index];
    }

    for (unsigned mismatch = 0; mismatch < 2; mismatch++)
    {
        uint64_t storage = inputs[0] + mismatch;
        uint64_t expected = storage + sum;
        uint64_t observed = atomic_pressure(&storage, inputs);
        if (observed != expected || storage != (mismatch ? inputs[0] + 1 : inputs[1]))
        {
            fprintf(stderr, "atomic CAS failed under register pressure\n");
            return 1;
        }
    }

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        const atomic_case *test = &cases[index];
        uint64_t mask = test->size == 8 ? UINT64_MAX : (UINT64_C(1) << (test->size * 8)) - 1;
        for (unsigned sample = 0; sample < 32; sample++)
        {
            uint64_t storage = UINT64_C(0xa7b392cdfe019583) ^ (sample * UINT64_C(0x193387aba2));
            uint64_t untouched = storage & ~mask;
            uint64_t original = storage & mask;
            uint64_t operand = (UINT64_C(0xf131def71913) + sample) & mask;
            uint64_t expected = sample & 1 ? original : original ^ 1;
            uint64_t after = original;
            switch (test->operation)
            {
                case 1:
                case 2:
                    after = operand;
                    break;
                case 3:
                    after += operand;
                    break;
                case 4:
                    after -= operand;
                    break;
                case 5:
                    after &= operand;
                    break;
                case 6:
                    after |= operand;
                    break;
                case 7:
                    after ^= operand;
                    break;
                case 8:
                    if (expected == original)
                        after = operand;
                    break;
                default:
                    break;
            }

            uint64_t old = test->function(&storage, operand, expected);
            uint64_t wanted_old = test->operation == 1 ? 0 : original;
            uint64_t wanted_storage = untouched | (after & mask);
            if (old != wanted_old || storage != wanted_storage)
            {
                fprintf(stderr, "atomic case failed: size=%u op=%u order=%u sample=%u old=%llu expected=%llu\n",
                        test->size, test->operation, test->order, sample, (unsigned long long)old, (unsigned long long)wanted_old);
                return 1;
            }
        }
    }

    for (unsigned compare = 0; compare < 2; compare++)
    {
        concurrent_case concurrent = { 0 };
        if (!anvil_test_run_threads(4, compare ? increment_with_compare : increment, &concurrent))
            return 1;

        uint64_t total = 0;
        for (unsigned index = 0; index < 4; index++)
            total += concurrent.sums[index];

        if (concurrent.counter != 200000 || total != UINT64_C(200000) * 199999 / 2)
        {
            fprintf(stderr, "concurrent atomic increments lost updates or returned duplicate old values\n");
            return 1;
        }
    }

    publication_case publication = { 0 };
    if (!anvil_test_run_threads(2, publish, &publication) || publication.failed)
    {
        fprintf(stderr, "release/acquire publication failed\n");
        return 1;
    }

    printf("%zu atomic cases, 400000 concurrent updates and 20000 release/acquire publications passed\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
