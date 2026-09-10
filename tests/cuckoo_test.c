/*
 * Cuckoo hash regression tests, part of ysearch5.
 * Copyright (C) 2026 by David W. Gero
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CUCKOO_ALLOCATOR_TEST
static int failallocation;
static size_t allocations, deallocations;
static size_t expectedalignment, expectedbytes;
static void *lastallocation;
static void *testallocate(size_t alignment, size_t bytes)
{
    assert(alignment == expectedalignment && bytes == expectedbytes);
    ++allocations;
    if (failallocation) return NULL;
    lastallocation = aligned_alloc(alignment, bytes);
    assert(lastallocation != NULL);
    memset(lastallocation, 0xa5, bytes);
    return lastallocation;
}
static void testfree(void *pointer)
{
    assert(pointer == lastallocation);
    ++deallocations;
    free(pointer);
    lastallocation = NULL;
}
#ifdef CUCKOO_MOCK_WINDOWS
static void *testwindowsallocate(size_t bytes, size_t alignment)
{
    return testallocate(alignment, bytes);
}
#define _WIN32 1
#define _aligned_malloc testwindowsallocate
#define _aligned_free testfree
#else
#define aligned_alloc testallocate
#define free testfree
#endif
#endif

#include "../cuckoo.h"

#define TEST_STORAGE_COUNT (CUCKOO_SLOT_COUNT + CUCKOO_BUCKET_SIZE)

#ifdef CUCKOO_ALLOCATOR_TEST
#undef aligned_alloc
#undef free
#undef _aligned_malloc
#undef _aligned_free
int main(void)
{
    expectedalignment = CUCKOO_BUCKET_SIZE * sizeof(uint64_t);
    expectedbytes = TEST_STORAGE_COUNT * sizeof(uint64_t);
    failallocation = 1;
    assert(cuckooallocate() == NULL);
    assert(allocations == 1U && deallocations == 0U);
    cuckoofree(NULL);
    assert(deallocations == 1U);
    failallocation = 0;
    uint64_t *keys = cuckooallocate();
    assert(keys != NULL && (uintptr_t)keys % 32U == 0U);
    for (size_t i = 0; i < TEST_STORAGE_COUNT; ++i) assert(keys[i] == 0U);
    keys[0] = 1U;
    keys[CUCKOO_SLOT_COUNT - 1U] = UINT64_MAX;
    assert(cuckooinsert(keys, 0U) == 1);
    assert(cuckoocontains(keys, 0U));
    failallocation = 1;
    assert(cuckooallocate() == NULL);
    assert(cuckoocontains(keys, 0U));
    assert(keys[0] == 1U && keys[CUCKOO_SLOT_COUNT - 1U] == UINT64_MAX);
    for (size_t i = CUCKOO_SLOT_COUNT + 1U; i < TEST_STORAGE_COUNT; ++i)
        assert(keys[i] == 0U);
    keys[TEST_STORAGE_COUNT - 1U] = UINT64_MAX;
    cuckoofree(keys);
    assert(allocations == 3U && deallocations == 2U);
    puts("allocation size/alignment/tail-zeroing/failure/matching-free passed");
    return 0;
}
#else
static uint64_t randomstate = UINT64_C(0xe619a347c728d905);
static uint64_t randomkey(void)
{
    randomstate += UINT64_C(0x9e3779b97f4a7c15);
    return cuckoomix(randomstate);
}

static int comparekeys(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int bothfull(const uint64_t *table, uint64_t key)
{
    size_t buckets[2];
    cuckoobuckets(key, buckets);
    for (size_t b = 0; b < 2U; ++b)
        for (size_t i = 0; i < CUCKOO_BUCKET_SIZE; ++i)
            if (table[buckets[b] * CUCKOO_BUCKET_SIZE + i] == 0U) return 0;
    return 1;
}

static size_t checktable(const uint64_t *table)
{
    /* Zero is membership metadata, not an occupied hashed slot. */
    assert(table[CUCKOO_SLOT_COUNT] <= 1U);
    assert(cuckoocontains(table, 0U) == (int)table[CUCKOO_SLOT_COUNT]);
    for (size_t i = CUCKOO_SLOT_COUNT + 1U; i < TEST_STORAGE_COUNT; ++i)
        assert(table[i] == 0U);
    size_t count = 0U;
    for (size_t i = 0; i < CUCKOO_SLOT_COUNT; ++i) {
        if (table[i] == 0U) continue;
        ++count;
        size_t buckets[2];
        cuckoobuckets(table[i], buckets);
        assert(buckets[0] < CUCKOO_BUCKET_COUNT);
        assert(buckets[1] < CUCKOO_BUCKET_COUNT);
        assert(i / CUCKOO_BUCKET_SIZE == buckets[0] ||
               i / CUCKOO_BUCKET_SIZE == buckets[1]);
        assert(cuckoocontains(table, table[i]));
    }
    return count;
}

/* Early insertion requires the invariants preserved by the insertion-only API.
 * Keep this separate from checktable: lookup and rollback fixtures may contain
 * manually placed keys without satisfying the stronger insertion contract.
 */
static size_t checkreachabletable(const uint64_t *table)
{
    size_t count = checktable(table);
    for (size_t bucket = 0; bucket < CUCKOO_BUCKET_COUNT; ++bucket) {
        int empty = 0;
        for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE; ++offset) {
            uint64_t key = table[bucket * CUCKOO_BUCKET_SIZE + offset];
            if (key == 0U) {
                empty = 1;
                continue;
            }
            assert(!empty); /* Each bucket has a dense occupied prefix. */
            size_t homes[2];
            cuckoobuckets(key, homes);
            if (bucket == homes[0]) continue;
            /* A secondary resident's primary bucket must already be full. */
            for (size_t first = 0; first < CUCKOO_BUCKET_SIZE; ++first)
                assert(table[homes[0] * CUCKOO_BUCKET_SIZE + first] != 0U);
        }
    }
    return count;
}

static void checkzero(uint64_t *table, uint64_t *snapshot, size_t bytes)
{
    memset(table, 0, bytes);
    assert(cuckoocontains(NULL, 0U) == 0);
    assert(cuckooinsert(NULL, 0U) == -1);
    assert(!cuckoocontains(table, 0U));
    memcpy(snapshot, table, bytes);
    snapshot[CUCKOO_SLOT_COUNT] = 1U;
    assert(cuckooinsert(table, 0U) == 1);
    assert(memcmp(table, snapshot, bytes) == 0);
    assert(checkreachabletable(table) == 0U);
    assert(cuckooinsert(table, 0U) == 0);
    assert(memcmp(table, snapshot, bytes) == 0);

    /* Allocating, changing, resetting, or freeing one map cannot reset another. */
    uint64_t *other = cuckooallocate();
    assert(other && !cuckoocontains(other, 0U));
    assert(cuckoocontains(table, 0U));
    assert(cuckooinsert(other, 1U) == 1);
    assert(!cuckoocontains(table, 1U));
    assert(cuckooinsert(other, 0U) == 1);
    assert(cuckooinsert(other, 0U) == 0);
    assert(checkreachabletable(other) == 1U);
    memset(table, 0, bytes);
    assert(!cuckoocontains(table, 0U) && cuckoocontains(other, 0U));
    assert(cuckooinsert(table, 0U) == 1);
    cuckoofree(other);
    assert(cuckoocontains(table, 0U));
    assert(memcmp(table, snapshot, bytes) == 0);
    puts("zero insertion/duplicate/tail placement/independent tables passed");
}

/* Find distinct, legal bucket residents without depending on catalogue keys. */
static uint64_t nextbucketkey(size_t bucket, uint64_t *cursor)
{
    size_t homes[2];
    do {
        assert(*cursor < UINT64_C(10000000));
        ++*cursor;
        cuckoobuckets(*cursor, homes);
    } while (homes[0] != bucket || homes[1] == bucket);
    return *cursor;
}

static void checkfastpaths(uint64_t *table, uint64_t *snapshot, size_t bytes)
{
    size_t buckets[2];
    uint64_t key = 1U;
    cuckoobuckets(key, buckets);
    while (buckets[0] == buckets[1]) cuckoobuckets(++key, buckets);
    uint64_t cursor = key;
    size_t primary = buckets[0] * CUCKOO_BUCKET_SIZE;
    size_t secondary = buckets[1] * CUCKOO_BUCKET_SIZE;

    /* Even when the secondary is empty, extend the primary occupied prefix. */
    memset(table, 0, bytes);
    for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE - 1U; ++offset)
        assert(cuckooinsert(table, nextbucketkey(buckets[0], &cursor)) == 1);
    assert(checkreachabletable(table) == CUCKOO_BUCKET_SIZE - 1U);
    memcpy(snapshot, table, bytes);
    snapshot[primary + CUCKOO_BUCKET_SIZE - 1U] = key;
    assert(cuckooinsert(table, key) == 1);
    assert(memcmp(table, snapshot, bytes) == 0);
    assert(checkreachabletable(table) == CUCKOO_BUCKET_SIZE);
    assert(cuckooinsert(table, key) == 0);
    assert(memcmp(table, snapshot, bytes) == 0);

    /* A full primary must select the first zero after the secondary prefix. */
    memset(table, 0, bytes);
    for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE; ++offset) {
        assert(cuckooinsert(table, nextbucketkey(buckets[0], &cursor)) == 1);
        if (offset < CUCKOO_BUCKET_SIZE - 1U)
            assert(cuckooinsert(table, nextbucketkey(buckets[1], &cursor)) == 1);
    }
    assert(checkreachabletable(table) == 2U * CUCKOO_BUCKET_SIZE - 1U);
    memcpy(snapshot, table, bytes);
    snapshot[secondary + CUCKOO_BUCKET_SIZE - 1U] = key;
    assert(cuckooinsert(table, key) == 1);
    assert(memcmp(table, snapshot, bytes) == 0);
    assert(checkreachabletable(table) == 2U * CUCKOO_BUCKET_SIZE);
    assert(cuckooinsert(table, key) == 0);
    assert(memcmp(table, snapshot, bytes) == 0);

    /* Lookup still supports arbitrary holes in either candidate bucket. Do not
     * insert into these manually populated, API-unreachable layouts.
     */
    for (size_t choice = 0; choice < 2U; ++choice) {
        for (size_t offset = 1U; offset < CUCKOO_BUCKET_SIZE; ++offset) {
            memset(table, 0, bytes);
            table[buckets[choice] * CUCKOO_BUCKET_SIZE + offset] = key;
            memcpy(snapshot, table, bytes);
            assert(cuckoocontains(table, key));
            assert(checktable(table) == 1U);
            assert(memcmp(table, snapshot, bytes) == 0);
        }
    }

    /* Independently reduced hashes may legitimately choose the same bucket. */
    key = 0U;
    do {
        assert(key < UINT64_C(10000000));
        cuckoobuckets(++key, buckets);
    } while (buckets[0] != buckets[1]);
    primary = buckets[0] * CUCKOO_BUCKET_SIZE;
    memset(table, 0, bytes);
    assert(cuckooinsert(table, key) == 1);
    assert(table[primary] == key && checkreachabletable(table) == 1U);
    memcpy(snapshot, table, bytes);
    assert(cuckooinsert(table, key) == 0);
    assert(memcmp(table, snapshot, bytes) == 0);

    /* The same-bucket incoming key must also survive a real eviction. */
    memset(table, 0, bytes);
    cursor = key;
    for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE; ++offset)
        assert(cuckooinsert(table, nextbucketkey(buckets[0], &cursor)) == 1);
    assert(checkreachabletable(table) == CUCKOO_BUCKET_SIZE);
    memcpy(snapshot, table, bytes);
    assert(cuckooinsert(table, key) == 1);
    assert(cuckoocontains(table, key));
    assert(checkreachabletable(table) == CUCKOO_BUCKET_SIZE + 1U);
    for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE; ++offset)
        assert(cuckoocontains(table, snapshot[primary + offset]));
    puts("primary preference, reachable duplicates, hole lookup, same-bucket choices passed");
}

/* Reduce a valid full table to only the buckets touched by a failed kick walk.
 * The resulting sparse fixture still has no empty slot along that bounded walk.
 * Include an overwritten slot visited twice to exercise journal reversal, not
 * just restoration of disjoint writes. The model is fixture construction only;
 * the assertions below test the actual public insertion/lookup implementation.
 */
static void checksparsefailure(uint64_t *table, uint64_t *snapshot,
                               size_t bytes, uint64_t firstkey)
{
    unsigned char *touched = calloc(CUCKOO_BUCKET_COUNT, sizeof(*touched));
    size_t victims[CUCKOO_MAX_KICKS];
    assert(touched);
    size_t repeated = 0U;
    uint64_t incoming = firstkey;
    for (size_t attempt = 0; attempt < 512U; ++attempt, ++incoming) {
        memset(touched, 0, CUCKOO_BUCKET_COUNT * sizeof(*touched));
        memcpy(snapshot, table, bytes);
        uint64_t current = incoming;
        size_t homes[2];
        cuckoobuckets(current, homes);
        size_t bucket = homes[0];
        touched[homes[1]] = 1U; /* Preserve the initial secondary fast-path scan. */
        repeated = 0U;
        for (size_t kick = 0; kick < CUCKOO_MAX_KICKS; ++kick) {
            touched[bucket] = 1U;
            size_t slot = bucket * CUCKOO_BUCKET_SIZE +
                          (size_t)((current + (uint64_t)kick) % CUCKOO_BUCKET_SIZE);
            for (size_t prior = 0; prior < kick; ++prior) {
                if (victims[prior] != slot) continue;
                ++repeated;
                break;
            }
            victims[kick] = slot;
            uint64_t evicted = snapshot[slot];
            assert(evicted != 0U);
            snapshot[slot] = current;
            current = evicted;
            cuckoobuckets(current, homes);
            bucket = bucket == homes[0] ? homes[1] : homes[0];
            touched[bucket] = 1U; /* Include the final alternate-slot scan. */
        }
        if (repeated != 0U) break;
    }
    assert(repeated != 0U);
    size_t retained = 0U;
    for (size_t bucket = 0; bucket < CUCKOO_BUCKET_COUNT; ++bucket) {
        if (touched[bucket]) {
            ++retained;
            continue;
        }
        memset(table + bucket * CUCKOO_BUCKET_SIZE, 0,
               CUCKOO_BUCKET_SIZE * sizeof(*table));
    }
    assert(retained > 0U && retained < CUCKOO_BUCKET_COUNT);
    assert(checktable(table) == retained * CUCKOO_BUCKET_SIZE);
    memcpy(snapshot, table, bytes);
    assert(!cuckoocontains(table, incoming));
    assert(cuckooinsert(table, incoming) == -1);
    assert(memcmp(snapshot, table, bytes) == 0);
    assert(checktable(table) == retained * CUCKOO_BUCKET_SIZE);
    printf("sparse failure: %zu occupied buckets, %zu repeated victim slots, atomic rollback\n",
           retained, repeated);
    free(touched);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const size_t bytes = sizeof(uint64_t) * TEST_STORAGE_COUNT;
    uint64_t *input = malloc(bytes);
    uint64_t *table = cuckooallocate();
    uint64_t *snapshot = malloc(bytes);
    assert(input && table && snapshot);
    assert((uintptr_t)table % 32U == 0U);
    assert(checkreachabletable(table) == 0U);
    FILE *stream = fopen(argv[1], "r");
    assert(stream);
    char line[128];
    size_t count = 0U;
    while (fgets(line, sizeof line, stream)) {
        uint64_t key;
        assert(strchr(line, '\n') != NULL);
        if (sscanf(line, "0x%" SCNx64 ":", &key) == 1) {
            assert(count < CUCKOO_SLOT_COUNT && key != 0U);
            input[count++] = key;
        }
    }
    assert(!ferror(stream));
    assert(fclose(stream) == 0);
    qsort(input, count, sizeof(*input), comparekeys);
    size_t unique = 0U;
    for (size_t i = 0; i < count; ++i)
        if (unique == 0U || input[i] != input[unique - 1U]) input[unique++] = input[i];
    count = unique;
    assert(count == 600907U); /* Complete current MAXLEN 12 catalogue. */
    assert(cuckoocontains(NULL, 1U) == 0);
    assert(cuckoocontains(table, 0U) == 0);
    assert(cuckooinsert(NULL, 1U) == -1);
    checkzero(table, snapshot, bytes);
    checkfastpaths(table, snapshot, bytes);
    memset(table, 0, bytes);
    size_t buckets[2];
    assert(cuckooinsert(table, input[0]) == 1);
    assert(checkreachabletable(table) == 1U);
    memcpy(snapshot, table, bytes);
    assert(cuckoocontains(table, input[0]));
    assert(cuckooinsert(table, input[0]) == 0);
    assert(memcmp(snapshot, table, bytes) == 0);
    for (size_t trial = 0U; trial < 7U; ++trial) {
        memset(table, 0, bytes);
        /* Ordinary insertions, including relocations, preserve zero membership. */
        assert(cuckooinsert(table, 0U) == 1);
        if (trial == 1U) {
            for (size_t i = 0; i < count / 2U; ++i) {
                uint64_t swap = input[i]; input[i] = input[count - 1U - i];
                input[count - 1U - i] = swap;
            }
        } else if (trial > 1U) {
            for (size_t i = count - 1U; i != 0U; --i) {
                size_t j = (size_t)(randomkey() % (i + 1U));
                uint64_t swap = input[i]; input[i] = input[j]; input[j] = swap;
            }
        }
        size_t relocations = 0U;
        for (size_t i = 0; i < count; ++i) {
            relocations += (size_t)bothfull(table, input[i]);
            int result = cuckooinsert(table, input[i]);
            if (result != 1) {
                fprintf(stderr, "Trial %zu failed at %zu result %d\n", trial, i, result);
                abort();
            }
        }
        assert(relocations > 0U && checkreachabletable(table) == count);
        assert(cuckoocontains(table, 0U));
        memcpy(snapshot, table, bytes);
        assert(cuckooinsert(table, 0U) == 0);
        for (size_t i = 0; i < count; ++i) {
            assert(cuckoocontains(table, input[i]));
            assert(cuckooinsert(table, input[i]) == 0);
        }
        assert(memcmp(snapshot, table, bytes) == 0);
        size_t maxmoves = 0U;
        for (size_t attempt = 0; attempt < 32U; ++attempt) {
            uint64_t key = randomkey();
            if (key == 0U || cuckoocontains(table, key)) continue;
            memcpy(snapshot, table, bytes);
            assert(cuckooinsert(table, key) == 1);
            size_t moves = 0U;
            for (size_t i = 0; i < CUCKOO_SLOT_COUNT; ++i)
                if (table[i] != snapshot[i]) ++moves;
            if (moves > maxmoves) maxmoves = moves;
            assert(checkreachabletable(table) == count + 1U);
            for (size_t i = 0; i < count; ++i) assert(cuckoocontains(table, input[i]));
            memcpy(table, snapshot, bytes);
        }
        assert(maxmoves > 2U);
        printf("trial %zu: %zu keys, %zu relocation insertions, max sampled path %zu\n",
               trial, count, relocations, maxmoves);
        fflush(stdout);
    }
    /* A valid full table: every distinct key is in its first candidate bucket. */
    memset(table, 0, bytes);
    size_t filled = 0U;
    uint64_t lastkey = 0U;
    while (filled < CUCKOO_SLOT_COUNT) {
        assert(lastkey < UINT64_C(10000000)); /* Bound fixture construction too. */
        ++lastkey;
        cuckoobuckets(lastkey, buckets);
        for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE; ++offset) {
            size_t slot = buckets[0] * CUCKOO_BUCKET_SIZE + offset;
            if (table[slot] != 0U) continue;
            table[slot] = lastkey;
            ++filled;
            break;
        }
    }
    assert(checktable(table) == CUCKOO_SLOT_COUNT);
    /* Zero remains insertable even when every normal hashed slot is occupied. */
    memcpy(snapshot, table, bytes);
    snapshot[CUCKOO_SLOT_COUNT] = 1U;
    assert(cuckooinsert(table, 0U) == 1);
    assert(memcmp(snapshot, table, bytes) == 0);
    assert(cuckooinsert(table, 0U) == 0);
    assert(memcmp(snapshot, table, bytes) == 0);
    memcpy(snapshot, table, bytes);
    for (uint64_t key = lastkey + 1U; key <= lastkey + 10U; ++key) {
        assert(!cuckoocontains(table, key));
        assert(cuckooinsert(table, key) == -1);
        assert(memcmp(snapshot, table, bytes) == 0);
    }
    assert(cuckooinsert(table, table[0]) == 0);
    assert(memcmp(snapshot, table, bytes) == 0);
    printf("valid full-table bounded failures preserve all %zu bytes\n", bytes);
    checksparsefailure(table, snapshot, bytes, lastkey + 1U);
    free(input); cuckoofree(table); free(snapshot);
    return 0;
}
#endif
