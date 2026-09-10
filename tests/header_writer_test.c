/* Header-writer fixtures, part of ysearch5; GPL-3.0-or-later. */
#define main header_writer_original_main
#if defined(TEST_YSEARCH_WRITER)
#define HAS_INFINITE_H 0
#define SINGLE_THREAD 1
#define DOTESTS 0
#define DOSEARCH 0
#include "../main.c"
#else
#include "../makeinfh.c"
#endif
#undef main
#include <assert.h>

static char *testpath(const char *directory, const char *filename)
{
    size_t length = strlen(directory) + strlen(filename) + 2U;
    char *path = malloc(length);
    assert(path != NULL);
    int result = snprintf(path, length, "%s/%s", directory, filename);
    assert(result >= 0 && (size_t)result < length);
    return path;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    assert(strlen(argv[2]) == 1U && argv[2][0] >= '0' && argv[2][0] <= '3');
    unsigned fixture = (unsigned)(argv[2][0] - '0');
    uint64_t *keys = cuckooallocate();
    assert(keys != NULL);
    size_t count = 0U;
    if ((fixture & 1U) != 0U) {
        assert(cuckooinsert(keys, UINT64_C(1)) == 1);
        assert(cuckooinsert(keys, UINT64_C(2)) == 1);
        count += 2U;
    }
    if ((fixture & 2U) != 0U) {
        assert(cuckooinsert(keys, UINT64_C(0)) == 1);
        ++count;
    }
    size_t bytes = (CUCKOO_SLOT_COUNT + CUCKOO_BUCKET_SIZE) * sizeof(*keys);
    uint64_t *snapshot = malloc(bytes);
    assert(snapshot != NULL);
    memcpy(snapshot, keys, bytes);

#if defined(TEST_YSEARCH_WRITER)
    infinitepath = testpath(argv[1], "infinite.cmb");
    neverendingset.keys = keys;
    neverendingset.capacity = CUCKOO_SLOT_COUNT;
    neverendingset.size = count;
    assert(writeinfiniteheader() == EXIT_SUCCESS);
    free(infinitepath);
    infinitepath = NULL;
    neverendingset.keys = NULL;
    neverendingset.capacity = neverendingset.size = 0U;
#else
    char *outputpath = testpath(argv[1], "infinite.h");
    char *temppath = testpath(argv[1], "infinite.h.tmp");
    KeyTable table = {keys, count};
    writeheader(outputpath, temppath, &table);
    free(outputpath);
    free(temppath);
#endif
    assert(memcmp(snapshot, keys, bytes) == 0);
    free(snapshot);
    cuckoofree(keys);
    return EXIT_SUCCESS;
}
