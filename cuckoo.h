/*
 * cuckoo.h
 * Part of ysearch5
 * Copyright (C) 2026 by David W. Gero
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CUCKOO_H
#define CUCKOO_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <malloc.h>
#endif

#define CUCKOO_SLOT_COUNT 655360U
#define CUCKOO_BUCKET_SIZE 4U
#define CUCKOO_BUCKET_COUNT (CUCKOO_SLOT_COUNT / CUCKOO_BUCKET_SIZE)
#define CUCKOO_HALF_BUCKET_COUNT (CUCKOO_BUCKET_COUNT / 2U)
#define CUCKOO_HASH_VERSION 3U
#define CUCKOO_MAX_KICKS 160U

_Static_assert(CUCKOO_SLOT_COUNT % CUCKOO_BUCKET_SIZE == 0U,
               "cuckoo slots must form complete buckets");
_Static_assert(CUCKOO_BUCKET_COUNT > 1U && CUCKOO_SLOT_COUNT <= UINT32_MAX,
               "cuckoo slots must fit the rollback journal");
_Static_assert(CUCKOO_BUCKET_COUNT % 2U == 0U,
               "cuckoo buckets must form two equal halves");
_Static_assert(CUCKOO_MAX_KICKS > 0U,
               "cuckoo eviction needs a nonzero kick limit");

/* Keep each four-key bucket within one aligned 64-byte block. */
static inline uint64_t *cuckooallocate(void)
{
    size_t bytes = (CUCKOO_SLOT_COUNT + CUCKOO_BUCKET_SIZE) * sizeof(uint64_t);
    size_t alignment = CUCKOO_BUCKET_SIZE * sizeof(uint64_t);
#if defined(_WIN32) || defined(_WIN64)
    uint64_t *keys = _aligned_malloc(bytes, alignment);
#else
    uint64_t *keys = aligned_alloc(alignment, bytes);
#endif
    if (keys != NULL) memset(keys, 0, bytes);
    return keys;
}

static inline void cuckoofree(uint64_t *keys)
{
#if defined(_WIN32) || defined(_WIN64)
    _aligned_free(keys);
#else
    free(keys);
#endif
}

/* Changing these hashes requires a new version and regenerated infinite.h. */

/* MurmurHash3 fmix64 selects a bucket in the first half. */
static inline uint32_t cuckoohash1(uint64_t key)
{
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    key *= UINT64_C(0xc4ceb9fe1a85ec53);
    key ^= key >> 33;
    return (uint32_t)(key % CUCKOO_HALF_BUCKET_COUNT);
}

/* The SplitMix64 finalizer selects a bucket in the second half. */
static inline uint32_t cuckoohash2(uint64_t key)
{
    key ^= key >> 30;
    key *= UINT64_C(0xbf58476d1ce4e5b9);
    key ^= key >> 27;
    key *= UINT64_C(0x94d049bb133111eb);
    key ^= key >> 31;
    return CUCKOO_HALF_BUCKET_COUNT +
           (uint32_t)(key % CUCKOO_HALF_BUCKET_COUNT);
}

static inline void cuckoobuckets(uint64_t key, size_t buckets[2])
{
    buckets[0] = cuckoohash1(key);
    buckets[1] = cuckoohash2(key);
}

static inline int cuckoocontains(const uint64_t *keys, uint64_t key)
{
    if (keys == NULL) return 0;
    if (key == 0) return (int)keys[CUCKOO_SLOT_COUNT];
    for (size_t choice = 0; choice < 2U; ++choice) {
        size_t bucket = choice == 0U ? cuckoohash1(key) : cuckoohash2(key);
        size_t start = bucket * CUCKOO_BUCKET_SIZE;
        for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE; ++offset) {
            if (keys[start + offset] == key) return 1;
        }
    }
    return 0;
}

/*
 * Evict into each displaced key's other bucket, as in cuckoonew.h. Record
 * overwritten slots in a journal, not a second key table. If the bounded
 * walk fails, reverse every swap to restore the original table, even when the
 * walk revisits a slot.
 */
static inline int cuckoorelocate(uint64_t *keys, uint64_t key,
                               const size_t buckets[2])
{
    uint32_t journal[CUCKOO_MAX_KICKS];
    uint64_t currentkey = key;
    size_t currentbucket = buckets[0];
    for (size_t kick = 0; kick < CUCKOO_MAX_KICKS; ++kick) {
        size_t victim = currentbucket * CUCKOO_BUCKET_SIZE +
            (size_t)((currentkey + (uint64_t)kick) % CUCKOO_BUCKET_SIZE);
        journal[kick] = (uint32_t)victim;
        uint64_t evicted = keys[victim];
        keys[victim] = currentkey;
        currentkey = evicted;

        size_t alternatives[2];
        cuckoobuckets(currentkey, alternatives);
        currentbucket = currentbucket == alternatives[0] ?
                        alternatives[1] : alternatives[0];
        size_t start = currentbucket * CUCKOO_BUCKET_SIZE;
        for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE; ++offset) {
            if (keys[start + offset] != 0U) continue;
            keys[start + offset] = currentkey;
            return 1;
        }
    }
    /* Failed.  Restore original table. */
    for (size_t kick = CUCKOO_MAX_KICKS; kick != 0U; --kick) {
        size_t slot = journal[kick - 1U];
        uint64_t displaced = keys[slot];
        keys[slot] = currentkey;
        currentkey = displaced;
    }
    return -1;
}

/* Return 1 for insertion, 0 for an existing key, and -1 without mutation on
 * exhaustion. Callers serialize mutations; lookup may be shared only while
 * the table is immutable. No ownership or allocation occurs here.
 */
static inline int cuckooinsert(uint64_t *keys, uint64_t key)
{
    if (keys == NULL) return -1;
    if (key == 0) {
        if (keys[CUCKOO_SLOT_COUNT]) return 0;
        keys[CUCKOO_SLOT_COUNT] = 1;
        return 1;
    }
    size_t buckets[2];
    cuckoobuckets(key, buckets);
    for (size_t choice = 0; choice < 2U; ++choice) {
        size_t start = buckets[choice] * CUCKOO_BUCKET_SIZE;
        for (size_t offset = 0; offset < CUCKOO_BUCKET_SIZE; ++offset) {
            uint64_t stored = keys[start + offset];
            if (stored == key) return 0;
            if (stored == 0U) {
                keys[start + offset] = key;
                return 1;
            }
        }
    }
    return cuckoorelocate(keys, key, buckets);
}

#endif /* CUCKOO_H */
