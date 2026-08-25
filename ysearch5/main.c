/*
 * main.c
 * part of ysearch5
 * Copyright (C) 2026 by David W. Gero.  All Rights Reserved.
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

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>
#include <sched.h>
#include <inttypes.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// Handle platform differences for isatty and fileno
#if defined(_WIN32) || defined(_WIN64)
    #include <io.h>
    #define ISATTY _isatty
    #define FILENO _fileno
#else
    #include <unistd.h>
    #define ISATTY isatty
    #define FILENO fileno
#endif

#if 1
    // for C11, C++11 and more recent
    #define PERTHREAD _Thread_local
#elif 1
    // for old Sun Studio C/C++, IBM XL C/C++, GNU C, LLVM Clang, and Intel C/C++ (on Linux systems)
    #define PERTHREAD __thread
#else
    // for Visual C++, Intel C/C++ (on Windows systems), Borland C++ Builder, and Digital Mars C++
    #define PERTHREAD __declspec(thread)
#endif

#define MAXLEN 12
#define MAXBUF ((3 * MAXLEN) - 1)
#define MAXSTR maxstr
#define MAXSTEPS maxsteps
#define MAXARRAY (32U * 1024U)
#define WORKER_STACK_SIZE (1024U * 1024U)
#define MAXSTRTABLE_11 20480U

#ifndef SINGLE_THREAD
#define SINGLE_THREAD 0
#endif
#ifndef PARANOID
#define PARANOID 0
#endif
#ifndef DOTESTS
#define DOTESTS 0
#endif
#ifndef DOSEARCH
#define DOSEARCH 1
#endif
#ifndef PRINTMAXES
#define PRINTMAXES 1
#endif
#ifndef HAS_INFINITE_H
#define HAS_INFINITE_H 1
#endif

#if HAS_INFINITE_H
#include "infinite.h"
#endif

#define MAXTHREADALLOC 64
_Static_assert(MAXTHREADALLOC >= 1 && MAXTHREADALLOC <= 64,
               "MAXTHREADALLOC must be between 1 and 64");

#if !SINGLE_THREAD
    static int maxthreads;
    static uint64_t allthreadsmask;
#endif

#if defined(__clang__)
    #define DEBUG_TRAP() __builtin_debugtrap()
#elif defined(__GNUC__)
    #include <signal.h>
    #define DEBUG_TRAP() raise(SIGTRAP)
#else
    #define DEBUG_TRAP() abort()
#endif

#ifdef DEBUG
    #define INT3 fflush(stdout);fflush(stderr);DEBUG_TRAP();
#else
    #define INT3 fflush(stdout);fflush(stderr);
#endif

#if !SINGLE_THREAD
static inline unsigned ctz64(uint64_t x)
{
    if (x == 0) {
        return 64;
    }

#if (defined(__IBMC__) || defined(__IBMCPP__)) && !defined(__clang__)
    return (unsigned)__cnttz8((unsigned long long)x);

#elif defined(__clang__) || defined(__GNUC__)
    return (unsigned)__builtin_ctzll((unsigned long long)x);

#elif defined(_MSC_VER)
    unsigned long index;

    #if defined(_M_X64) || defined(_M_ARM64)
        (void)_BitScanForward64(&index, (unsigned __int64)x);
        return (unsigned)index;
    #else
        if (_BitScanForward(&index, (unsigned long)x)) {
            return (unsigned)index;
        }
        (void)_BitScanForward(&index, (unsigned long)(x >> 32));
        return (unsigned)index + 32;
    #endif

#else
    // 1. Isolate the lowest set bit
    uint64_t lowest_set_bit = x & -x;

    // 2. Define the 64-entry De Bruijn lookup table
    static const unsigned MultiplyDeBruijnBitPosition64[64] = {
        0,  1,  2, 53,  3,  7, 54, 27,  4, 38, 41,  8, 34, 55, 48, 28,
       62,  5, 39, 46, 44, 42, 22,  9, 24, 35, 59, 56, 49, 18, 29, 11,
       63, 52,  6, 26, 37, 40, 33, 47, 61, 45, 43, 21, 23, 58, 17, 10,
       51, 25, 36, 32, 60, 20, 57, 16, 50, 31, 19, 15, 30, 14, 13, 12
    };

    // 3. Multiply by 64-bit De Bruijn constant and shift right by 58 to get a unique 6-bit index
    return MultiplyDeBruijnBitPosition64[(uint64_t)(lowest_set_bit * 0x022FDD63CC95386DULL) >> 58];

#endif
}
#endif

#if DOSEARCH
static inline unsigned popcount32(uint32_t x)
{
#if (defined(__IBMC__) || defined(__IBMCPP__)) && !defined(__clang__)
    return (unsigned)__popcnt4((unsigned int)x);

#elif defined(__clang__) || defined(__GNUC__)
    return (unsigned)__builtin_popcount((unsigned int)x);

#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    return (unsigned)__popcnt((unsigned int)x);

#else
    // 1. Count bits in pairs (2-bit chunks): 00->00, 01->01, 10->01, 11->10
    x -= (x >> 1) & 0x55555555;

    // 2. Merge pairs into 4-bit nibbles
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);

    // 3. Merge nibbles into 8-bit bytes (sum of bits in each byte)
    x = (x + (x >> 4)) & 0x0F0F0F0F;

    // 4. Multiply by 0x01010101 to accumulate all byte sums into the top 8 bits, then shift right
    return (x * 0x01010101) >> 24;

#endif
}
#endif

uint_fast32_t maxstrtable[12] = {
    // 0    1     2     3     4     5     6
    1024, 1024, 1024, 1024, 1024, 1024, 1024,
    // 7    8     9    10    11
    1024, 2048, 4096, 4096, MAXSTRTABLE_11
};

uint_fast32_t maxsteptable[12] = {
    // 0    1     2     3     4     5     6
    1024, 1024, 1024, 1024, 1024, 1024, 1024,
    // 7    8     9     10     11
    2048, 2048, 4096, 10240, 2048000
};

_Static_assert(MAXLEN <= (sizeof(maxstrtable) / sizeof(maxstrtable[0])),
               "MAXLEN exceeds maxstrtable capacity");
_Static_assert(MAXLEN <= (sizeof(maxsteptable) / sizeof(maxsteptable[0])),
               "MAXLEN exceeds maxsteptable capacity");
_Static_assert(MAXSTRTABLE_11 < MAXARRAY,
               "maxstrtable[11] must be less than MAXARRAY");

uint_fast32_t maxstr = 1024;
uint_fast32_t maxsteps = 1024;

pthread_mutex_t globallock = PTHREAD_MUTEX_INITIALIZER;
// globallock protects changes to the following:
_Atomic(uint_fast32_t) maxstep;
_Atomic(uint_fast32_t) maxstepstrlen;
atomic_int bufmaxis0 = 1;
char bufmax[MAXBUF + 1] = "";
_Atomic(uint_fast32_t) maxlen;
_Atomic(uint_fast32_t) maxlenstep;
char bufmaxlen[MAXBUF + 1];

pthread_mutex_t printlock = PTHREAD_MUTEX_INITIALIZER;

#if !HAS_INFINITE_H
pthread_mutex_t infinitefilelock = PTHREAD_MUTEX_INITIALIZER;
FILE *infinitefile;
#endif
char *infinitepath;

// Allocator state changes on every cell allocation/free. Keep it private to
// each worker so independent evaluators do not contend for one cache line.
// The cell arrays themselves remain selected by worker id.
#if SINGLE_THREAD
    #define next nxt[0]
    #define contents cnts[0]
    static uint_fast32_t highwatermark;
    static uint_fast32_t freelist;
#else
    typedef struct {
        int id;
        uint_fast32_t highwatermark_value;
        uint_fast32_t freelist_value;
    } ThreadAllocatorState;
    PERTHREAD static ThreadAllocatorState allocatorstate;
    #define myid allocatorstate.id
    #define next nxt[myid]
    #define contents cnts[myid]
    #define highwatermark allocatorstate.highwatermark_value
    #define freelist allocatorstate.freelist_value
#endif

uint_fast32_t *nxt[MAXTHREADALLOC];
uint_fast32_t *cnts[MAXTHREADALLOC];
// An expression pointer identifies its tail cell; next[tail] identifies its
// head cell, making the expression a circular ring. If contents < FREEMIN,
// it is a character. An untagged larger value identifies the tail of an owned
// subexpression. A value with MEMO_BIT set identifies a memo cell: the memo
// cell's contents points to the shared subexpression tail and its next holds
// the reference count (plus a transient evaluator busy flag).
#define FREEMIN 256
#define MEMO_BIT (UINT_FAST32_MAX - (UINT_FAST32_MAX >> 1))
#define MEMO_MASK (MEMO_BIT - 1)
_Static_assert((MEMO_BIT & (MEMO_BIT - 1)) == 0,
               "MEMO_BIT must be the highest value bit");
_Static_assert((uint_fast32_t)MAXARRAY < MEMO_BIT,
               "arena indices must fit below MEMO_BIT");
typedef uint64_t PackedKey;

typedef struct {
#if HAS_INFINITE_H
    const PackedKey *keys;
#else
    PackedKey *keys;
#endif
    size_t capacity;
    size_t size;
} PackedKeySet;

static PackedKeySet neverendingset;
#if !HAS_INFINITE_H
static void loadinfinitecatalog(FILE *file);
static void destroypackedkeyset(PackedKeySet *set);
#endif

_Atomic(uint_fast32_t) repeatcount = 0;
_Atomic(uint_fast32_t) nevercount = 0;
_Atomic(uint_fast32_t) totalinfinitecount = 0;
_Atomic(uint_fast32_t) neverendsmatch = 0;
_Atomic(uint_fast32_t) checked = 0;
atomic_int initdone = 0;

#if !SINGLE_THREAD
#ifndef WORK_BATCH_CAPACITY
#define WORK_BATCH_CAPACITY 32U
#endif
_Static_assert(WORK_BATCH_CAPACITY > 0U,
               "worker batch capacity must be positive");

typedef struct {
    unsigned length;
    uint_fast32_t num;
    unsigned size;
    char buffer[MAXBUF + 1];
    int index[MAXLEN];
    unsigned counts[WORK_BATCH_CAPACITY];
} WorkBatch;

pthread_t thread[MAXTHREADALLOC];
sem_t *threadsem[MAXTHREADALLOC];
sem_t *mastersem;
_Atomic(uint64_t) threadempty = 0;
_Atomic(uint64_t) threadwaiting = 0;
atomic_int exiting = 0;
atomic_int masterwaiting = 0;
WorkBatch workbatch[MAXTHREADALLOC];
char threadname[MAXTHREADALLOC][32];

static void setmaxthreads(void) {
    uint64_t cpucount;

#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO systeminfo;

    GetSystemInfo(&systeminfo);
    cpucount = (uint64_t)systeminfo.dwNumberOfProcessors;
    if (cpucount == 0) {
        fprintf(stderr, "GetSystemInfo reported no processors\n");
        exit(EXIT_FAILURE);
    }
#else
    errno = 0;
    long detectedcpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (detectedcpus < 1) {
        int error = errno ? errno : EIO;

        fprintf(stderr, "sysconf(_SC_NPROCESSORS_ONLN) failed: %s\n",
                strerror(error));
        exit(EXIT_FAILURE);
    }
    cpucount = (uint64_t)detectedcpus;
#endif

    uint64_t workercount = (cpucount > 1U) ? (cpucount - 1U) : 1U;

    if (workercount > MAXTHREADALLOC) workercount = MAXTHREADALLOC;
    maxthreads = (int)workercount;
    allthreadsmask = UINT64_MAX >> (64U - (unsigned)maxthreads);
    atomic_store(&threadempty, allthreadsmask);
}
#endif

#if !HAS_INFINITE_H
static char *getexecutablepath(const char *argv0)
{
    (void)argv0;

#if defined(__APPLE__)
    uint32_t capacity = 1024;
    char *path = malloc(capacity);

    if (path == NULL) {
        fprintf(stderr, "failed to allocate executable path\n");
        exit(EXIT_FAILURE);
    }
    for (;;) {
        uint32_t required = capacity;

        if (_NSGetExecutablePath(path, &required) == 0) break;
        char *larger = realloc(path, required);

        if (larger == NULL) {
            free(path);
            fprintf(stderr, "failed to allocate executable path\n");
            exit(EXIT_FAILURE);
        }
        path = larger;
        capacity = required;
    }
    char *resolved = realpath(path, NULL);

    if (resolved == NULL) {
        fprintf(stderr, "failed to resolve executable path %s: %s\n",
                path, strerror(errno));
        free(path);
        exit(EXIT_FAILURE);
    }
    free(path);
    return resolved;
#elif defined(_WIN32) || defined(_WIN64)
    DWORD capacity = 256;

    for (;;) {
        char *path = malloc((size_t)capacity);

        if (path == NULL) {
            fprintf(stderr, "failed to allocate executable path\n");
            exit(EXIT_FAILURE);
        }
        DWORD length = GetModuleFileNameA(NULL, path, capacity);

        if (length == 0) {
            DWORD error = GetLastError();

            free(path);
            fprintf(stderr,
                    "GetModuleFileNameA failed with error %lu\n",
                    (unsigned long)error);
            exit(EXIT_FAILURE);
        }
        if (length < capacity) return path;
        free(path);
        if (capacity > (UINT32_MAX / 2U)) {
            fprintf(stderr, "executable path is too long\n");
            exit(EXIT_FAILURE);
        }
        capacity *= 2U;
    }
#elif defined(__linux__)
    size_t capacity = 256;

    for (;;) {
        char *path = malloc(capacity + 1U);

        if (path == NULL) {
            fprintf(stderr, "failed to allocate executable path\n");
            exit(EXIT_FAILURE);
        }
        ssize_t length = readlink("/proc/self/exe", path, capacity);

        if (length < 0) {
            fprintf(stderr, "failed to read executable path: %s\n",
                    strerror(errno));
            free(path);
            exit(EXIT_FAILURE);
        }
        if ((size_t)length < capacity) {
            path[length] = '\0';
            return path;
        }
        free(path);
        if (capacity > (SIZE_MAX / 2U)) {
            fprintf(stderr, "executable path is too long\n");
            exit(EXIT_FAILURE);
        }
        capacity *= 2U;
    }
#else
    if ((argv0 == NULL) || (*argv0 == '\0')) {
        fprintf(stderr, "cannot determine executable path\n");
        exit(EXIT_FAILURE);
    }
    char *path = realpath(argv0, NULL);

    if (path == NULL) {
        fprintf(stderr, "failed to resolve executable path %s: %s\n",
                argv0, strerror(errno));
        exit(EXIT_FAILURE);
    }
    return path;
#endif
}

static void openinfinitefile(const char *argv0)
{
    static const char filename[] = "infinite.cmb";
    char *executable = getexecutablepath(argv0);
    char *separator = strrchr(executable, '/');

#if defined(_WIN32) || defined(_WIN64)
    char *backslash = strrchr(executable, '\\');

    if ((backslash != NULL) &&
        ((separator == NULL) || (backslash > separator))) {
        separator = backslash;
    }
#endif
    size_t directorylength = (separator == NULL)
        ? 0U
        : (size_t)(separator - executable) + 1U;

    if (directorylength > (SIZE_MAX - sizeof(filename))) {
        free(executable);
        fprintf(stderr, "infinite.cmb path is too long\n");
        exit(EXIT_FAILURE);
    }
    infinitepath = malloc(directorylength + sizeof(filename));
    if (infinitepath == NULL) {
        free(executable);
        fprintf(stderr, "failed to allocate infinite.cmb path\n");
        exit(EXIT_FAILURE);
    }
    memcpy(infinitepath, executable, directorylength);
    memcpy(infinitepath + directorylength, filename, sizeof(filename));
    free(executable);

    // An existing catalogue is an immutable cache for this run. Only a
    // missing catalogue enables output, so cached data is never truncated.
    for (int attempt = 0; attempt < 2; ++attempt) {
        errno = 0;
        FILE *input = fopen(infinitepath, "r");

        if (input != NULL) {
            loadinfinitecatalog(input);
            errno = 0;
            if (fclose(input) != 0) {
                int error = errno ? errno : EIO;

                fprintf(stderr, "failed to close %s after reading: %s\n",
                        infinitepath, strerror(error));
                exit(EXIT_FAILURE);
            }
            return;
        }
        int error = errno;

        if (error != ENOENT) {
            fprintf(stderr, "failed to open %s for reading: %s\n",
                    infinitepath, strerror(error));
            exit(EXIT_FAILURE);
        }

        errno = 0;
        infinitefile = fopen(infinitepath, "wx");
        if (infinitefile != NULL) return;
        error = errno;
        if ((error == EEXIST) && (attempt == 0)) continue;

        fprintf(stderr, "failed to create %s: %s\n",
                infinitepath, strerror(error ? error : EIO));
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "failed to initialize %s\n", infinitepath);
    exit(EXIT_FAILURE);
}

static int closeinfinitefile(void)
{
    int result = EXIT_SUCCESS;

    if (infinitefile != NULL) {
        errno = 0;
        if (fclose(infinitefile) != 0) {
            int error = errno ? errno : EIO;

            fprintf(stderr, "failed to close %s: %s\n",
                    infinitepath, strerror(error));
            result = EXIT_FAILURE;
        }
        infinitefile = NULL;
    }
    destroypackedkeyset(&neverendingset);
    free(infinitepath);
    infinitepath = NULL;
    return result;
}
#endif

// things that can return 0:
// next[]
// getfree
// str2cell
// clonecells
// str2cells
// clonecontents

void setupfreelist(void) {
    highwatermark = FREEMIN;
    freelist = 0;
}

// The high-water mark grows only when the freelist is empty, at which point
// every earlier arena cell is live. It therefore also records peak live use.
static inline uint_fast32_t getfree(void) {
    uint_fast32_t temp = freelist;

    if (temp) {
        freelist = next[temp];
    } else {
#if PARANOID
        if (highwatermark >= maxstr) {
            return 0;
        }
#endif
        temp = highwatermark++;
    }

    return temp;
}

static inline void putfree(uint_fast32_t cell) {
    next[cell] = freelist;
    freelist = cell;
}

static inline int ismemocontents(uint_fast32_t value) {
    return (value & MEMO_BIT) != 0;
}

static inline int isdirectcontents(uint_fast32_t value) {
    return (value >= FREEMIN) && !ismemocontents(value);
}

static inline uint_fast32_t memocell(uint_fast32_t value) {
    return value & MEMO_MASK;
}

static inline uint_fast32_t resolvedtail(uint_fast32_t value) {
    return ismemocontents(value) ? contents[memocell(value)] : value;
}

static inline uint_fast32_t memoreferences(uint_fast32_t memo) {
    return next[memo] & MEMO_MASK;
}

void memoacquire(uint_fast32_t value) {
    uint_fast32_t memo = memocell(value);
    uint_fast32_t state = next[memo];

#if PARANOID
    if (!ismemocontents(value) || (memo < FREEMIN) ||
        (memo >= highwatermark) || ((state & MEMO_MASK) == 0) ||
        ((state & MEMO_MASK) == MEMO_MASK)) {
        printf("*** Programmer error: invalid memo acquire at %" PRIuFAST32 "\n",
               memo);
        INT3
        return;
    }
#endif
    next[memo] = state + 1;
}

void freeall(uint_fast32_t cells);

void releasecontents(uint_fast32_t value) {
    if (value < FREEMIN) return;
    if (isdirectcontents(value)) {
        freeall(value);
        return;
    }

    uint_fast32_t memo = memocell(value);
    uint_fast32_t state = next[memo];
    uint_fast32_t references = state & MEMO_MASK;

#if PARANOID
    if ((memo < FREEMIN) || (memo >= highwatermark) || (references == 0)) {
        printf("*** Programmer error: invalid memo release at %" PRIuFAST32 "\n",
               memo);
        INT3
        return;
    }
#endif
    if (references > 1) {
        next[memo] = state - 1;
        return;
    }
#if PARANOID
    if ((state & MEMO_BIT) != 0) {
        printf("*** Programmer error: releasing busy memo at %" PRIuFAST32 "\n",
               memo);
        INT3
        return;
    }
#endif
    uint_fast32_t target = contents[memo];

    freeall(target);
    putfree(memo);
}

void freeall(uint_fast32_t cells) {
    uint_fast32_t tail = cells;
    uint_fast32_t head;
    
    if (cells < FREEMIN) {
        printf("*** Programmer error: freeall called for non-list %" PRIuFAST32 "\n", cells);
        INT3
        return;
    }
    head = next[tail];
    cells = head;
#if PARANOID
    if (cells == 0) {
        printf("*** Programmer error: unexpected end of cells during freeall\n");
        INT3
        return;
    }
#endif
    for (;;) {
        uint_fast32_t temp = next[cells];

        releasecontents(contents[cells]);
        putfree(cells);
        if (cells == tail) {
            return;
        }
        cells = temp;
#if PARANOID
        if (cells == 0) {
            printf("*** Programmer error: unexpected end of cells during freeall\n");
            INT3
            return;
        }
#endif
    }
}

uint_fast32_t startnum(unsigned length) {
    uint_fast32_t result = 0;
    
    for (unsigned i = 0; i < length; ++i) {
        result = (4 * result) + 1;
    }
    return result << 2;
}

uint_fast32_t endnum(unsigned length) {
    uint_fast32_t result = 0;
    
    for (unsigned i = 0; i < length; ++i) {
        result = (2 * result) + 1;
    }
    return result << (length + 1);
}

int earlyend(uint_fast32_t num, unsigned length) {
    int ones = 0;
    int zeros = 0;
    
    for (unsigned i = (2 * length); i > 0; --i) {
        if ((num >> i) & 1) {
            ++ones;
        } else {
            ++zeros;
        }
        if ((ones + 1) == zeros) {
            return 1;
        }
    }
    return 0;
}

#if 0
void num2binary(int num) {
    int numlen = 8 * sizeof(num);
    int first = 1;

    for (int i = numlen - 1; i >= 0; --i) {
        if (num & (1 << i)) {
            strncat(buffer, "1", MAXBUF);
            first = 0;
        // suppress leaing zeroes
        } else if (first == 0) {
            strncat(buffer, "0", MAXBUF);
        }
    }
    // make sure there's at least one digit
    if (first) {
        strncat(buffer, "0", MAXBUF);
    }
}

int num2fps(int num, int position) {
    if (position < 0) {
        return position;
    }
    if (num & (1 << position--)) {
        strncat(buffer, "(", MAXBUF);
        position = num2fps(num, position);
        position = num2fps(num, position);
        strncat(buffer, ")", MAXBUF);
    } else {
        strncat(buffer, "?", MAXBUF);
    }
    return position;
}
#endif

int_fast32_t num2str(uint_fast32_t num, int_fast32_t position, char *buffer, uint_fast32_t first) {
    if (position < 0) {
        return position;
    }
    if (num & (1 << position--)) {
        uint_fast32_t dontparen = first + (num & (1 << (position + 2)));

        if (dontparen == 0) {
            strncat(buffer, "(", MAXBUF);
        }
        position = num2str(num, position, buffer, 0);
        position = num2str(num, position, buffer, 0);
        if (dontparen == 0) {
            strncat(buffer, ")", MAXBUF);
        }
    } else {
        strncat(buffer, "?", MAXBUF);
    }
    return position;
}

void prepareSKbuffer(unsigned length, uint_fast32_t num, char *buffer,
                     int *index) {
    int indexnum = (int)length;

    buffer[0] = '\0';
    (void)num2str(num, (int_fast32_t)(2 * length), buffer, 1);
    for (int position = 0; buffer[position] != '\0'; ++position) {
        if (buffer[position] == '?') {
            index[indexnum--] = position;
        }
    }
#if PARANOID
    if ((indexnum != -1) || (index[length] != 0)) {
        fprintf(stderr, "*** Programmer error: malformed numeric work buffer\n");
        INT3
        exit(EXIT_FAILURE);
    }
#endif
    buffer[0] = 'S';
    strncat(buffer, "x", MAXBUF);
}

void setSKbuffer(unsigned length, unsigned count, char *buffer,
                 const int *index) {
    for (unsigned i = 0; i < length; ++i) {
        buffer[index[i]] = (count & (1U << i)) ? 'K' : 'S';
    }
}

uint_fast32_t str2cell(int nested, char *buffer, uint_fast32_t *str2cellspos) {
    uint_fast32_t head = 0;
    uint_fast32_t tail = 0;

#if !PARANOID
    (void)nested;
#endif
    for (;;) {
        uint_fast32_t temp;
        uint_fast32_t temp2;
        unsigned char curchar = (unsigned char)buffer[(*str2cellspos)++];
        
        if (curchar == '\0') {
#if PARANOID
            if (head == 0) {
                printf("*** Programmer error: unexpected buffer end at %" PRIuFAST32 "\n", --*str2cellspos);
                INT3
                return 0;
            }
#endif
            next[tail] = head;
            return tail;
        }
        if (curchar == ')') {
#if PARANOID
            if ((head == 0) || (nested == 0)) {
                printf("*** Programmer error: unexpected right paren at %" PRIuFAST32 "\n", --*str2cellspos);
                INT3
                return 0;
            }
#endif
            next[tail] = head;
            return tail;
        }
        if (curchar == '(') {
#if PARANOID
            if (head == 0) {
                printf("*** Programmer error: unexpected left paren at %" PRIuFAST32 "\n", --*str2cellspos);
                INT3
                return 0;
            }
#endif
            temp2 = str2cell(1, buffer, str2cellspos);
            if (temp2 == 0) {
                return 0;
            }
            temp = getfree();
            if (temp == 0) {
                return 0;
            }
            contents[temp] = temp2;
            next[temp] = head;
#if PARANOID
            if (head == 0) {
                head = temp;
            }
            if (tail) {
                next[tail] = temp;
            }
#else
            next[tail] = temp;
#endif
            tail = temp;
        } else {
            temp = getfree();
            if (temp == 0) {
                return 0;
            }
            contents[temp] = curchar;
            next[temp] = 0;
            if (head == 0) {
                head = temp;
            }
            if (tail) {
                next[tail] = temp;
            }
            tail = temp;
        }
    }
}

#if DOTESTS
static inline uint_fast32_t str2cells(char *buffer) {
    uint_fast32_t str2cellspos = 0;

    return str2cell(0, buffer, &str2cellspos);
}
#endif

void printcells(uint_fast32_t startcell) {
    uint_fast32_t tail = startcell;
    uint_fast32_t cells = next[tail];
    int first = 1;
    
#if PARANOID
    if (cells == 0) {
        printf("*** Programmer error: unexpected end of cells during print at %" PRIuFAST32 "\n", startcell);
        INT3
        return;
    }
#endif
    for (;;) {
        uint_fast32_t curconts = contents[cells];

        if (curconts < FREEMIN) {
            putchar((unsigned char)curconts);
        } else if (first) {
            printcells(resolvedtail(curconts));
        } else {
            putchar('(');
            printcells(resolvedtail(curconts));
            putchar(')');
        }
        if (cells == tail) return;
        cells = next[cells];
        first = 0;
#if PARANOID
        if (cells == 0) {
            printf("*** Programmer error: unexpected end of cells during print at %" PRIuFAST32 "\n", startcell);
            INT3
            return;
        }
#endif
    }
}

static inline void putcells(uint_fast32_t cells) {
    printcells(cells);
    putchar('\n');
}

#if 0
static inline void putcontents(uint_fast32_t conts) {
    if (conts < FREEMIN) {
        putchar((unsigned char)conts);
        putchar('\n');
        return;
    }
    putcells(resolvedtail(conts));
}
#endif

// cells2str unused
# if 0
char strbuf[MAXARRAY + 1];
int cells2strpos = 0;

void cell2str(uint_fast32_t startcell) {
    uint_fast32_t tail = startcell;
    uint_fast32_t cells = next[tail];
    int first = 1;
    
#if PARANOID
    if (cells == 0) {
        printf("*** Programmer error: unexpected end of cells during conversion at %" PRIuFAST32 "\n", startcell);
        INT3
        return;
    }
#endif
    for (;;) {
        uint_fast32_t curconts = contents[cells];

        if (curconts < FREEMIN) {
            strbuf[cells2strpos++] = (unsigned char)curconts;
        } else if (first) {
            cell2str(resolvedtail(curconts));
        } else {
            strbuf[cells2strpos++] = '(';
            cell2str(resolvedtail(curconts));
            strbuf[cells2strpos++] = ')';
        }
        if (cells == tail) return;
        cells = next[cells];
        first = 0;
#if PARANOID
        if (cells == 0) {
            printf("*** Programmer error: unexpected end of cells during conversion at %" PRIuFAST32 "\n", startcell);
            INT3
            return;
        }
#endif
    }
}

static inline void cells2str(uint_fast32_t cells) {
    cells2strpos = 0;
    cell2str(cells);
    strbuf[cells2strpos] = '\0';
}
#endif

uint_fast32_t clonecontents(uint_fast32_t conts);

uint_fast32_t clonecells(uint_fast32_t startcell) {
    uint_fast32_t tail = startcell;
    uint_fast32_t cells = next[tail];
    uint_fast32_t newhead = 0;
    uint_fast32_t newtail = 0;
    
#if PARANOID
    if (cells == 0) {
        printf("*** Programmer error: unexpected end of cells during clone at %" PRIuFAST32 "\n", startcell);
        INT3
        return 0;
    }
#endif
    for (;;) {
        uint_fast32_t temp = getfree();
        uint_fast32_t curconts;

        if (temp == 0) {
            return 0;
        }
        curconts = clonecontents(contents[cells]);
        if (curconts == 0) {
            putfree(temp);
            return 0;
        }
        contents[temp] = curconts;
        if (newhead == 0) {
            newhead = temp;
        } else {
            next[newtail] = temp;
        }
        newtail = temp;
        if (cells == tail) {
            next[newtail] = newhead;
            return newtail;
        }
        cells = next[cells];
#if PARANOID
        if (cells == 0) {
            printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", startcell);
            INT3
            return 0;
        }
#endif
    }
}

uint_fast32_t clonecontents(uint_fast32_t conts) {
    if (conts < FREEMIN) {
        return conts;
    }
    if (ismemocontents(conts)) {
        memoacquire(conts);
        return conts;
    }
    return clonecells(conts);
}

int equalcells(uint_fast32_t startcells1, uint_fast32_t startcells2, int toplevel);

static int equalcontents(uint_fast32_t value1, uint_fast32_t value2) {
    if ((value1 < FREEMIN) || (value2 < FREEMIN)) {
        return value1 == value2;
    }
    if (ismemocontents(value1) && (value1 == value2)) return 1;
    return equalcells(resolvedtail(value1), resolvedtail(value2), 0) != 0;
}

typedef struct ExpressionCursor {
    uint_fast32_t cell;
    uint_fast32_t tail;
    int athead;
    const struct ExpressionCursor *continuation;
} ExpressionCursor;

// Reduction may move a direct subexpression or memo tag into the first cell of
// a ring. Canonical observers flatten that leading span, so it denotes the same
// left-associated application as the former character-headed representation.

typedef struct {
    uint_fast32_t memo;
    size_t memberslot;
    uint_fast32_t occurrence;
    uint_fast32_t parenttail;
    size_t previouscontinuation;
    size_t activecontinuation;
    ExpressionCursor continuation;
} MemoPathFrame;

typedef struct {
    uint_fast32_t memo;
    uint32_t generation;
} MemoPathMember;

typedef struct {
    MemoPathFrame *frames;
    size_t depth;
    size_t capacity;
    size_t activecontinuation;
    MemoPathMember *members;
    size_t membercapacity;
    size_t membercount;
    size_t tombstones;
    uint32_t generation;
} MemoPath;

PERTHREAD static MemoPath evaluatorpath;

static void freememopath(void) {
    free(evaluatorpath.frames);
    free(evaluatorpath.members);
    evaluatorpath.frames = NULL;
    evaluatorpath.members = NULL;
    evaluatorpath.depth = 0;
    evaluatorpath.capacity = 0;
    evaluatorpath.activecontinuation = 0;
    evaluatorpath.membercapacity = 0;
    evaluatorpath.membercount = 0;
    evaluatorpath.tombstones = 0;
    evaluatorpath.generation = 0;
}

static size_t memopathhash(uint_fast32_t memo, size_t capacity) {
    uint64_t mixed = (uint64_t)memo * UINT64_C(11400714819323198485);

    mixed ^= mixed >> 32;
    return (size_t)mixed & (capacity - 1);
}

static void rehashmemopath(MemoPath *path, size_t newcapacity) {
    MemoPathMember *newmembers = calloc(newcapacity, sizeof(*newmembers));

    if (newmembers == NULL) {
        fprintf(stderr,
                "failed to grow memo update membership table to %zu entries\n",
                newcapacity);
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < path->depth; ++i) {
        MemoPathFrame *frame = &path->frames[i];
        size_t index = memopathhash(frame->memo, newcapacity);

        while (newmembers[index].memo != 0) {
            index = (index + 1) & (newcapacity - 1);
        }
        newmembers[index].memo = frame->memo;
        newmembers[index].generation = path->generation;
        frame->memberslot = index;
    }
    free(path->members);
    path->members = newmembers;
    path->membercapacity = newcapacity;
    path->tombstones = 0;
}

static int memoinpath(const MemoPath *path, uint_fast32_t memo) {
    if (path->membercapacity == 0) return 0;

    size_t index = memopathhash(memo, path->membercapacity);

    for (;;) {
        MemoPathMember member = path->members[index];

        if ((member.generation != path->generation) || (member.memo == 0)) {
            return 0;
        }
        if (member.memo == memo) return 1;
        index = (index + 1) & (path->membercapacity - 1);
    }
}

static size_t insertmemopathmember(MemoPath *path, uint_fast32_t memo) {
    if ((path->membercapacity == 0) ||
        ((path->membercount + path->tombstones + 1) >=
         (path->membercapacity / 2))) {
        size_t newcapacity;

        if (path->membercapacity == 0) {
            newcapacity = 128;
        } else if ((path->membercount + 1) <
                   (path->membercapacity / 4)) {
            newcapacity = path->membercapacity;
        } else {
            newcapacity = path->membercapacity * 2;
        }

        if ((newcapacity < path->membercapacity) ||
            (newcapacity > (SIZE_MAX / sizeof(*path->members)))) {
            fprintf(stderr, "memo update membership table is too large\n");
            exit(EXIT_FAILURE);
        }
        rehashmemopath(path, newcapacity);
    }

    size_t index = memopathhash(memo, path->membercapacity);
    size_t tombstone = SIZE_MAX;

    for (;;) {
        MemoPathMember *member = &path->members[index];

        if (member->generation != path->generation) break;
        if (member->memo == 1) {
            if (tombstone == SIZE_MAX) tombstone = index;
        } else if (member->memo == 0) {
            break;
        }
        index = (index + 1) & (path->membercapacity - 1);
    }
    if (tombstone != SIZE_MAX) {
        index = tombstone;
        path->tombstones--;
    }
    path->members[index].memo = memo;
    path->members[index].generation = path->generation;
    path->membercount++;
    return index;
}

static void removememopathmember(MemoPath *path,
                                 const MemoPathFrame *frame) {
#if PARANOID
    if (frame->memberslot >= path->membercapacity) {
        printf("*** Programmer error: memo update path slot is invalid\n");
        INT3
        return;
    }
#endif
    MemoPathMember *member = &path->members[frame->memberslot];

#if PARANOID
    if ((member->generation != path->generation) ||
        (member->memo != frame->memo)) {
        printf("*** Programmer error: memo update path member is missing\n");
        INT3
        return;
    }
#endif
    member->memo = 1;
    path->membercount--;
    path->tombstones++;
}

static void clearmemopath(MemoPath *path) {
    path->depth = 0;
    path->activecontinuation = 0;
    path->membercount = 0;
    path->tombstones = 0;
    path->generation++;
    if (path->generation == 0) {
        if (path->membercapacity != 0) {
            memset(path->members, 0,
                   path->membercapacity * sizeof(*path->members));
        }
        path->generation = 1;
    }
}

static void truncatememopath(MemoPath *path, size_t newdepth) {
    while (path->depth > newdepth) {
        path->depth--;
        removememopathmember(path, &path->frames[path->depth]);
    }
    path->activecontinuation =
        newdepth ? path->frames[newdepth - 1].activecontinuation : 0;
}

static void pushmemopath(MemoPath *path, uint_fast32_t memo,
                         uint_fast32_t occurrence, uint_fast32_t parenttail) {
    if (path->depth == path->capacity) {
        size_t newcapacity = path->capacity ? (path->capacity * 2) : 64;

        if ((newcapacity < path->capacity) ||
            (newcapacity > (SIZE_MAX / sizeof(*path->frames)))) {
            fprintf(stderr, "memo update path is too large\n");
            exit(EXIT_FAILURE);
        }
        MemoPathFrame *newframes =
            realloc(path->frames, newcapacity * sizeof(*newframes));

        if (newframes == NULL) {
            fprintf(stderr,
                    "failed to grow memo update path to %zu frames\n",
                    newcapacity);
            exit(EXIT_FAILURE);
        }
        path->frames = newframes;
        path->capacity = newcapacity;
        for (size_t i = 0; i < path->depth; ++i) {
            size_t previous = path->frames[i].previouscontinuation;

            path->frames[i].continuation.continuation =
                previous ? &path->frames[previous - 1].continuation : NULL;
        }
    }
    MemoPathFrame *frame = &path->frames[path->depth];

    frame->memo = memo;
    frame->occurrence = occurrence;
    frame->parenttail = parenttail;
    frame->previouscontinuation = path->activecontinuation;
    if (occurrence != parenttail) {
        frame->continuation.cell = next[occurrence];
        frame->continuation.tail = parenttail;
        frame->continuation.athead = 0;
        frame->continuation.continuation = frame->previouscontinuation
            ? &path->frames[frame->previouscontinuation - 1].continuation
            : NULL;
        path->activecontinuation = path->depth + 1;
    }
    frame->activecontinuation = path->activecontinuation;
    frame->memberslot = insertmemopathmember(path, memo);
    path->depth++;
}

static void startcursor(ExpressionCursor *result, uint_fast32_t tail,
                        const ExpressionCursor *continuation) {
    result->cell = next[tail];
    result->tail = tail;
    result->athead = 1;
    result->continuation = continuation;
}

static void advancecursor(ExpressionCursor *cursor) {
    if (cursor->cell != cursor->tail) {
        cursor->cell = next[cursor->cell];
        cursor->athead = 0;
        return;
    }
    if (cursor->continuation != NULL) {
        *cursor = *cursor->continuation;
        return;
    }
    cursor->cell = 0;
    cursor->tail = 0;
    cursor->athead = 0;
    cursor->continuation = NULL;
}

static int equalcursors(ExpressionCursor first, ExpressionCursor second,
                        int toplevel) {
    for (;;) {
        if (first.athead && (contents[first.cell] >= FREEMIN)) {
            ExpressionCursor continuation = first;
            ExpressionCursor nested;

            advancecursor(&continuation);
            startcursor(&nested, resolvedtail(contents[first.cell]),
                        &continuation);

            return equalcursors(nested, second, toplevel);
        }
        if (second.athead && (contents[second.cell] >= FREEMIN)) {
            ExpressionCursor continuation = second;
            ExpressionCursor nested;

            advancecursor(&continuation);
            startcursor(&nested, resolvedtail(contents[second.cell]),
                        &continuation);

            return equalcursors(first, nested, toplevel);
        }

        uint_fast32_t value1 = contents[first.cell];
        uint_fast32_t value2 = contents[second.cell];
        ExpressionCursor nextfirst = first;
        ExpressionCursor nextsecond = second;

        advancecursor(&nextfirst);
        advancecursor(&nextsecond);

        if (!equalcontents(value1, value2)) {
            if (toplevel && (value1 == 'x') && (nextfirst.cell == 0)) {
                return 2;
            }
            return 0;
        }
        if (nextfirst.cell == 0) {
            if (nextsecond.cell == 0) return 1;
            return toplevel ? 2 : 0;
        }
        if (nextsecond.cell == 0) return 0;
        first = nextfirst;
        second = nextsecond;
    }
}

static int equalcellspath(uint_fast32_t startcells1, uint_fast32_t startcells2,
                          int toplevel, MemoPath *path) {
    if (path->depth == 0) {
        return equalcells(startcells1, startcells2, toplevel);
    }

#if PARANOID
    if (path->frames[0].parenttail != startcells2) {
        printf("*** Programmer error: memo path has wrong parent tail\n");
        INT3
        return 0;
    }
#endif

    const ExpressionCursor *continuation = path->activecontinuation
        ? &path->frames[path->activecontinuation - 1].continuation
        : NULL;

    ExpressionCursor first;
    ExpressionCursor second;

    startcursor(&first, startcells1, NULL);
    startcursor(&second, contents[path->frames[path->depth - 1].memo],
                continuation);

    return equalcursors(first, second, toplevel);
}

// startcells1 is always bufferhead if toplevel == 1
int equalcells(uint_fast32_t startcells1, uint_fast32_t startcells2, int toplevel) {
    ExpressionCursor first;
    ExpressionCursor second;

    startcursor(&first, startcells1, NULL);
    startcursor(&second, startcells2, NULL);

#if PARANOID
    if (first.cell == 0) {
        printf("*** Programmer error: unexpected end of first comparison at %" PRIuFAST32 "\n", startcells1);
        INT3
        return 0;
    }
    if (second.cell == 0) {
        printf("*** Programmer error: unexpected end of second comparison at %" PRIuFAST32 "\n", startcells2);
        INT3
        return 0;
    }
#endif
    if ((contents[first.cell] >= FREEMIN) ||
        (contents[second.cell] >= FREEMIN)) {
        return equalcursors(first, second, toplevel);
    }

    uint_fast32_t cell1 = first.cell;
    uint_fast32_t cell2 = second.cell;

    for (;;) {
        uint_fast32_t value1 = contents[cell1];
        uint_fast32_t value2 = contents[cell2];

        if (!equalcontents(value1, value2)) {
            if (toplevel && (value1 == 'x') && (cell1 == startcells1)) {
                return 2;
            }
            return 0;
        }
        if (cell1 == startcells1) {
            if (cell2 == startcells2) return 1;
            return toplevel ? 2 : 0;
        }
        if (cell2 == startcells2) return 0;
        cell1 = next[cell1];
        cell2 = next[cell2];
    }
}

// A packed key is the exact two-bit preorder grammar in its low 58 bits,
// with the payload width in the high six bits. Application is token zero, so
// storing the width is what preserves leading application tokens.
#define PACKED_KEY_LENGTH_BITS 6U
#define PACKED_KEY_LENGTH_SHIFT (64U - PACKED_KEY_LENGTH_BITS)
#define PACKED_KEY_PAYLOAD_BITS PACKED_KEY_LENGTH_SHIFT
#define PACKED_KEY_PAYLOAD_MASK (UINT64_MAX >> PACKED_KEY_LENGTH_BITS)
#define PACKED_KEY_TOKEN_BITS 2U
#define PACKED_TOKEN_APPLICATION UINT64_C(0)
#define PACKED_TOKEN_S UINT64_C(1)
#define PACKED_TOKEN_K UINT64_C(2)
#define PACKED_TOKEN_X UINT64_C(3)
#define PACKED_LEAF_KEY(token) \
    ((((PackedKey)PACKED_KEY_TOKEN_BITS) << PACKED_KEY_LENGTH_SHIFT) | \
     (PackedKey)(token))
#define PACKED_KEY_S PACKED_LEAF_KEY(PACKED_TOKEN_S)
#define PACKED_KEY_K PACKED_LEAF_KEY(PACKED_TOKEN_K)
#define PACKED_KEY_X PACKED_LEAF_KEY(PACKED_TOKEN_X)

_Static_assert(PACKED_TOKEN_APPLICATION == 0,
               "packed application token must be zero");
_Static_assert(((4U * MAXLEN) - 2U) <= PACKED_KEY_PAYLOAD_BITS,
               "MAXLEN expressions must fit in packed keys");

static inline unsigned packedkeybits(PackedKey key) {
    return (unsigned)(key >> PACKED_KEY_LENGTH_SHIFT);
}

#if PARANOID
static int validpackedkey(PackedKey key) {
    unsigned bits = packedkeybits(key);
    PackedKey payload = key & PACKED_KEY_PAYLOAD_MASK;

    if ((bits < PACKED_KEY_TOKEN_BITS) ||
        (bits > PACKED_KEY_PAYLOAD_BITS) ||
        ((bits % PACKED_KEY_TOKEN_BITS) != 0)) {
        return 0;
    }
    return (bits == PACKED_KEY_PAYLOAD_BITS) || ((payload >> bits) == 0);
}
#endif

static inline PackedKey packedapplicationkey(PackedKey left,
                                              PackedKey right) {
    unsigned leftbits = packedkeybits(left);
    unsigned rightbits = packedkeybits(right);
    unsigned resultbits = PACKED_KEY_TOKEN_BITS + leftbits + rightbits;

#if PARANOID
    if (!validpackedkey(left) || !validpackedkey(right)) {
        fprintf(stderr, "invalid packed combinator key\n");
        INT3
        exit(EXIT_FAILURE);
    }
#endif
    if (resultbits > PACKED_KEY_PAYLOAD_BITS) {
        fprintf(stderr,
                "combinator exceeds packed-key capacity of %u bits\n",
                PACKED_KEY_PAYLOAD_BITS);
        exit(EXIT_FAILURE);
    }
    PackedKey payload =
        ((left & PACKED_KEY_PAYLOAD_MASK) << rightbits) |
        (right & PACKED_KEY_PAYLOAD_MASK);

    return ((PackedKey)resultbits << PACKED_KEY_LENGTH_SHIFT) | payload;
}

static inline size_t packedkeyhash(PackedKey key, size_t mask) {
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    key *= UINT64_C(0xc4ceb9fe1a85ec53);
    key ^= key >> 33;
    return (size_t)key & mask;
}

#if !HAS_INFINITE_H
#define INITIAL_PACKED_KEY_CAPACITY 1048576U
_Static_assert((INITIAL_PACKED_KEY_CAPACITY &
                (INITIAL_PACKED_KEY_CAPACITY - 1U)) == 0,
               "initial packed-key capacity must be a power of two");

static void initializepackedkeyset(PackedKeySet *set) {
    set->keys = calloc(INITIAL_PACKED_KEY_CAPACITY,
                       sizeof(*set->keys));
    if (set->keys == NULL) {
        fprintf(stderr, "failed to allocate packed-key set for %s\n",
                infinitepath);
        exit(EXIT_FAILURE);
    }
    set->capacity = INITIAL_PACKED_KEY_CAPACITY;
    set->size = 0;
}

static void growpackedkeyset(PackedKeySet *set) {
    if ((set->capacity > (SIZE_MAX / 2U)) ||
        ((set->capacity * 2U) > (SIZE_MAX / sizeof(*set->keys)))) {
        fprintf(stderr, "packed-key set for %s is too large\n", infinitepath);
        exit(EXIT_FAILURE);
    }
    size_t newcapacity = set->capacity * 2U;
    PackedKey *newkeys = calloc(newcapacity, sizeof(*newkeys));

    if (newkeys == NULL) {
        fprintf(stderr, "failed to grow packed-key set for %s\n",
                infinitepath);
        exit(EXIT_FAILURE);
    }
    size_t mask = newcapacity - 1U;

    for (size_t oldindex = 0; oldindex < set->capacity; ++oldindex) {
        PackedKey key = set->keys[oldindex];

        if (key == 0) continue;
        size_t newindex = packedkeyhash(key, mask);
        while (newkeys[newindex] != 0) {
            newindex = (newindex + 1U) & mask;
        }
        newkeys[newindex] = key;
    }
    free(set->keys);
    set->keys = newkeys;
    set->capacity = newcapacity;
}

static int packedkeysetinsert(PackedKeySet *set, PackedKey key) {
    if (key == 0) {
        fprintf(stderr, "cannot insert an empty packed key from %s\n",
                infinitepath);
        exit(EXIT_FAILURE);
    }
    size_t mask = set->capacity - 1U;
    size_t index = packedkeyhash(key, mask);

    while (set->keys[index] != 0) {
        if (set->keys[index] == key) return 0;
        index = (index + 1U) & mask;
    }
    if (set->size >= (set->capacity / 2U)) {
        growpackedkeyset(set);
        return packedkeysetinsert(set, key);
    }
    set->keys[index] = key;
    set->size++;
    return 1;
}
#endif

static int packedkeysetcontains(const PackedKeySet *set, PackedKey key) {
    if ((set->keys == NULL) || (key == 0)) return 0;
    size_t mask = set->capacity - 1U;
    size_t index = packedkeyhash(key, mask);
    size_t start = index;

    while (set->keys[index] != 0) {
        if (set->keys[index] == key) return 1;
        index = (index + 1U) & mask;
        if (index == start) break;
    }
    return 0;
}

#if !HAS_INFINITE_H
static void destroypackedkeyset(PackedKeySet *set) {
    free(set->keys);
    set->keys = NULL;
    set->capacity = 0;
    set->size = 0;
}

_Noreturn static void invalidcatalogline(size_t linenumber,
                                         const char *message) {
    fprintf(stderr, "invalid %s line %zu: %s\n",
            infinitepath, linenumber, message);
    exit(EXIT_FAILURE);
}

static int hexvalue(unsigned char character) {
    if ((character >= '0') && (character <= '9')) {
        return (int)(character - '0');
    }
    if ((character >= 'a') && (character <= 'f')) {
        return (int)(character - 'a') + 10;
    }
    if ((character >= 'A') && (character <= 'F')) {
        return (int)(character - 'A') + 10;
    }
    return -1;
}

static PackedKey parsecatalogexpression(const char **position,
                                         size_t linenumber) {
    PackedKey result = 0;

    while ((**position != '\0') && (**position != ')')) {
        PackedKey term;

        if (**position == 'S') {
            term = PACKED_KEY_S;
            (*position)++;
        } else if (**position == 'K') {
            term = PACKED_KEY_K;
            (*position)++;
        } else if (**position == '(') {
            (*position)++;
            term = parsecatalogexpression(position, linenumber);
            if (**position != ')') {
                invalidcatalogline(linenumber, "missing closing parenthesis");
            }
            (*position)++;
        } else {
            invalidcatalogline(linenumber,
                               "expression contains an invalid character");
        }

        if (result == 0) {
            result = term;
        } else {
            unsigned resultbits = PACKED_KEY_TOKEN_BITS +
                packedkeybits(result) + packedkeybits(term);

            if (resultbits > PACKED_KEY_PAYLOAD_BITS) {
                invalidcatalogline(linenumber,
                                   "expression exceeds packed-key capacity");
            }
            result = packedapplicationkey(result, term);
        }
    }
    if (result == 0) {
        invalidcatalogline(linenumber, "expression is empty");
    }
    return result;
}

#define INFINITE_CATALOG_LINE_CAPACITY 128U

static void loadinfinitecatalog(FILE *file) {
    char line[INFINITE_CATALOG_LINE_CAPACITY];
    size_t linenumber = 0;

    initializepackedkeyset(&neverendingset);
    for (;;) {
        errno = 0;
        if (fgets(line, (int)sizeof(line), file) == NULL) {
            if (ferror(file)) {
                int error = errno ? errno : EIO;

                fprintf(stderr, "failed to read %s after line %zu: %s\n",
                        infinitepath, linenumber, strerror(error));
                exit(EXIT_FAILURE);
            }
            break;
        }
        linenumber++;
        size_t length = strlen(line);

        if ((length == 0) || (line[length - 1U] != '\n')) {
            invalidcatalogline(linenumber,
                               "line is overlong or not newline-terminated");
        }
        line[--length] = '\0';
        if ((length != 0) && (line[length - 1U] == '\r')) {
            line[--length] = '\0';
        }
        if ((length < 21U) || (line[0] != '0') || (line[1] != 'x') ||
            (line[18] != ':') || (line[19] != ' ')) {
            invalidcatalogline(linenumber,
                               "expected 0x<16 hex digits>: <SK expression>");
        }
        PackedKey recordedkey = 0;

        for (size_t i = 2; i < 18; ++i) {
            int digit = hexvalue((unsigned char)line[i]);

            if (digit < 0) {
                invalidcatalogline(linenumber,
                                   "packed key contains a non-hex digit");
            }
            recordedkey = (recordedkey << 4) | (PackedKey)(unsigned)digit;
        }
        const char *position = line + 20;
        PackedKey expressionkey = parsecatalogexpression(&position,
                                                          linenumber);

        if (*position != '\0') {
            invalidcatalogline(linenumber,
                               "expression has an unmatched parenthesis");
        }
        if (recordedkey != expressionkey) {
            invalidcatalogline(linenumber,
                               "packed key does not match expression");
        }
        (void)packedkeysetinsert(&neverendingset, recordedkey);
    }
}
#else
_Static_assert(INFINITE_KEY_CAPACITY != 0U &&
               (INFINITE_KEY_CAPACITY & (INFINITE_KEY_CAPACITY - 1U)) == 0U,
               "embedded infinite-key capacity must be a power of two");
_Static_assert(INFINITE_KEY_COUNT <= (INFINITE_KEY_CAPACITY / 2U),
               "embedded infinite-key table must be at most half full");

static void readinfiniteh(void) {
    infinitepath = "infinite.h";
    neverendingset.keys = infinite_keys;
    neverendingset.capacity = INFINITE_KEY_CAPACITY;
    neverendingset.size = INFINITE_KEY_COUNT;
}
#endif

static PackedKey cellcontentpackedkey(uint_fast32_t value);

static PackedKey cells2packedkey(uint_fast32_t tail) {
    uint_fast32_t cell = next[tail];
    PackedKey key = cellcontentpackedkey(contents[cell]);

    while (cell != tail) {
        cell = next[cell];
        key = packedapplicationkey(key,
                                   cellcontentpackedkey(contents[cell]));
    }
    return key;
}

static PackedKey cellcontentpackedkey(uint_fast32_t value) {
    if (value >= FREEMIN) return cells2packedkey(resolvedtail(value));
    if (value == 'S') return PACKED_KEY_S;
    if (value == 'K') return PACKED_KEY_K;
    if (value == 'x') return PACKED_KEY_X;
    fprintf(stderr, "cannot pack character value %" PRIuFAST32 "\n", value);
    exit(EXIT_FAILURE);
}

#if !HAS_INFINITE_H
static PackedKey cells2packedkeywithoutfinal(uint_fast32_t tail) {
    uint_fast32_t cell = next[tail];
    PackedKey key = cellcontentpackedkey(contents[cell]);

    while (next[cell] != tail) {
        cell = next[cell];
        key = packedapplicationkey(key,
                                   cellcontentpackedkey(contents[cell]));
    }
    return key;
}

static void writeinfinitecombinator(uint_fast32_t bufferhead,
                                    const char *buffer) {
    if (infinitefile == NULL) return;

    size_t length = strlen(buffer);

    if ((length == 0) || (length > MAXBUF) ||
        (buffer[length - 1U] != 'x') || (contents[bufferhead] != 'x')) {
        fprintf(stderr, "cannot write malformed divergent combinator %s\n",
                buffer);
        exit(EXIT_FAILURE);
    }
    PackedKey key = cells2packedkeywithoutfinal(bufferhead);

    pthread_mutex_lock(&infinitefilelock);
    errno = 0;
    if (fprintf(infinitefile, "0x%016" PRIx64 ": %.*s\n",
                (uint64_t)key, (int)(length - 1U), buffer) < 0) {
        int error = errno ? errno : EIO;
        FILE *failedfile = infinitefile;

        infinitefile = NULL;
        (void)fclose(failedfile);
        fprintf(stderr, "failed to write %s: %s\n",
                infinitepath, strerror(error));
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    pthread_mutex_unlock(&infinitefilelock);
}
#endif

typedef struct {
    uint_fast32_t head;
    uint_fast32_t tail;
} CellSpan;

enum SArgumentShape {
    S_ARGUMENT_ERROR = -1,
    S_ARGUMENT_OTHER,
    S_ARGUMENT_SINGLE_K,
    S_ARGUMENT_EXACT_SK,
    S_ARGUMENT_K_APPLICATION,
    S_ARGUMENT_LEGACY_SK_APPLICATION
};

#define S_ARGUMENT_SHAPE_BITS 3U
#define S_ARGUMENT_SHAPE_MASK ((1U << S_ARGUMENT_SHAPE_BITS) - 1U)
_Static_assert(S_ARGUMENT_LEGACY_SK_APPLICATION <= S_ARGUMENT_SHAPE_MASK,
               "S argument shapes do not fit their packed representation");

static enum SArgumentShape classifySargumenttail(uint_fast32_t tail,
                                                  int allowlegacy) {
    uint_fast32_t first = next[tail];

    if (first == 0) return S_ARGUMENT_OTHER;
    if (first == tail) {
        return contents[first] == 'K'
            ? S_ARGUMENT_SINGLE_K : S_ARGUMENT_OTHER;
    }

    uint_fast32_t second = next[first];

    if (second == 0) return S_ARGUMENT_OTHER;
    if (second == tail) {
        if ((contents[first] == 'S') && (contents[second] == 'K')) {
            return S_ARGUMENT_EXACT_SK;
        }
        return contents[first] == 'K'
            ? S_ARGUMENT_K_APPLICATION : S_ARGUMENT_OTHER;
    }
    if (!allowlegacy || (next[second] != tail)) {
        return S_ARGUMENT_OTHER;
    }
    return ((contents[first] == 'S') && (contents[second] == 'K'))
        ? S_ARGUMENT_LEGACY_SK_APPLICATION : S_ARGUMENT_OTHER;
}

// Inspect an S argument once. Tagged shortcut shapes are made private here
// because the shortcut code consumes their rings destructively. The returned
// shape describes that private copy, so tryoptimizedS does not need to traverse
// it again. A one-cell tagged K becomes the immediate K value.
static inline enum SArgumentShape
prepareSargument(uint_fast32_t cell, int allowlegacy) {
    uint_fast32_t value = contents[cell];

    if (value < FREEMIN) return S_ARGUMENT_OTHER;

    uint_fast32_t tail = resolvedtail(value);
    enum SArgumentShape shape =
        classifySargumenttail(tail, allowlegacy);

    if (shape == S_ARGUMENT_OTHER) return shape;
    if (ismemocontents(value)) {
        if (shape == S_ARGUMENT_SINGLE_K) {
            releasecontents(value);
            contents[cell] = 'K';
            return S_ARGUMENT_OTHER;
        }

        uint_fast32_t copy = clonecells(tail);

        if (copy == 0) return S_ARGUMENT_ERROR;
        releasecontents(value);
        contents[cell] = copy;
    } else if (shape == S_ARGUMENT_SINGLE_K) {
        // Preserve the existing treatment of a direct one-cell (K) ring.
        return S_ARGUMENT_OTHER;
    }
    return shape;
}

CellSpan takeownedcell(uint_fast32_t cell) {
    CellSpan span;
    uint_fast32_t value = contents[cell];

    if (!isdirectcontents(value)) {
        span.head = cell;
        span.tail = cell;
    } else {
        span.head = next[value];
        span.tail = value;
        putfree(cell);
    }
    return span;
}

void discardownedcell(uint_fast32_t cell) {
    releasecontents(contents[cell]);
    putfree(cell);
}

CellSpan makecanonicalSKKtop(uint_fast32_t skcell) {
    uint_fast32_t ktail = contents[skcell];
    CellSpan span = {next[ktail], skcell};

    contents[skcell] = 'K';
    next[ktail] = skcell;
    return span;
}

void makecanonicalSKKargument(uint_fast32_t skcell,
                              uint_fast32_t thirdcell) {
    uint_fast32_t ktail = contents[skcell];
    uint_fast32_t skhead = next[ktail];

    contents[thirdcell] = 'K';
    next[ktail] = thirdcell;
    next[thirdcell] = skhead;
    contents[skcell] = thirdcell;
}

void finishSspan(CellSpan span, uint_fast32_t rest,
                 uint_fast32_t *resulthead, uint_fast32_t *resulttail) {
    *resulthead = span.head;
    next[span.tail] = rest;
    if (rest == 0) *resulttail = span.tail;
}

int tryoptimizedS(uint_fast32_t scell,
                  uint_fast32_t xcell, uint_fast32_t x,
                  uint_fast32_t ycell, unsigned packedshapes,
                  uint_fast32_t zcell, uint_fast32_t rest,
                  uint_fast32_t *resulthead, uint_fast32_t *resulttail,
                  uint_fast32_t *additionalsteps) {
    enum SArgumentShape xshape =
        (enum SArgumentShape)(packedshapes & S_ARGUMENT_SHAPE_MASK);
    enum SArgumentShape yshape =
        (enum SArgumentShape)(packedshapes >> S_ARGUMENT_SHAPE_BITS);
    // Every successful path below transfers or discards existing ownership.
    // None allocates, clones, or consults a memo table.
    if (x == 'K') {
        CellSpan result = takeownedcell(zcell);

        putfree(scell);
        putfree(xcell);
        discardownedcell(ycell);
        finishSspan(result, rest, resulthead, resulttail);
        *additionalsteps = 1; // K z (y z) -> z
        return 1;
    }

    // S (SK) y z -> y z.
    if (xshape == S_ARGUMENT_EXACT_SK) {
        if (yshape == S_ARGUMENT_EXACT_SK) {
            CellSpan result = makecanonicalSKKtop(xcell);

            putfree(scell);
            discardownedcell(ycell);
            discardownedcell(zcell);
            finishSspan(result, rest, resulthead, resulttail);
            *additionalsteps = 2; // SK z (y z) -> y z
            return 1;
        }
        if (yshape == S_ARGUMENT_K_APPLICATION) {
            uint_fast32_t y = contents[ycell];
            uint_fast32_t yfirst = next[y];
            CellSpan result = takeownedcell(y);

            putfree(scell);
            discardownedcell(xcell);
            discardownedcell(zcell);
            putfree(yfirst);
            putfree(ycell);
            finishSspan(result, rest, resulthead, resulttail);
            *additionalsteps = 3; // SK eliminates x; K eliminates y
            return 1;
        }

        CellSpan result = takeownedcell(ycell);

        putfree(scell);
        discardownedcell(xcell);
        next[result.tail] = zcell;
        result.tail = zcell;
        finishSspan(result, rest, resulthead, resulttail);
        *additionalsteps = 2; // SK z (y z) -> y z
        return 1;
    }

    // S (K a) y z -> a (y z).
    if (xshape == S_ARGUMENT_K_APPLICATION) {
        if (yshape == S_ARGUMENT_EXACT_SK) {
            uint_fast32_t xfirst = next[x];
            CellSpan result = takeownedcell(x);

            putfree(xfirst);
            putfree(xcell);
            discardownedcell(zcell);
            makecanonicalSKKargument(ycell, scell);
            next[result.tail] = ycell;
            result.tail = ycell;
            finishSspan(result, rest, resulthead, resulttail);
            *additionalsteps = 1; // K<any> z -> <any>
            return 1;
        }
        if (yshape == S_ARGUMENT_K_APPLICATION) {
            uint_fast32_t y = contents[ycell];
            uint_fast32_t xfirst = next[x];
            uint_fast32_t yfirst = next[y];
            CellSpan result = takeownedcell(x);

            putfree(scell);
            putfree(xfirst);
            putfree(xcell);
            putfree(yfirst);
            putfree(ycell);
            discardownedcell(zcell);
            next[result.tail] = y;
            result.tail = y;
            finishSspan(result, rest, resulthead, resulttail);
            *additionalsteps = 2; // both K applications discard z
            return 1;
        }

        uint_fast32_t xfirst = next[x];
        CellSpan result = takeownedcell(x);
        CellSpan yz = takeownedcell(ycell);

        putfree(scell);
        putfree(xfirst);
        next[yz.tail] = zcell;
        next[zcell] = yz.head;
        contents[xcell] = zcell;
        next[result.tail] = xcell;
        result.tail = xcell;
        finishSspan(result, rest, resulthead, resulttail);
        *additionalsteps = 1; // K<any> z -> <any>
        return 1;
    }

    // S x (SK) z -> x z (SKK).
    if (yshape == S_ARGUMENT_EXACT_SK) {
        CellSpan result = takeownedcell(xcell);

        makecanonicalSKKargument(ycell, scell);
        next[result.tail] = zcell;
        next[zcell] = ycell;
        result.tail = ycell;
        finishSspan(result, rest, resulthead, resulttail);
        *additionalsteps = 0;
        return 1;
    }
    // S x (K a) z -> x z a.
    if (yshape == S_ARGUMENT_K_APPLICATION) {
        uint_fast32_t y = contents[ycell];
        uint_fast32_t yfirst = next[y];
        CellSpan result = takeownedcell(xcell);

        putfree(scell);
        putfree(yfirst);
        putfree(ycell);
        next[result.tail] = zcell;
        next[zcell] = y;
        result.tail = y;
        finishSspan(result, rest, resulthead, resulttail);
        *additionalsteps = 1; // K<any> z -> <any>
        return 1;
    }
    return 0;
}

// Give zcell and duplicate one reference each to the same subexpression.
// The original owned ring moves into a new memo cell; an existing memo is
// simply retained. Characters remain immediate values.
static int shareScontents(uint_fast32_t zcell, uint_fast32_t *duplicate) {
    uint_fast32_t value = contents[zcell];

    if (value < FREEMIN) {
        *duplicate = value;
        return 1;
    }
    if (ismemocontents(value)) {
        memoacquire(value);
        *duplicate = value;
        return 1;
    }

    uint_fast32_t memo = getfree();

    if (memo == 0) return 0;
    contents[memo] = value;
    next[memo] = 2;
    value = MEMO_BIT | memo;
    contents[zcell] = value;
    *duplicate = value;
    return 1;
}

// Reduce one S or K redex at the front of an open linear chain. The returned
// cost includes contractions performed algebraically by the existing direct S
// shortcuts. A zero result means weak-head form; -1 means arena exhaustion.
static int reducetop(uint_fast32_t *headptr, uint_fast32_t *tailptr,
                     uint_fast32_t *cost) {
    uint_fast32_t head = *headptr;
    uint_fast32_t tail = *tailptr;
    uint_fast32_t curchar = contents[head];

    *cost = 0;
    if (curchar == 'S') {
        uint_fast32_t xcell = next[head];

        if (xcell == 0) return 0;
        uint_fast32_t ycell = next[xcell];
        if (ycell == 0) return 0;
        uint_fast32_t zcell = next[ycell];
        if (zcell == 0) return 0;

        uint_fast32_t rest = next[zcell];
        uint_fast32_t x;
        uint_fast32_t y;
        uint_fast32_t z = contents[zcell];
        uint_fast32_t optimizedsteps;
        enum SArgumentShape xshape;
        enum SArgumentShape yshape = S_ARGUMENT_OTHER;

        xshape = prepareSargument(xcell, 0);
        if (xshape == S_ARGUMENT_ERROR) return -1;
        x = contents[xcell];
        if (x != 'K') {
            yshape = prepareSargument(ycell, 1);
            if (yshape == S_ARGUMENT_ERROR) return -1;
        }
        unsigned packedshapes =
            (unsigned)xshape |
            ((unsigned)yshape << S_ARGUMENT_SHAPE_BITS);

        if (tryoptimizedS(head, xcell, x, ycell, packedshapes, zcell, rest,
                          &head, &tail, &optimizedsteps)) {
            *headptr = head;
            *tailptr = tail;
            *cost = optimizedsteps + 1;
            return 1;
        }

        y = contents[ycell];

        uint_fast32_t xhead = xcell;
        uint_fast32_t xtail = xcell;
        uint_fast32_t yhead = ycell;
        uint_fast32_t ytail = ycell;
        uint_fast32_t savey = ycell;
        uint_fast32_t needtofreex = 0;
        int gotSK = yshape == S_ARGUMENT_LEGACY_SK_APPLICATION;

        if (isdirectcontents(x)) {
            needtofreex = xcell;
            xhead = next[x];
            xtail = x;
        }
        if (isdirectcontents(y)) {
            yhead = next[y];
            ytail = y;
        }

        putfree(head);
        head = xhead;
        next[xtail] = zcell;

        if (gotSK) {
            // (SK<any>) z -> z, leaving x z z. This pre-existing shortcut
            // keeps its direct clone behavior; generic S is memoized below.
            uint_fast32_t duplicate = clonecontents(z);

            if (duplicate == 0) {
                *headptr = head;
                *tailptr = tail;
                return -1;
            }
            contents[savey] = duplicate;
            next[savey] = rest;
            next[zcell] = savey;
            if (rest == 0) tail = savey;
            freeall(y);
            *cost = 3;
        } else {
            uint_fast32_t duplicate;

            if (!shareScontents(zcell, &duplicate)) {
                *headptr = head;
                *tailptr = tail;
                return -1;
            }
            // A direct y ring is transferred and its old wrapper can become
            // the duplicate-z cell. Characters and memo tags retain their
            // occurrence cell, so they need a new cell for duplicate z.
            if (!isdirectcontents(y)) {
                savey = getfree();
                if (savey == 0) {
                    *headptr = head;
                    *tailptr = tail;
                    return -1;
                }
            }
            contents[savey] = duplicate;
            next[savey] = yhead;
            next[ytail] = savey;

            uint_fast32_t wrapper = getfree();
            if (wrapper == 0) {
                *headptr = head;
                *tailptr = tail;
                return -1;
            }
            next[zcell] = wrapper;
            contents[wrapper] = savey;
            next[wrapper] = rest;
            if (rest == 0) tail = wrapper;
            *cost = 1;
        }
        if (needtofreex) putfree(needtofreex);
        *headptr = head;
        *tailptr = tail;
        return 1;
    }

    if (curchar == 'K') {
        uint_fast32_t xcell = next[head];

        if (xcell == 0) return 0;
        uint_fast32_t ycell = next[xcell];
        if (ycell == 0) return 0;

        uint_fast32_t rest = next[ycell];
        uint_fast32_t x = contents[xcell];
        uint_fast32_t xhead = xcell;
        uint_fast32_t xtail = xcell;
        uint_fast32_t needtofreex = 0;

        if (isdirectcontents(x)) {
            needtofreex = xcell;
            xhead = next[x];
            xtail = x;
        }
        putfree(head);
        head = xhead;
        releasecontents(contents[ycell]);
        putfree(ycell);
        next[xtail] = rest;
        if (rest == 0) tail = xtail;
        if (needtofreex) putfree(needtofreex);

        *headptr = head;
        *tailptr = tail;
        *cost = 1;
        return 1;
    }
    return 0;
}

enum HeadExposure {
    HEAD_ERROR = -1,
    HEAD_READY = 0,
    HEAD_PROGRESS = 1,
    HEAD_CHANGED = 2
};

static int ringisweakhead(uint_fast32_t tail) {
    uint_fast32_t head = next[tail];
    uint_fast32_t value = contents[head];

    if (value >= FREEMIN) return 0;
    if (value == 'K') {
        return (head == tail) || (next[head] == tail);
    }
    if (value == 'S') {
        if (head == tail) return 1;
        uint_fast32_t firstargument = next[head];

        return (firstargument == tail) || (next[firstargument] == tail);
    }
    return 1;
}

// Perform only zero-cost head exposure. Calling this immediately before the
// evaluator observation makes a just-produced tagged head look like the old
// privately cloned representation without reducing a shared target early.
static int exposeweakhead(uint_fast32_t *headptr, uint_fast32_t *tailptr) {
    int changed = 0;

    for (;;) {
        uint_fast32_t occurrence = *headptr;
        uint_fast32_t value = contents[occurrence];

        if (value < FREEMIN) return changed ? HEAD_CHANGED : HEAD_READY;

        uint_fast32_t rest = next[occurrence];
        uint_fast32_t replacement;

        if (isdirectcontents(value)) {
            replacement = value;
        } else {
            uint_fast32_t memo = memocell(value);
            uint_fast32_t state = next[memo];

            if ((state & MEMO_BIT) != 0) return HEAD_ERROR;
            if (memoreferences(memo) == 1) {
                replacement = contents[memo];
                putfree(memo);
            } else {
                if (!ringisweakhead(contents[memo])) return HEAD_READY;
                replacement = clonecells(contents[memo]);
                if (replacement == 0) return HEAD_ERROR;
                releasecontents(value);
            }
        }

        uint_fast32_t replacementhead = next[replacement];

        next[replacement] = rest;
        if (occurrence == *tailptr) *tailptr = replacement;
        putfree(occurrence);
        *headptr = replacementhead;
        changed = 1;
    }
}

static int openpathmemo(MemoPath *path, uint_fast32_t *headptr,
                        uint_fast32_t *tailptr) {
    uint_fast32_t memo = path->frames[path->depth - 1].memo;
    uint_fast32_t state = next[memo];

    if ((state & MEMO_BIT) != 0) return 0;
    next[memo] = state | MEMO_BIT;
    *tailptr = contents[memo];
    *headptr = next[*tailptr];
    next[*tailptr] = 0;
    return 1;
}

static void closepathmemo(MemoPath *path, uint_fast32_t head,
                          uint_fast32_t tail) {
    uint_fast32_t memo = path->frames[path->depth - 1].memo;

    next[tail] = head;
    contents[memo] = tail;
    next[memo] &= MEMO_MASK;
}

// Remove leading parentheses/indirections from the base open chain. Shared
// memo targets are advanced with an iterative update path: only the deepest
// target is open, ancestor targets stay closed, and a weak-head child is
// materialized into its parent frame before that frame resumes. This keeps one
// outer observation per contraction without recursively revisiting ancestors.
static int exposehead(uint_fast32_t *headptr, uint_fast32_t *tailptr,
                      uint_fast32_t *cost, MemoPath *path) {
    uint_fast32_t basehead = *headptr;
    uint_fast32_t basetail = *tailptr;
    uint_fast32_t head = basehead;
    uint_fast32_t tail = basetail;
    int basechanged = 0;

    *cost = 0;
    if ((path->depth != 0) && !openpathmemo(path, &head, &tail)) {
        return HEAD_ERROR;
    }

    for (;;) {
        uint_fast32_t occurrence = head;
        uint_fast32_t value = contents[occurrence];

        if (value >= FREEMIN) {
            uint_fast32_t rest = next[occurrence];
            uint_fast32_t replacement;

            if (isdirectcontents(value)) {
                replacement = value;
            } else {
                uint_fast32_t memo = memocell(value);
                uint_fast32_t state = next[memo];

                if (((state & MEMO_BIT) != 0) || memoinpath(path, memo)) {
                    if (path->depth != 0) {
                        closepathmemo(path, head, tail);
                        *headptr = basehead;
                        *tailptr = basetail;
                    } else {
                        *headptr = head;
                        *tailptr = tail;
                    }
                    return HEAD_ERROR;
                }
                if (memoreferences(memo) == 1) {
                    replacement = contents[memo];
                    putfree(memo);
                } else {
                    size_t olddepth = path->depth;

                    pushmemopath(path, memo, occurrence, tail);
                    if (olddepth != 0) {
                        uint_fast32_t parentmemo =
                            path->frames[olddepth - 1].memo;

                        next[tail] = head;
                        contents[parentmemo] = tail;
                        next[parentmemo] &= MEMO_MASK;
                    } else {
                        basehead = head;
                        basetail = tail;
                    }
                    if (!openpathmemo(path, &head, &tail)) {
                        *headptr = basehead;
                        *tailptr = basetail;
                        return HEAD_ERROR;
                    }
                    continue;
                }
            }

            uint_fast32_t replacementhead = next[replacement];

            next[replacement] = rest;
            if (occurrence == tail) tail = replacement;
            putfree(occurrence);
            head = replacementhead;
            if (path->depth == 0) basechanged = 1;
            continue;
        }

        if (path->depth == 0) {
            *headptr = head;
            *tailptr = tail;
            return basechanged ? HEAD_CHANGED : HEAD_READY;
        }

        int reduced = reducetop(&head, &tail, cost);

        if (reduced < 0) {
            closepathmemo(path, head, tail);
            *headptr = basehead;
            *tailptr = basetail;
            return HEAD_ERROR;
        }
        if (reduced > 0) {
            closepathmemo(path, head, tail);
            *headptr = basehead;
            *tailptr = basetail;
            return HEAD_PROGRESS;
        }

        size_t childdepth = path->depth;
        MemoPathFrame childframe = path->frames[childdepth - 1];
        uint_fast32_t childmemo = childframe.memo;
        uint_fast32_t childtag = contents[childframe.occurrence];
        uint_fast32_t replacement;
        int steal = 0;

        closepathmemo(path, head, tail);
        if (memoreferences(childmemo) == 1) {
            replacement = contents[childmemo];
            steal = 1;
        } else {
            replacement = clonecells(contents[childmemo]);
            if (replacement == 0) {
                *headptr = basehead;
                *tailptr = basetail;
                return HEAD_ERROR;
            }
        }

        size_t parentdepth = childdepth - 1;

#if PARANOID
        if (!ismemocontents(childtag) ||
            (memocell(childtag) != childmemo)) {
            printf("*** Programmer error: memo update occurrence changed\n");
            INT3
            if (!steal) freeall(replacement);
            *headptr = basehead;
            *tailptr = basetail;
            return HEAD_ERROR;
        }
        if ((parentdepth != 0) &&
            ((next[path->frames[parentdepth - 1].memo] & MEMO_BIT) != 0)) {
            printf("*** Programmer error: memo update parent is busy\n");
            INT3
            if (!steal) freeall(replacement);
            *headptr = basehead;
            *tailptr = basetail;
            return HEAD_ERROR;
        }
#endif

        truncatememopath(path, parentdepth);
        if (parentdepth == 0) {
            head = basehead;
            tail = basetail;
        } else if (!openpathmemo(path, &head, &tail)) {
            if (!steal) freeall(replacement);
            *headptr = basehead;
            *tailptr = basetail;
            return HEAD_ERROR;
        }

#if PARANOID
        if ((head != childframe.occurrence) ||
            (tail != childframe.parenttail)) {
            printf("*** Programmer error: memo update parent changed\n");
            INT3
            if (parentdepth != 0) closepathmemo(path, head, tail);
            if (!steal) freeall(replacement);
            *headptr = basehead;
            *tailptr = basetail;
            return HEAD_ERROR;
        }
#endif

        uint_fast32_t rest = next[childframe.occurrence];
        uint_fast32_t replacementhead = next[replacement];

        if (steal) {
            putfree(childmemo);
        } else {
            releasecontents(childtag);
        }
        next[replacement] = rest;
        if (childframe.occurrence == tail) tail = replacement;
        putfree(childframe.occurrence);
        head = replacementhead;

        if (parentdepth == 0) {
            basehead = head;
            basetail = tail;
            basechanged = 1;
        }
    }
}

#define RESTOREHEAD \
if (gotx) { \
    next[tail] = head; \
    contents[subowner] = tail; \
    subhead = tail; \
} else { \
    next[tail] = head; \
    evalhead = tail; \
}

void evalcells(unsigned length, uint_fast32_t bufferhead, uint_fast32_t evalhead,
               uint_fast32_t initlen, char *buffer, int doprint) {
    uint_fast32_t steps = 0;
    int gotx = 0;
    int repeatsforever = 0;
    int gotwinner = 0;
    uint_fast32_t cells = evalhead;
    uint_fast32_t head;
    uint_fast32_t tail;
    uint_fast32_t curconts;
    uint_fast32_t subhead = 0;
    uint_fast32_t subowner = 0;
    uint_fast32_t submemo = 0;
    MemoPath *path = &evaluatorpath;

    clearmemopath(path);
    
#if 0
    putcells(evalhead);
#endif
#if PARANOID
    if (cells == 0) {
        printf("*** Programmer error: unexpected end of cells at evalhead\n");
        INT3
        return;
    }
#endif
    tail = cells;
    head = next[tail];
    next[tail] = 0;
    //printf("evalhead:%" PRIuFAST32 " head:%" PRIuFAST32 " tail:%" PRIuFAST32 "\n", evalhead, head, tail);
#if PARANOID
    if (head == tail) {
        printf("*** Programmer error: only one item in cells at %" PRIuFAST32 "\n", evalhead);
        INT3
        return;
    }
#endif

    while (steps < MAXSTEPS) {
        uint_fast32_t reductioncost = 0;
        int exposed = exposehead(&head, &tail, &reductioncost, path);

        if (exposed == HEAD_ERROR) {
            steps = MAXSTEPS;
            break;
        }
        if (exposed == HEAD_PROGRESS) {
            steps += reductioncost;
            goto reductionobserved;
        }
        if (exposed == HEAD_CHANGED) goto reductionobserved;

        cells = head;
#if PARANOID
        if (cells == 0) {
            RESTOREHEAD
            if (submemo) next[submemo] &= MEMO_MASK;
            printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n",
                   evalhead);
            INT3
            return;
        }
#endif
        curconts = contents[cells];
#if PARANOID
        if (curconts >= FREEMIN) {
            RESTOREHEAD
            if (submemo) next[submemo] &= MEMO_MASK;
            printf("*** Programmer error: unresolved head at %" PRIuFAST32 "\n",
                   evalhead);
            INT3
            return;
        }
#endif

        int reduced = reducetop(&head, &tail, &reductioncost);

        if (reduced < 0) {
            steps = MAXSTEPS;
            break;
        }
        if (reduced > 0) {
            steps += reductioncost;
            goto reductionobserved;
        }

        if (curconts == 'x') {
            // x is only special at the head of a two-item expression.
            if (gotx || (cells == tail)) break;
            cells = next[cells];
            if ((cells == 0) || (cells != tail)) break;
            curconts = contents[cells];
            if (curconts < FREEMIN) break;

            next[tail] = head;
            evalhead = tail;
            if (ismemocontents(curconts)) {
                submemo = memocell(curconts);
                if ((next[submemo] & MEMO_BIT) != 0) {
                    steps = MAXSTEPS;
                    break;
                }
                next[submemo] |= MEMO_BIT;
                subowner = submemo;
                tail = contents[submemo];
            } else {
                subowner = cells;
                tail = curconts;
            }
            head = next[tail];
            subhead = tail;
            gotx = 1;
            if (equalcellspath(bufferhead, subhead, 0, path) == 0) {
                next[tail] = 0;
                continue;
            }
            reductioncost = 1;
            steps += reductioncost;
            goto reductionobserved;
        }

        if ((curconts == 'S') || (curconts == 'K')) break;

        puts(buffer);
        putchar('=');
        RESTOREHEAD
        if (submemo) next[submemo] &= MEMO_MASK;
        putcells(evalhead);
        fprintf(stderr, "*** Programmer error: not S, K, or x at %" PRIuFAST32 "\n",
                evalhead);
        INT3
        return;

reductionobserved:
        {
            int weakheadexposed = exposeweakhead(&head, &tail);

            if (weakheadexposed == HEAD_ERROR) {
                steps = MAXSTEPS;
            } else if (weakheadexposed == HEAD_CHANGED) {
                clearmemopath(path);
            }
        }
        RESTOREHEAD
        if (gotx) {
            if (equalcellspath(bufferhead, subhead, 0, path)) {
                gotwinner = 1;
                break;
            }
        } else {
            repeatsforever = equalcellspath(bufferhead, evalhead, 1, path);
            if (repeatsforever) break;
        }
        // Reducer mutations use a temporary linear chain. Close it for the
        // structural observations above, then reopen it for another turn.
        next[tail] = 0;
    }

    uint_fast32_t peakcells = (highwatermark - FREEMIN) - initlen;

    RESTOREHEAD
    if (submemo) next[submemo] &= MEMO_MASK;
    if (steps == 0) {
        if (atomic_load(&bufmaxis0)) {
            pthread_mutex_lock(&globallock);
            if (atomic_load(&bufmaxis0)) {
                strncpy(bufmax, buffer, MAXBUF);
                atomic_store(&bufmaxis0, ((bufmax[0] == '\0') ? 1 : 0));
            }
            pthread_mutex_unlock(&globallock);
        }
        if (atomic_load(&maxlen) < peakcells) {
            pthread_mutex_lock(&globallock);
            if (atomic_load(&maxlen) < peakcells) {
                atomic_store(&maxlenstep, 0);
                strncpy(bufmaxlen, buffer, MAXBUF);
                atomic_store(&maxlen, peakcells);
            }
            pthread_mutex_unlock(&globallock);
        }
    } else {
        if (repeatsforever) {
#if !HAS_INFINITE_H
            writeinfinitecombinator(bufferhead, buffer);
#endif
            (void)atomic_fetch_add(&totalinfinitecount, 1);
            if (repeatsforever == 1) {
                (void)atomic_fetch_add(&repeatcount, 1);
                if ((length <= 8) || (length == 13)) {
                    pthread_mutex_lock(&printlock);
                    printf("\r%s                                     \n", buffer);
                    putchar('=');
                    printcells(evalhead);
                    printf(" in %" PRIuFAST32 " steps with maximum cell count %" PRIuFAST32 "\n",
                           steps, peakcells);
                    printf("*** Repeats forever\n\n");
                    fflush(stdout);
                    pthread_mutex_unlock(&printlock);
                }
            } else {
                (void)atomic_fetch_add(&nevercount, 1);
                if (length <= 6) {
                    pthread_mutex_lock(&printlock);
                    printf("\r%s                                     \n", buffer);
                    putchar('=');
                    printcells(evalhead);
                    printf(" in %" PRIuFAST32 " steps with maximum cell count %" PRIuFAST32 "\n",
                           steps, peakcells);
                    printf("*** Never ends\n\n");
                    fflush(stdout);
                    pthread_mutex_unlock(&printlock);
                }
            }
        } else if (steps >= MAXSTEPS) {
#if !HAS_INFINITE_H
            writeinfinitecombinator(bufferhead, buffer);
#endif
            (void)atomic_fetch_add(&totalinfinitecount, 1);
            (void)atomic_fetch_add(&nevercount, 1);
            if (length <= 6) {
                pthread_mutex_lock(&printlock);
                printf("\r%s ran out of resources                \n", buffer);
                printf("*** Never ends\n\n");
                fflush(stdout);
                pthread_mutex_unlock(&printlock);
            }
        } else {
            if ((atomic_load(&maxstep) < steps) ||
                ((atomic_load(&maxstep) == steps) && (atomic_load(&maxstepstrlen) < peakcells))) {
                pthread_mutex_lock(&globallock);
                if ((atomic_load(&maxstep) < steps) ||
                    ((atomic_load(&maxstep) == steps) && (atomic_load(&maxstepstrlen) < peakcells))) {
                    atomic_store(&maxstepstrlen, peakcells);
                    strncpy(bufmax, buffer, MAXBUF);
                    atomic_store(&bufmaxis0, ((bufmax[0] == '\0') ? 1 : 0));
                    atomic_store(&maxstep, steps);
                }
                pthread_mutex_unlock(&globallock);
            }
            if ((atomic_load(&maxlen) < peakcells) ||
                ((atomic_load(&maxlen) == peakcells) && (atomic_load(&maxlenstep) == 0))) {
                pthread_mutex_lock(&globallock);
                if ((atomic_load(&maxlen) < peakcells) ||
                    ((atomic_load(&maxlen) == peakcells) && (atomic_load(&maxlenstep) == 0))) {
                    atomic_store(&maxlenstep, steps);
                    strncpy(bufmaxlen, buffer, MAXBUF);
                    atomic_store(&maxlen, peakcells);
                }
                pthread_mutex_unlock(&globallock);
            }
        }
    }
    if (gotwinner || doprint) {
        pthread_mutex_lock(&printlock);
        printf("\r%s                                     \n", buffer);
        putchar('=');
        printcells(evalhead);
        printf(" in %" PRIuFAST32 " steps with maximum cell count %" PRIuFAST32 "\n",
               steps, peakcells);
        if (gotwinner) {
            char tempbuf[MAXBUF + 1];
        
            strcpy(tempbuf, buffer);
            // remove trailing x from buffer
            tempbuf[strlen(buffer) - 1] = '\0';
            printf("!!! Y = %s\n\n", tempbuf);
        }
        fflush(stdout);
        pthread_mutex_unlock(&printlock);
    }
}

typedef struct {
    int left;
    int right;
    int skbit;
} NumericNode;

int decodenumericnode(uint_fast32_t num, int_fast32_t *position,
                      unsigned length, unsigned *leafnum,
                      NumericNode *nodes, unsigned *nodecount) {
    unsigned currentnode = (*nodecount)++;
    int isapplication = (num & ((uint_fast32_t)1 << *position)) != 0;

    *position -= 1;
    if (!isapplication) {
        unsigned currentleaf = (*leafnum)++;

        nodes[currentnode].left = -1;
        nodes[currentnode].right = -1;
        nodes[currentnode].skbit =
            (currentleaf == 0) ? -1 : (int)(length - currentleaf);
        return (int)currentnode;
    }

    nodes[currentnode].skbit = -1;
    nodes[currentnode].left = decodenumericnode(num, position, length,
                                                leafnum, nodes, nodecount);
    nodes[currentnode].right = decodenumericnode(num, position, length,
                                                 leafnum, nodes, nodecount);
    return (int)currentnode;
}

int decodenumerictree(unsigned length, uint_fast32_t num, NumericNode *nodes) {
    int_fast32_t position = (int_fast32_t)(2 * length);
    unsigned leafnum = 0;
    unsigned nodecount = 0;
    int root = decodenumericnode(num, &position, length, &leafnum,
                                 nodes, &nodecount);

#if PARANOID
    if ((position != -1) || (leafnum != (length + 1)) ||
        (nodecount != ((2 * length) + 1))) {
        printf("*** Programmer error: malformed numeric expression during filtering\n");
        INT3
        return -1;
    }
#endif
    return root;
}

unsigned char numericnodesymbol(const NumericNode *node, unsigned count) {
    return ((node->skbit >= 0) && (count & (1U << node->skbit))) ? 'K' : 'S';
}

static PackedKey numericnodepackedkey(const NumericNode *nodes, int root,
                                      unsigned count) {
    if (nodes[root].left < 0) {
        return numericnodesymbol(&nodes[root], count) == 'K'
            ? PACKED_KEY_K : PACKED_KEY_S;
    }
    return packedapplicationkey(
        numericnodepackedkey(nodes, nodes[root].left, count),
        numericnodepackedkey(nodes, nodes[root].right, count));
}

int numericnodehasKorSK(const NumericNode *nodes, int root,
                        unsigned count, unsigned extraargs) {
    int arguments[MAXLEN];
    unsigned argumentcount = 0;
    int current = root;

    while (nodes[current].left >= 0) {
        arguments[argumentcount++] = nodes[current].right;
        current = nodes[current].left;
    }

    unsigned totalarguments = argumentcount + extraargs;
    unsigned char head = numericnodesymbol(&nodes[current], count);

    if ((head == 'K') && (totalarguments >= 2)) {
        return 1;
    }
    if ((head == 'S') && (totalarguments >= 3) && (argumentcount != 0)) {
        int firstargument = arguments[argumentcount - 1];

        if ((nodes[firstargument].left < 0) &&
            (numericnodesymbol(&nodes[firstargument], count) == 'K')) {
            return 1;
        }
    }

    for (unsigned i = 0; i < argumentcount; ++i) {
        int argument = arguments[i];

        if ((nodes[argument].left >= 0) &&
            numericnodehasKorSK(nodes, argument, count, 0)) {
            return 1;
        }
    }
    return 0;
}

uint_fast32_t num2cell(uint_fast32_t num, int_fast32_t *position,
                       unsigned length, unsigned count, unsigned *leafnum) {
    int isapplication = (num & ((uint_fast32_t)1 << *position)) != 0;

    *position -= 1;
    if (!isapplication) {
        unsigned currentleaf = (*leafnum)++;

        if (currentleaf == 0) {
            return 'S';
        }
        return (count & (1U << (length - currentleaf))) ? 'K' : 'S';
    }

    uint_fast32_t left = num2cell(num, position, length, count, leafnum);
    if (left == 0) {
        return 0;
    }
    uint_fast32_t right = num2cell(num, position, length, count, leafnum);
    if (right == 0) {
        return 0;
    }
    uint_fast32_t head;
    uint_fast32_t tail;

    if (left < FREEMIN) {
        tail = getfree();
        if (tail == 0) {
            return 0;
        }
        contents[tail] = left;
        head = tail;
    } else {
        tail = left;
        head = next[tail];
    }

    uint_fast32_t newtail = getfree();
    if (newtail == 0) {
        return 0;
    }
    contents[newtail] = right;
    next[tail] = newtail;
    next[newtail] = head;
    return newtail;
}

uint_fast32_t num2cells(unsigned length, uint_fast32_t num, unsigned count) {
    int_fast32_t position = (int_fast32_t)(2 * length);
    unsigned leafnum = 0;
    uint_fast32_t tail = num2cell(num, &position, length, count, &leafnum);

    if (tail == 0) {
        return 0;
    }
    if (tail < FREEMIN) {
        uint_fast32_t temp = getfree();

        if (temp == 0) {
            return 0;
        }
        contents[temp] = tail;
        next[temp] = temp;
        tail = temp;
    }

    uint_fast32_t xcell = getfree();
    if (xcell == 0) {
        return 0;
    }
    contents[xcell] = 'x';
    next[xcell] = next[tail];
    next[tail] = xcell;

#if PARANOID
    if ((position != -1) || (leafnum != (length + 1))) {
        printf("*** Programmer error: malformed numeric expression\n");
        INT3
        return 0;
    }
#endif
    return xcell;
}

void dooneSK(unsigned length, uint_fast32_t num, unsigned count, char *buffer) {
    setupfreelist();
    
    uint_fast32_t bufferhead = num2cells(length, num, count);
    
#if PARANOID
    if (bufferhead == 0) {
        printf("*** Programmer error: unexpected end of cells at %s\n", buffer);
        INT3
        return;
    }
#endif
    
    uint_fast32_t initlen = highwatermark - FREEMIN;
    uint_fast32_t evalhead = clonecells(bufferhead);
    
#if PARANOID
    if (evalhead == 0) {
        printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", bufferhead);
        INT3
        return;
    }
#endif
    evalcells(length, bufferhead, evalhead, initlen, buffer, 0);
}

#if !SINGLE_THREAD
void dispatchworkbatch(const WorkBatch *batch) {
    uint64_t thmt = atomic_load(&threadempty);

#if PARANOID
    if ((batch->size == 0) || (batch->size > WORK_BATCH_CAPACITY)) {
        fprintf(stderr, "*** Programmer error: invalid worker batch size %u\n",
                batch->size);
        INT3
        exit(EXIT_FAILURE);
    }
#endif

    // A clear threadempty bit transfers the batch slot to the worker. The
    // worker copies its compact descriptors before setting the bit again, so
    // each worker can have one private active batch and one queued batch.
    while (thmt == 0) {
        atomic_store(&masterwaiting, 1);
        // A worker can become free just before masterwaiting is set. Recheck
        // after publishing the wait state so that worker either sees it and
        // posts, or its free bit is observed here.
        thmt = atomic_load(&threadempty);
        if (thmt == 0) {
            // macOS doesn't reliably restart sem_wait after a signal.
            for (;;) {
                int sw = sem_wait(mastersem);

                if (sw == 0) break;
                if (errno != EINTR) {
                    fprintf(stderr, "sem_wait on mastersem failed: ");
                    perror(NULL);
                    INT3
                    exit(EXIT_FAILURE);
                }
            }
        }
        atomic_store(&masterwaiting, 0);
        // Several workers may have posted while the producer was asleep.
        while ((sem_trywait(mastersem) == 0) || (errno == EINTR)) {
            // Drain surplus wakeups.
        }
        thmt = atomic_load(&threadempty);
    }

    unsigned threadnum = ctz64(thmt);

#if PARANOID
    if (threadnum >= (unsigned)maxthreads) {
        fprintf(stderr, "*** Programmer error: threadnum == %u >= maxthreads == %d\n",
                threadnum, maxthreads);
        INT3
        exit(EXIT_FAILURE);
    }
#endif

    uint64_t mask = (uint64_t)1 << threadnum;
    WorkBatch *destination = &workbatch[threadnum];

    destination->length = batch->length;
    destination->num = batch->num;
    destination->size = batch->size;
    strcpy(destination->buffer, batch->buffer);
    memcpy(destination->index, batch->index,
           batch->length * sizeof(*batch->index));
    memcpy(destination->counts, batch->counts,
           batch->size * sizeof(*batch->counts));
    (void)atomic_fetch_and(&threadempty, ~mask);

    /*
     * If the producer cleared the waiting bit, it owns the wakeup and must
     * post. Otherwise the worker observed the published batch directly.
     */
    if (atomic_fetch_and(&threadwaiting, ~mask) & mask) {
        if (sem_post(threadsem[threadnum]) != 0) {
            fprintf(stderr, "sem_post on threadsem[%02u] failed: ", threadnum);
            perror(NULL);
            INT3
            exit(EXIT_FAILURE);
        }
    }
}
#endif

void generateallSK(unsigned length, uint_fast32_t num, char *buffer) {
    int index[MAXLEN];
    unsigned maxcount = 1U << length;
    NumericNode numericnodes[(2 * MAXLEN) - 1];
    int numericroot = decodenumerictree(length, num, numericnodes);
#if !SINGLE_THREAD
    WorkBatch batch = {.length = length, .num = num, .size = 0};
#endif

#if PARANOID
    if (numericroot < 0) {
        return;
    }
#endif

    prepareSKbuffer(length, num, buffer, index);
#if !SINGLE_THREAD
    strcpy(batch.buffer, buffer);
    memcpy(batch.index, index, length * sizeof(*index));
#endif
    for (unsigned count = 0; count < maxcount; ++count) {
        if (numericnodehasKorSK(numericnodes, numericroot,
                               count, 1)) {
            continue;
        }
        if ((neverendingset.size != 0) &&
            packedkeysetcontains(
                &neverendingset,
                numericnodepackedkey(numericnodes, numericroot, count))) {
            (void)atomic_fetch_add(&neverendsmatch, 1);
            continue;
        }
        setSKbuffer(length, count, buffer, index);
        if (((atomic_fetch_add(&checked, 1) + 1) % 1000) == 0) {
            pthread_mutex_lock(&printlock);
            printf("\rChecked:%" PRIuFAST32 " %s                                     ",
                   atomic_load(&checked), buffer);
            fflush(stdout);
            pthread_mutex_unlock(&printlock);
        }
#if SINGLE_THREAD
        dooneSK(length, num, count, buffer);
#else
        batch.counts[batch.size] = count;
        batch.size++;
        if (batch.size == WORK_BATCH_CAPACITY) {
            dispatchworkbatch(&batch);
            batch.size = 0;
        }
#endif
    }
#if !SINGLE_THREAD
    if (batch.size != 0) dispatchworkbatch(&batch);
#endif
}

#if !SINGLE_THREAD
void *threadrun(void *arg) {
    int mythreadnum;
    uint64_t mask;
    
    mythreadnum = *(int *)arg;
    myid = mythreadnum;
    mask = (uint64_t)1 << mythreadnum;
    for (;;) {
        if (atomic_load(&exiting) != 0) {
            break;
        }
        if ((atomic_load(&threadempty) & mask) == 0) {
            WorkBatch *mybatch = &workbatch[mythreadnum];
            unsigned mylen = mybatch->length;
            uint_fast32_t mynum = mybatch->num;
            unsigned mysize = mybatch->size;
            unsigned mycounts[WORK_BATCH_CAPACITY];
            char mybuf[MAXBUF + 1];
            int myindex[MAXLEN];

#if PARANOID
            if ((mysize == 0) || (mysize > WORK_BATCH_CAPACITY)) {
                fprintf(stderr, "*** Programmer error: invalid worker batch size %u\n",
                        mysize);
                INT3
                exit(EXIT_FAILURE);
            }
#endif
            strcpy(mybuf, mybatch->buffer);
            memcpy(myindex, mybatch->index,
                   mylen * sizeof(*myindex));
            memcpy(mycounts, mybatch->counts,
                   mysize * sizeof(*mycounts));

            // Everything needed by this worker is now private. Releasing the
            // slot here preserves the old one-slot lookahead while amortizing
            // that handoff over the entire batch.
            (void)atomic_fetch_or(&threadempty, mask);
            if (atomic_load(&masterwaiting)) {
                if (sem_post(mastersem) != 0) {
                    fprintf(stderr, "sem_post on mastersem failed: ");
                    perror(NULL);
                    INT3
                    exit(EXIT_FAILURE);
                }
            }

            for (unsigned i = 0; i < mysize; ++i) {
                unsigned mycount = mycounts[i];

                setSKbuffer(mylen, mycount, mybuf, myindex);
                dooneSK(mylen, mynum, mycount, mybuf);
            }
            continue;
        }
        (void)atomic_fetch_or(&threadwaiting, mask);
        
        /*
         * Recheck after registering as waiting.
         *
         * If work appeared and our fetch_and returns the bit, the worker
         * cleared it first and can consume the work directly.
         *
         * If it returns zero, the producer already cleared the bit and
         * therefore owns—and is committed to—the semaphore post. Fall
         * through to sem_wait() to consume that post.
         */
        if (((atomic_load(&threadempty) & mask) == 0) &&
            (atomic_fetch_and(&threadwaiting, ~mask) & mask)) {
            continue;
        }
        // mac os doesn't handle sigaction(SA_RESTART) correctly for sem_wait
        // so, if errno == EINTR, try again
        for (;;) {
            int sw = sem_wait(threadsem[mythreadnum]);
            
            if (sw == 0) {
                break;
            }
            if (errno != EINTR) {
                fprintf(stderr, "sem_wait on threadsem[%02d] failed: ", mythreadnum);
                perror(NULL);
                INT3
                exit(EXIT_FAILURE);
            }
        }
    }
    freememopath();
    return NULL;
}

void threadinit(void) {
    pthread_attr_t threadattr;
    
    if (atomic_load(&initdone)) {
        return;
    }
    if ((errno = pthread_attr_init(&threadattr)) != 0) {
        fprintf(stderr, "pthread_attr_init failed: ");
        perror(NULL);
        INT3
        exit(EXIT_FAILURE);
    }
    if ((errno = pthread_attr_setstacksize(&threadattr,
                                            WORKER_STACK_SIZE)) != 0) {
        fprintf(stderr, "pthread_attr_setstacksize failed: ");
        perror(NULL);
        INT3
        exit(EXIT_FAILURE);
    }
    mastersem = sem_open("/mastersem", O_CREAT, (S_IRUSR | S_IWUSR), 0);
    if (mastersem == SEM_FAILED) {
        fprintf(stderr, "sem_open of /mastersem failed: ");
        perror(NULL);
        INT3
        exit(EXIT_FAILURE);
    }
    if (sem_unlink("/mastersem") != 0) {
        fprintf(stderr, "sem_unlink of /mastersem failed: ");
        perror(NULL);
        INT3
        exit(EXIT_FAILURE);
    }
    // if /mastersem already existed, drain any possible previous sem_posts
    while ((sem_trywait(mastersem) == 0) || (errno == EINTR)) {
        // nothing to do, keep going
    }
    for (int i = 0; i < maxthreads; ++i) {
        snprintf(threadname[i], sizeof(threadname[i]), "/threadsem%02d", i);
        threadsem[i] = sem_open(threadname[i], O_CREAT, (S_IRUSR | S_IWUSR), 0);
        if (threadsem[i] == SEM_FAILED) {
            fprintf(stderr, "sem_open of %s failed: ", threadname[i]);
            perror(NULL);
            INT3
            exit(EXIT_FAILURE);
        }
        if (sem_unlink(threadname[i]) != 0) {
            fprintf(stderr, "sem_unlink of %s failed: ", threadname[i]);
            perror(NULL);
            INT3
            exit(EXIT_FAILURE);
        }
        // if /threadsem?? already existed, drain all possible previous sem_posts
        while ((sem_trywait(threadsem[i]) == 0) || (errno == EINTR)) {
            // nothing to do, keep going
        }
        if ((errno = pthread_create(&thread[i], &threadattr, threadrun, &i)) != 0) {
            fprintf(stderr, "pthread_create of thread %d failed: ", i);
            perror(NULL);
            INT3
            exit(EXIT_FAILURE);
        }
        nxt[i] = malloc(MAXARRAY * sizeof(uint_fast32_t));
        if (nxt[i] == NULL) {
            fprintf(stderr, "malloc(%zu) failed\n", (MAXARRAY * sizeof(uint_fast32_t)));
            INT3
            exit(EXIT_FAILURE);
        }
        cnts[i] = malloc(MAXARRAY * sizeof(uint_fast32_t));
        if (cnts[i] == NULL) {
            fprintf(stderr, "malloc(%zu) failed\n", (MAXARRAY * sizeof(uint_fast32_t)));
            INT3
            exit(EXIT_FAILURE);
        }
        // wait for thread to start
        while ((atomic_load(&threadwaiting) & ((uint64_t)1 << i)) == 0) {
            sched_yield();
        }
    }
    if ((errno = pthread_attr_destroy(&threadattr)) != 0) {
        fprintf(stderr, "pthread_attr_destroy failed: ");
        perror(NULL);
        INT3
        exit(EXIT_FAILURE);
    }
    atomic_store(&initdone, 1);
}

void threadfinal(void) {
    atomic_store(&exiting, 1);
    // do sem_post on all semaphores before sem_close
    if (sem_post(mastersem) != 0) {
        fprintf(stderr, "sem_post on mastersem failed: ");
        perror(NULL);
        INT3
    }
    for (int i = 0; i < maxthreads; ++i) {
        if (sem_post(threadsem[i]) != 0) {
            fprintf(stderr, "sem_post on threadsem[%02d] failed: ", i);
            perror(NULL);
            INT3
            if ((errno = pthread_cancel(thread[i])) != 0) {
                fprintf(stderr, "pthread_cancel of thread %d failed: ", i);
                perror(NULL);
                INT3
            }
        }
        if ((errno = pthread_join(thread[i], NULL)) != 0) {
            fprintf(stderr, "pthread_join of thread %d failed: ", i);
            perror(NULL);
            INT3
            _Exit(EXIT_FAILURE);
        }
    }
    if (sem_close(mastersem) != 0) {
        fprintf(stderr, "sem_close of /mastersem failed: ");
        perror(NULL);
        INT3
    }
    for (int i = 0; i < maxthreads; ++i) {
        if (sem_close(threadsem[i]) != 0) {
            fprintf(stderr, "sem_close of %s failed: ", threadname[i]);
            perror(NULL);
            INT3
        }
    }
}
#endif // !SINGLE_THREAD

int main(int argc, char **argv) {
    (void)argc;
#if !SINGLE_THREAD
    setmaxthreads();
#endif
#if HAS_INFINITE_H
    (void)argv;

    readinfiniteh();
#else
    openinfinitefile(argv[0]);
#endif

#if SINGLE_THREAD
    nxt[0] = malloc(MAXARRAY * sizeof(uint_fast32_t));
    if (nxt[0] == NULL) {
        fprintf(stderr, "malloc(%zu) failed\n", (MAXARRAY * sizeof(uint_fast32_t)));
        INT3
        exit(EXIT_FAILURE);
    }
    cnts[0] = malloc(MAXARRAY * sizeof(uint_fast32_t));
    if (cnts[0] == NULL) {
        fprintf(stderr, "malloc(%zu) failed\n", (MAXARRAY * sizeof(uint_fast32_t)));
        INT3
        exit(EXIT_FAILURE);
    }
    atomic_store(&initdone, 1);
#else
    threadinit();
#endif

#if DOTESTS || DOSEARCH
    char buffer[MAXBUF + 1];
#endif
    
#if DOTESTS
    uint_fast32_t bufferhead;
    uint_fast32_t evalhead;
    uint_fast32_t initlen;
    
    atomic_store(&maxstep, 0);
    atomic_store(&maxlen, 0);
    bufmax[0] = '\0';
    atomic_store(&bufmaxis0, 1);
    bufmaxlen[0] = '\0';
    strncpy(buffer, "S(SKK)(SKK)(S(SKK)(SKK))x", MAXBUF);
    setupfreelist();
    bufferhead = str2cells(buffer);
#if PARANOID
    if (bufferhead == 0) {
        printf("*** Programmer error: unexpected end of cells at %s\n", buffer);
        INT3
        exit(EXIT_FAILURE);
    }
#endif
    initlen = highwatermark - FREEMIN;
#if 0
    for (uint_fast32_t i = FREEMIN; i < highwatermark; ++i) {
        uint_fast32_t curconts = contents[i];
        
        if (curconts < FREEMIN) {
            printf("%" PRIuFAST32 ": contents:%c next:%" PRIuFAST32 "\n", i, curconts, next[i]);
        } else {
            printf("%" PRIuFAST32 ": contents:%" PRIuFAST32 " next:%" PRIuFAST32 "\n", i, curconts, next[i]);
        }
    }
#endif // if 0
    evalhead = clonecells(bufferhead);
#if PARANOID
    if (evalhead == 0) {
        printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", bufferhead);
        INT3
        exit(EXIT_FAILURE);
    }
#endif
    evalcells(13, bufferhead, evalhead, initlen, buffer, 0);
    
    atomic_store(&maxstep, 0);
    atomic_store(&maxlen, 0);
    bufmax[0] = '\0';
    atomic_store(&bufmaxis0, 1);
    bufmaxlen[0] = '\0';
    strncpy(buffer, "SSK(S(K(SS(S(SSK))))K)x", MAXBUF);
    setupfreelist();
    bufferhead = str2cells(buffer);
#if PARANOID
    if (bufferhead == 0) {
        printf("*** Programmer error: unexpected end of cells at %s\n", buffer);
        INT3
        exit(EXIT_FAILURE);
    }
#endif
    initlen = highwatermark - FREEMIN;
    evalhead = clonecells(bufferhead);
#if PARANOID
    if (evalhead == 0) {
        printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", bufferhead);
        INT3
        exit(EXIT_FAILURE);
    }
#endif
    evalcells(11, bufferhead, evalhead, initlen, buffer, 0);
    
    atomic_store(&maxstep, 0);
    atomic_store(&maxlen, 0);
    bufmax[0] = '\0';
    atomic_store(&bufmaxis0, 1);
    bufmaxlen[0] = '\0';
    strncpy(buffer, "SKSx", MAXBUF);
    setupfreelist();
    bufferhead = str2cells(buffer);
#if PARANOID
    if (bufferhead == 0) {
        printf("*** Programmer error: unexpected end of cells at %s\n", buffer);
        INT3
        exit(EXIT_FAILURE);
    }
#endif
    initlen = highwatermark - FREEMIN;
    evalhead = clonecells(bufferhead);
#if PARANOID
    if (evalhead == 0) {
        printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", bufferhead);
        INT3
        exit(EXIT_FAILURE);
    }
#endif
    evalcells(2, bufferhead, evalhead, initlen, buffer, 1);
    
    fflush(stdout);
#endif // DOTESTS
    
#if DOSEARCH
    for (unsigned length = 0; length < MAXLEN; ++length) {
        unsigned lastnum = endnum(length);
        
        printf("\nLength %u:\n", (length + 1));
        maxstr = maxstrtable[length];
        maxsteps = maxsteptable[length];
        atomic_store(&maxstep, 0);
        atomic_store(&maxlen, 0);
        bufmax[0] = '\0';
        atomic_store(&bufmaxis0, 1);
        bufmaxlen[0] = '\0';
        atomic_store(&repeatcount, 0);
        atomic_store(&nevercount, 0);
        atomic_store(&checked, 0);
        for (unsigned nextnum = startnum(length); nextnum <= lastnum; nextnum += 4) {
            if (popcount32(nextnum) != length) {
                continue;
            }
            if (earlyend(nextnum, length)) {
                continue;
            }
            
#if 0
            buffer[0] = '\0';
            num2binary(nextnum);
            puts(buffer);
            buffer[0] = '\0';
            num2fps(nextnum, (2 * length));
            puts(buffer);
#endif
            generateallSK(length, nextnum, buffer);
        }
#if !SINGLE_THREAD
        // wait for all threads to finish
        // check threadempty first to make sure no thread is still working
        while (atomic_load(&threadempty) != allthreadsmask) {
            sched_yield();
        }
        // then make sure all threads are waiting
        while (atomic_load(&threadwaiting) != allthreadsmask) {
            sched_yield();
        }
#endif
#if PRINTMAXES
        printf("\rChecked:%" PRIuFAST32 "                                     \n",
               atomic_load(&checked));
        printf("Maximum steps:%" PRIuFAST32 " with %s\n", atomic_load(&maxstep), bufmax);
        printf("Maximum cell count:");
        printf("%" PRIuFAST32 " with %s", atomic_load(&maxlen), bufmaxlen);
        
        uint_fast32_t nevcnt = atomic_load(&nevercount);
        
        if (nevcnt) {
            printf("\nNever ends:%" PRIuFAST32 "", nevcnt);
        }
        
        uint_fast32_t rptcnt = atomic_load(&repeatcount);
        
        if (rptcnt) {
            if (nevcnt) {
                putchar(' ');
            } else {
                putchar('\n');
            }
            printf("Repeats forever:%" PRIuFAST32 "", rptcnt);
        }
        putchar('\n');
        printf("Total new infinites:%" PRIuFAST32,
               atomic_load(&totalinfinitecount));
        if (neverendingset.keys != NULL) {
            printf(" Infinite matches:%" PRIuFAST32,
                   atomic_load(&neverendsmatch));
        }
        putchar('\n');
#endif // PRINTMAXES
        fflush(stdout);
    }
#endif // DOSEARCH
#if !SINGLE_THREAD
    threadfinal();
#endif
#if HAS_INFINITE_H
    int result = EXIT_SUCCESS;
#else
    int result = closeinfinitefile();
#endif

    freememopath();
    if ((result == EXIT_SUCCESS) && ISATTY(FILENO(stdin))) {
        puts("\nPress any key to exit");
        getchar();
    }
    return result;
}
