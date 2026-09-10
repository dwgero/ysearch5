/* Generated-header checks, part of ysearch5; GPL-3.0-or-later. */
#include "../cuckoo.h"
#include "infinite.h"
#include <assert.h>

#ifndef HEADER_FIXTURE
#error "specify HEADER_FIXTURE (0 empty, 1 S/K, 2 zero, 3 zero plus S/K)"
#endif

_Static_assert(sizeof(infinite_keys) / sizeof(infinite_keys[0]) ==
               CUCKOO_SLOT_COUNT + CUCKOO_BUCKET_SIZE,
               "generated table must reserve its entire metadata bucket");
_Static_assert(INFINITE_KEY_CAPACITY == CUCKOO_SLOT_COUNT,
               "metadata must not increase the hashed slot capacity");
_Static_assert(INFINITE_KEY_COUNT ==
               ((HEADER_FIXTURE & 1U) != 0U ? 2U : 0U) +
               ((HEADER_FIXTURE & 2U) != 0U ? 1U : 0U),
               "generated count must include zero exactly once");

int main(void)
{
    int ordinary = (HEADER_FIXTURE & 1U) != 0U;
    int haszero = (HEADER_FIXTURE & 2U) != 0U;
    assert((uintptr_t)infinite_keys % (CUCKOO_BUCKET_SIZE * sizeof(uint64_t)) == 0U);
    assert(cuckoocontains(infinite_keys, 0U) == haszero);
    assert(cuckoocontains(infinite_keys, 1U) == ordinary);
    assert(cuckoocontains(infinite_keys, 2U) == ordinary);
    assert(cuckoocontains(infinite_keys, UINT64_C(0x35)) == 0);
    assert(infinite_keys[CUCKOO_SLOT_COUNT] == (uint64_t)haszero);
    for (size_t i = 1U; i < CUCKOO_BUCKET_SIZE; ++i)
        assert(infinite_keys[CUCKOO_SLOT_COUNT + i] == 0U);
    size_t nonzero = 0U;
    for (size_t i = 0U; i < CUCKOO_SLOT_COUNT; ++i) {
        if (infinite_keys[i] == 0U) continue;
        ++nonzero;
        assert(ordinary && (infinite_keys[i] == 1U || infinite_keys[i] == 2U));
    }
    assert(nonzero == (ordinary ? 2U : 0U));
    return 0;
}
