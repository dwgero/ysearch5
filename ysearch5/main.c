//
//  ysearch5
//
//  Created by David W. Gero on 8/22/26.
//  Copyright (C) 2026 by David W. Gero.  All Rights Reserved.
//

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

//#define MAXLEN 11
#define MAXLEN 10
#define MAXBUF ((3 * MAXLEN) + 2)
#define MAXSTR maxstr
#define MAXSTEPS maxsteps
#define MAXARRAY 33554432

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

#if SINGLE_THREAD
    #define MAXTHREADS 1
#else
    // maximum 64, suggested is (number of hardware CPUs - 1)
    #define MAXTHREADS 15
#endif

_Static_assert(MAXTHREADS >= 1 && MAXTHREADS <= 64,
               "MAXTHREADS must be between 1 and 64");
#define ALL_THREADS_MASK \
    (UINT64_MAX >> (64 - MAXTHREADS))

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
    // 7     8       9      10       11
    49152, 98304, 196608, 786432, 25165824
};

uint_fast32_t maxsteptable[12] = {
    // 0    1     2     3     4     5     6
    1024, 1024, 1024, 1024, 1024, 1024, 1024,
    // 7    8     9     10      11
    2048, 4096, 8192, 32768, 1572864
};

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

// rather than refactoring, just redefine
#if SINGLE_THREAD
    #define next nxt[0]
    #define contents cnts[0]
    #define highwatermark hwm[0]
    #define freelist frls[0]
    #define buflen bfln[0]
    static uint_fast32_t peakbuflen;
#else
    PERTHREAD static int myid;
    #define next nxt[myid]
    #define contents cnts[myid]
    #define highwatermark hwm[myid]
    #define freelist frls[myid]
    #define buflen bfln[myid]
    PERTHREAD static uint_fast32_t peakbuflen;
#endif

uint_fast32_t *nxt[MAXTHREADS];
uint_fast32_t *cnts[MAXTHREADS];
// An expression pointer identifies its tail cell; next[tail] identifies its
// head cell, making the expression a circular ring. If contents < FREEMIN,
// it is a character; otherwise it identifies the tail of a subexpression.
#define FREEMIN 256
uint_fast32_t hwm[MAXTHREADS];
uint_fast32_t frls[MAXTHREADS];
uint_fast32_t bfln[MAXTHREADS];

_Atomic(uint_fast32_t) repeatcount = 0;
_Atomic(uint_fast32_t) nevercount = 0;
_Atomic(uint_fast32_t) checked = 0;
atomic_int initdone = 0;

#if !SINGLE_THREAD
pthread_t thread[MAXTHREADS];
sem_t *threadsem[MAXTHREADS];
sem_t *mastersem;
_Atomic(uint64_t) threadempty = ALL_THREADS_MASK;
_Atomic(uint64_t) threadwaiting = 0;
atomic_int exiting = 0;
atomic_int masterwaiting = 0;
char workbuf[MAXTHREADS][MAXBUF + 1];
unsigned worklen[MAXTHREADS];
uint_fast32_t worknum[MAXTHREADS];
unsigned workcount[MAXTHREADS];
char threadname[MAXTHREADS][32];
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
    buflen = 0;
    peakbuflen = 0;
}

uint_fast32_t getfree(void) {
    uint_fast32_t temp = freelist;

    if (temp) {
        freelist = next[temp];
    } else {
        if (highwatermark >= maxstr) {
            return 0;
        }
        temp = highwatermark++;
    }

    buflen += 1;
    if (peakbuflen < buflen) {
        peakbuflen = buflen;
    }
    return temp;
}

void putfree(uint_fast32_t cell) {
    next[cell] = freelist;
    freelist = cell;
    buflen -= 1;
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

        if (contents[cells] >= FREEMIN) {
            freeall(contents[cells]);
        }
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

uint_fast32_t str2cells(char *buffer) {
    uint_fast32_t str2cellspos = 0;
    
    return str2cell(0, buffer, &str2cellspos);
}

void printcells(uint_fast32_t startcell) {
    uint_fast32_t tail = startcell;
    uint_fast32_t cells = next[tail];
    uint_fast32_t curconts;
    
#if PARANOID
    if (cells == 0) {
        printf("*** Programmer error: unexpected end of cells during print at %" PRIuFAST32 "\n", startcell);
        INT3
        return;
    }
#endif
    //if (cells == tail) {
    //    printf("*** Programmer error: only one item in cells at %" PRIuFAST32 "\n", startcell);
    //    return;
    //}
    curconts = contents[cells];
#if PARANOID
    if (curconts >= FREEMIN) {
        printf("*** Programmer error: first item of cells is subcells at %" PRIuFAST32 "\n", startcell);
        INT3
        return;
    }
#endif
    putchar((unsigned char)curconts);
    for (;;) {
        if (cells == tail) {
            return;
        }
        cells = next[cells];
#if PARANOID
        if (cells == 0) {
            printf("*** Programmer error: unexpected end of cells during print at %" PRIuFAST32 "\n", startcell);
            INT3
            return;
        }
#endif
        curconts = contents[cells];
        if (curconts < FREEMIN) {
            putchar((unsigned char)curconts);
        } else {
            putchar('(');
            printcells(curconts);
            putchar(')');
        }
    }
}

void putcells(uint_fast32_t cells) {
    printcells(cells);
    putchar('\n');
}

void putcontents(uint_fast32_t conts) {
    if (conts < FREEMIN) {
        putchar((unsigned char)conts);
        putchar('\n');
        return;
    }
    putcells(conts);
}

// cells2str unused
# if 0
char strbuf[MAXARRAY + 1];
int cells2strpos = 0;

void cell2str(uint_fast32_t startcell) {
    uint_fast32_t tail = startcell;
    uint_fast32_t cells = next[tail];
    uint_fast32_t curconts;
    
#if PARANOID
    if (cells == 0) {
        printf("*** Programmer error: unexpected end of cells during conversion at %" PRIuFAST32 "\n", startcell);
        INT3
        return;
    }
#endif
    //if (cells == tail) {
    //    printf("*** Programmer error: only one item in cells at %" PRIuFAST32 "\n", startcell);
    //    return;
    //}
    curconts = contents[cells];
#if PARANOID
    if (curconts >= FREEMIN) {
        printf("*** Programmer error: first item of cells is subcells at %" PRIuFAST32 "\n", startcell);
        INT3
        return;
    }
#endif
    strbuf[cells2strpos++] = (unsigned char)curconts;
    for (;;) {
        if (cells == tail) {
            return;
        }
        cells = next[cells];
#if PARANOID
        if (cells == 0) {
            printf("*** Programmer error: unexpected end of cells during conversion at %" PRIuFAST32 "\n", startcell);
            INT3
            return;
        }
#endif
        curconts = contents[cells];
        if (curconts < FREEMIN) {
            strbuf[cells2strpos++] = (unsigned char)curconts;
        } else {
            strbuf[cells2strpos++] = '(';
            cell2str(curconts);
            strbuf[cells2strpos++] = ')';
        }
    }
}

void cells2str(uint_fast32_t cells) {
    cells2strpos = 0;
    cell2str(cells);
    strbuf[cells2strpos] = '\0';
}
#endif

uint_fast32_t clonecells(uint_fast32_t startcell) {
    uint_fast32_t tail = startcell;
    uint_fast32_t cells = next[tail];
    uint_fast32_t curconts;
    uint_fast32_t newhead;
    uint_fast32_t newtail;
    
#if PARANOID
    if (cells == 0) {
        printf("*** Programmer error: unexpected end of cells during clone at %" PRIuFAST32 "\n", startcell);
        INT3
        return 0;
    }
#endif
    //if (cells == tail) {
    //    printf("*** Programmer error: only one item in cells at %" PRIuFAST32 "\n", startcell);
    //    return 0;
    //}
    curconts = contents[cells];
#if PARANOID
    if (curconts >= FREEMIN) {
        printf("*** Programmer error: first item of cells is subcells at %" PRIuFAST32 "\n", startcell);
        INT3
        return 0;
    }
#endif
    {
        uint_fast32_t temp = getfree();
        
        if (temp == 0) {
            return 0;
        }
        contents[temp] = curconts;
        newhead = temp;
        newtail = temp;
    }
    for (;;) {
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
        curconts = contents[cells];
        if (curconts >= FREEMIN) {
            curconts = clonecells(curconts);
            if (curconts == 0) {
                return 0;
            }
        }
        {
            uint_fast32_t temp = getfree();
            
            if (temp == 0) {
                return 0;
            }
            contents[temp] = curconts;
            next[newtail] = temp;
            newtail = temp;
        }
    }
}

uint_fast32_t clonecontents(uint_fast32_t conts) {
    if (conts < FREEMIN) {
        return conts;
    }
    return clonecells(conts);
}

// startcells1 is always bufferhead if toplevel == 1
int equalcells(uint_fast32_t startcells1, uint_fast32_t startcells2, int toplevel) {
    uint_fast32_t tail1 = startcells1;
    uint_fast32_t tail2 = startcells2;
    uint_fast32_t cells1 = next[tail1];
    uint_fast32_t cells2 = next[tail2];
    uint_fast32_t curconts1, curconts2;
    
#if PARANOID
    if (cells1 == 0) {
        printf("*** Programmer error: unexpected end of first comparison at %" PRIuFAST32 "\n", startcells1);
        INT3
        return 0;
    }
    if (cells2 == 0) {
        printf("*** Programmer error: unexpected end of second comparison at %" PRIuFAST32 "\n", startcells2);
        INT3
        return 0;
    }
#endif
    curconts1 = contents[cells1];
    curconts2 = contents[cells2];
#if PARANOID
    if (curconts1 >= FREEMIN) {
        printf("*** Programmer error: first item of cells is subcells at %" PRIuFAST32 "\n", startcells1);
        INT3
        return 0;
    }
    if (curconts2 >= FREEMIN) {
        printf("*** Programmer error: first item of cells is subcells at %" PRIuFAST32 "\n", startcells2);
        INT3
        return 0;
    }
#endif
    if (curconts1 != curconts2) {
        return 0;
    }
    for (;;) {
        if (cells1 == tail1) {
            // reached end of bufferhead if toplevel == 1
            if (cells2 == tail2) {
                // total match
                return 1;
            }
            // match sofar to cells2, but more of cells2 left
            if (toplevel) {
                return 2;
            }
            return 0;
        }
        if (cells2 == tail2) {
            return 0;
        }
        cells1 = next[cells1];
        cells2 = next[cells2];
#if PARANOID
        if (cells1 == 0) {
            printf("*** Programmer error: unexpected end of first comparison at %" PRIuFAST32 "\n", startcells1);
            INT3
            return 0;
        }
        if (cells2 == 0) {
            printf("*** Programmer error: unexpected end of comparison %" PRIuFAST32
                   " against %" PRIuFAST32 "\n", startcells1, startcells2);
            INT3
            return 0;
        }
#endif
        curconts1 = contents[cells1];
        curconts2 = contents[cells2];
        if (curconts1 < FREEMIN) {
            if (curconts1 != curconts2) {
                if (toplevel && (curconts1 == 'x') && (cells1 == tail1)) {
                    return 2;
                }
                return 0;
            }
        } else {
            if (curconts2 < FREEMIN) {
                return 0;
            }
            if (equalcells(curconts1, curconts2, 0) == 0) {
                return 0;
            }
        }
    }
}

#define INITIAL_CAPACITY 100000 // 50% load factor for 50k items

typedef struct {
    char **keys;
    uint32_t capacity;
    uint32_t size;
} HashSet;

uint32_t fnv1a_hash(const char *str, uint32_t capacity) {
    uint32_t hash = 2166136261U;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619U;
    }
    return hash % capacity;
}

HashSet *set_create(uint32_t capacity) {
    HashSet *set = malloc(sizeof(HashSet));
    if (!set) return NULL;
    set->keys = calloc(capacity, sizeof(char*));
    if (!set->keys) { free(set); return NULL; }
    set->capacity = capacity;
    set->size = 0;
    return set;
}

HashSet *neverends;

int set_add(HashSet *set, const char *key) {
    if (set->size >= set->capacity / 2) {
        fprintf(stderr,
                "never-ending expression set reached its maximum size of %" PRIu32 " entries\n",
                set->capacity / 2);
        exit(EXIT_FAILURE);
    }

    uint32_t index = fnv1a_hash(key, set->capacity);
    while (set->keys[index] != NULL) {
        if (strcmp(set->keys[index], key) == 0) return 1; // Already exists
        index = (index + 1) % set->capacity;
    }

    set->keys[index] = strdup(key);
    if (set->keys[index] == NULL) {
        fprintf(stderr, "strdup failed while adding to never-ending expression set\n");
        exit(EXIT_FAILURE);
    }
    set->size++;
    return 1;
}

int set_contains(HashSet *set, const char *key) {
    uint32_t index = fnv1a_hash(key, set->capacity);
    uint32_t start = index;

    while (set->keys[index] != NULL) {
        if (strcmp(set->keys[index], key) == 0) return 1;
        index = (index + 1) % set->capacity;
        if (index == start) break;
    }
    return 0;
}

void set_destroy(HashSet *set) {
    for (uint32_t i = 0; i < set->capacity; i++) {
        if (set->keys[i]) free(set->keys[i]);
    }
    free(set->keys);
    free(set);
}

_Atomic(uint_fast32_t) neverendscount = 0;
_Atomic(uint_fast32_t) neverendsmatch = 0;
pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;

void addtoneverends(char *buffer) {
    size_t bufferlen;
    char last;
    
    bufferlen = strlen(buffer);
    last = buffer[--bufferlen];
    buffer[bufferlen] = '\0';
    pthread_rwlock_wrlock(&rwlock);
    set_add(neverends, buffer);
    pthread_rwlock_unlock(&rwlock);
    buffer[bufferlen] = last;
    atomic_fetch_add(&neverendscount, 1);
}

uint_fast32_t matchingrightparen(uint_fast32_t position, char *buffer) {
    uint_fast32_t parencount = 1;
    
    for (;;) {
        char curchar = buffer[++position];
        
        if (curchar == ')') {
            if (--parencount == 0) {
                return position;
            }
        } else if (curchar == '(') {
            parencount += 1;
        } else if (curchar == '\0') {
            printf("*** Programmer error: missing right paren\n");
            INT3
            return position;
        }
    }
}

int checkforneverends(size_t bufferlen, char *buffer, uint_fast32_t position) {
    int result = 0;
    char last;
    
    pthread_rwlock_rdlock(&rwlock);
    do {
        last = buffer[position];
        buffer[position] = '\0';
        if (set_contains(neverends, buffer)) {
            buffer[position] = last;
            result = 1;
            atomic_fetch_add(&neverendsmatch, 1);
            break;
        }
        buffer[position] = last;
        if (last == '(') {
            position = matchingrightparen(position, buffer);
        }
        position += 1;
    } while (position < bufferlen);
    pthread_rwlock_unlock(&rwlock);
    return result;
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
        uint_fast32_t x, y, z;
        uint_fast32_t xhead, yhead, zhead;
        uint_fast32_t xtail, ytail, ztail;
        uint_fast32_t rest;
        uint_fast32_t curchar;
        
        cells = head;
#if PARANOID
        if (cells == 0) {
            RESTOREHEAD
            printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", evalhead);
            INT3
            return;
        }
#endif
        curconts = contents[cells];
#if PARANOID
        if (curconts >= FREEMIN) {
            RESTOREHEAD
            printf("*** Programmer error: first item of cells is subcells at %" PRIuFAST32 "\n", evalhead);
            INT3
            return;
        }
#endif
        curchar = curconts;
        if (curchar == 'S') {
            int gotSK = 0;
            int gotK = 0;
            uint_fast32_t needtofreex = 0;
            uint_fast32_t savey;
            
            if (cells == tail) {
                break;
            }
            cells = next[cells];
#if PARANOID
            if (cells == 0) {
                RESTOREHEAD
                printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", evalhead);
                INT3
                return;
            }
#endif
            // x
            xhead = xtail = cells;
            x = contents[cells];
            if (cells == tail) {
                break;
            }
            cells = next[cells];
#if PARANOID
            if (cells == 0) {
                RESTOREHEAD
                printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", evalhead);
                INT3
                return;
            }
#endif
            // y
            yhead = ytail = cells;
            y = contents[cells];
            if (cells == tail) {
                break;
            }
            cells = next[cells];
#if PARANOID
            if (cells == 0) {
                RESTOREHEAD
                printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", evalhead);
                INT3
                return;
            }
#endif
            // z
            zhead = ztail = cells;
            z = contents[cells];
            rest = next[cells];
            // got x, y, and z, do S
            if (x >= FREEMIN) {
                needtofreex = xhead;
                xhead = next[x];
                xtail = x;
#if PARANOID
                if (xhead == 0) {
                    RESTOREHEAD
                    printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", x);
                    INT3
                    return;
                }
#endif
                if (xhead == xtail) {
                    RESTOREHEAD
                    printf("*** Programmer error: only one item in cells at %" PRIuFAST32 "\n", x);
                    INT3
                    return;
                }
            }
            savey = yhead;
            if (y >= FREEMIN) {
                uint_fast32_t cury;
                
                yhead = next[y];
                ytail = y;
#if PARANOID
                if (yhead == 0) {
                    RESTOREHEAD
                    printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", y);
                    INT3
                    return;
                }
#endif
                if (yhead == ytail) {
                    RESTOREHEAD
                    printf("*** Programmer error: only one item in cells at %" PRIuFAST32 "\n", y);
                    INT3
                    return;
                }
                cury = yhead;
                curconts = contents[cury];
                if (curconts >= FREEMIN) {
                    RESTOREHEAD
                    printf("*** Programmer error: first item of cells is subcells at %" PRIuFAST32 "\n", y);
                    INT3
                    return;
                }
                curchar = curconts;
                if (curchar == 'K') {
                    cury = next[cury];
#if PARANOID
                    if (cury == 0) {
                        RESTOREHEAD
                        printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", y);
                        INT3
                        return;
                    }
#endif
                    if (cury == ytail) {
                        gotK = 1;
                    }
                } else if (curchar == 'S') {
                    cury = next[cury];
#if PARANOID
                    if (cury == 0) {
                        RESTOREHEAD
                        printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", y);
                        INT3
                        return;
                    }
#endif
                    if (cury != ytail) {
                        curconts = contents[cury];
                        if (curconts < FREEMIN) {
                            curchar = curconts;
                            if (curchar == 'K') {
                                cury = next[cury];
#if PARANOID
                                if (cury == 0) {
                                    RESTOREHEAD
                                    printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", y);
                                    INT3
                                    return;
                                }
#endif
                                if (cury == ytail) {
                                    gotSK = 1;
                                }
                            }
                        }
                    }
                }
            }
            // free up S from head
            putfree(head);
            // copy x without outermost parens
            head = xhead;
            //printf("head:%" PRIuFAST32 "\n", head);
#if 0
            printf("x:");
            putcontents(x);
#endif
            // copy z
            next[xtail] = zhead;
            //printf("xhead:%" PRIuFAST32 " xtail:%" PRIuFAST32 " contents:%" PRIuFAST32 " next:%" PRIuFAST32 "\n",
            //       xhead, xtail, contents[xtail], zhead);
#if 0
            printf("z:");
            if (z < FREEMIN) {
                putcontents(z);
            } else {
                putchar('(');
                printcells(z);
                putchar(')');
                putchar('\n');
            }
#endif
            if (gotSK) {
                // y == (SK[something])
                // (y z) == (z) == z
                // copy clone of z
                uint_fast32_t temp = clonecontents(z);
                
                if (temp == 0) {
                    steps = MAXSTEPS;
                    break;
                }
#if 0
                printf("gotSK, clonecontents(z) at %" PRIuFAST32 ":", temp);
                if (temp < FREEMIN) {
                    putcontents(temp);
                } else {
                    putchar('(');
                    printcells(temp);
                    putchar(')');
                    putchar('\n');
                }
#endif
                // reuse savey
                contents[savey] = temp;
                next[savey] = rest;
                //printf("savey:%" PRIuFAST32 " contents:%" PRIuFAST32 " next:%" PRIuFAST32 "\n", savey, temp, rest);
                next[ztail] = savey;
                //printf("zhead:%" PRIuFAST32 " ztail:%" PRIuFAST32 " contents:%" PRIuFAST32 " next:%" PRIuFAST32 "\n",
                //       zhead, ztail, contents[ztail], savey);
                //printf("rest:%" PRIuFAST32 " tail was:%" PRIuFAST32 "\n", rest, tail);
                if (rest == 0) {
                    tail = savey;
                }
                //printf(" tail now:%" PRIuFAST32 "\n", tail);
                if (y >= FREEMIN) {
                    freeall(y);
                }
                steps += 2;
            } else if (gotK) {
                // y == (KK) or (KS) or (Kx) or (K([something]))
                // (y z) == K or S or x or ([something])
                // remove the (K on the left of y and the ) on the right of y
                uint_fast32_t yK = yhead;
                
#if PARANOID
                if (yK == 0) {
                    RESTOREHEAD
                    printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", y);
                    INT3
                    return;
                }
                if (contents[yK] != 'K') {
                    RESTOREHEAD
                    printf("*** Programmer error: gotK but K is missing at %" PRIuFAST32 "\n", y);
                    INT3
                    return;
                }
#endif
                
                uint_fast32_t temp = next[yK];
                
#if PARANOID
                if (temp == 0) {
                    RESTOREHEAD
                    printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", y);
                    INT3
                    return;
                }
#endif
                curconts = contents[temp];
                if (curconts < FREEMIN) {
                    // K or S or x
                    freeall(y);
                } else {
                    // ([something])
                    putfree(yK);
                    putfree(temp);
                }
                // reuse savey
                contents[savey] = curconts;
                next[ztail] = savey;
                next[savey] = rest;
                if (rest == 0) {
                    tail = savey;
                }
                steps += 1;
            } else {
                // normal (y z)
#if 0
                printf("y:");
                putcontents(y);
#endif
                // copy clone of z
                uint_fast32_t temp = clonecontents(z);
                
                if (temp == 0) {
                    steps = MAXSTEPS;
                    break;
                }
#if 0
                printf("normal (y z), clonecontents(z) at %" PRIuFAST32 ":", temp);
                if (temp < FREEMIN) {
                    putcontents(temp);
                } else {
                    putchar('(');
                    printcells(temp);
                    putchar(')');
                    putchar('\n');
                }
#endif
                // maybe reuse savey
                if (y < FREEMIN) {
                    savey = getfree();
                    if (savey == 0) {
                        steps = MAXSTEPS;
                        break;
                    }
                }
                contents[savey] = temp;
                next[savey] = yhead;
                //printf("savey:%" PRIuFAST32 " contents:%" PRIuFAST32 " next:%" PRIuFAST32 "\n",
                //       savey, temp, yhead);
                next[ytail] = savey;
                //printf("yhead:%" PRIuFAST32 " ytail:%" PRIuFAST32 " contents:%" PRIuFAST32 " next:%" PRIuFAST32 "\n",
                //       yhead, ytail, contents[ytail], savey);
                temp = getfree();
                if (temp == 0) {
                    steps = MAXSTEPS;
                    break;
                }
                next[ztail] = temp;
                //printf("ztail:%" PRIuFAST32 " contents:%" PRIuFAST32 " next:%" PRIuFAST32 "\n",
                //       ztail, contents[ztail], temp);
                contents[temp] = savey;
                next[temp] = rest;
                //printf("temp:%" PRIuFAST32 " contents:%" PRIuFAST32 " next:%" PRIuFAST32 "\n",
                //       temp, savey, rest);
                if (rest == 0) {
                    tail = temp;
                }
#if 0
                return;
#endif
            }
#if 0
            printf("(y z):");
            {
                uint_fast32_t temp = next[ztail];
                
#if PARANOID
                if (temp == 0) {
                    RESTOREHEAD
                    printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", evalhead);
                    INT3
                    return;
                }
#endif
                temp = contents[temp];
                if (temp < FREEMIN) {
                    putcontents(temp);
                } else {
                    putchar('(');
                    printcells(temp);
                    putchar(')');
                    putchar('\n');
                }
            }
#endif
            if (needtofreex) {
                putfree(needtofreex);
            }
        } else if (curchar == 'K') {
            uint_fast32_t needtofreex = 0;
            
            if (cells == tail) {
                break;
            }
            cells = next[cells];
#if PARANOID
            if (cells == 0) {
                RESTOREHEAD
                printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", evalhead);
                INT3
                return;
            }
#endif
            // x
            xhead = xtail = cells;
            x = contents[cells];
            if (cells == tail) {
                break;
            }
            cells = next[cells];
#if PARANOID
            if (cells == 0) {
                RESTOREHEAD
                printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", evalhead);
                INT3
                return;
            }
#endif
            // y
            yhead = cells;
            y = contents[cells];
            rest = next[cells];
            // got x and y, do K
            if (x >= FREEMIN) {
                needtofreex = xhead;
                xhead = next[x];
                xtail = x;
#if PARANOID
                if (xhead == 0) {
                    RESTOREHEAD
                    printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", x);
                    INT3
                    return;
                }
#endif
                if (xhead == xtail) {
                    RESTOREHEAD
                    printf("*** Programmer error: only one item in cells at %" PRIuFAST32 "\n", x);
                    INT3
                    return;
                }
            }
            // free up K from head
            putfree(head);
            // copy x without outermost parens
            head = xhead;
#if 0
            printf("x:");
            putcontents(x);
#endif
            // y isn't copied
            putfree(yhead);
            if (y >= FREEMIN) {
                freeall(y);
            }
            next[xtail] = rest;
            if (rest == 0) {
                tail = xtail;
            }
            if (needtofreex) {
                putfree(needtofreex);
            }
        } else if (curchar == 'x') {
            // x only allowed at head of list
            if (gotx) {
                break;
            }
            if (cells == tail) {
                break;
            }
            cells = next[cells];
#if PARANOID
            if (cells == 0) {
                printf("*** Programmer error: unexpected end of cells at %" PRIuFAST32 "\n", evalhead);
                INT3
                return;
            }
#endif
            if (cells != tail) {
                break;
            }
            curconts = contents[cells];
            if (curconts < FREEMIN) {
                break;
            }
            // restart evaluation at curconts
            next[tail] = head;
            evalhead = tail;
            subowner = cells;
            head = next[curconts];
            tail = curconts;
#if PARANOID
            if (head == tail) {
                printf("*** Programmer error: only one item in cells at %" PRIuFAST32 "\n", evalhead);
                INT3
                return;
            }
#endif
            subhead = curconts;
            gotx = 1;
            if (equalcells(bufferhead, subhead, 0) == 0) {
                next[tail] = 0;
                continue;
            }
        } else {
            puts(buffer);
            putchar('=');
            RESTOREHEAD
            putcells(evalhead);
            fprintf(stderr, "*** Programmer error: not S, K, or x at %" PRIuFAST32 "\n", evalhead);
            INT3
            return;
        }
        RESTOREHEAD
#if 0
        putchar('=');
        putcells(evalhead);
#endif
        steps += 1;
        if (gotx) {
            if (equalcells(bufferhead, subhead, 0)) {
                gotwinner = 1;
                break;
            }
        } else {
            repeatsforever = equalcells(bufferhead, evalhead, 1);
            if (repeatsforever) {
                break;
            }
        }
        // Evaluator mutations use a temporary linear chain. RESTOREHEAD closes
        // the ring for comparison, so reopen it before the next reduction.
        next[tail] = 0;
    }

    uint_fast32_t peakcells = peakbuflen - initlen;

    RESTOREHEAD
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
            addtoneverends(buffer);
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
            addtoneverends(buffer);
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

int numericnodehasKorSK(const NumericNode *nodes, int root,
                        unsigned count, unsigned extraargs) {
    int arguments[MAXLEN + 1];
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
    
    uint_fast32_t initlen = buflen;
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

void generateallSK(unsigned length, uint_fast32_t num, char *buffer) {
    int index[MAXLEN + 1];
    int indexnum = (int)length;
    int maxcount = 1 << length;
    int count;
    size_t bufferlen;
    uint_fast32_t position = 0;
    int skcount = 0;
    NumericNode numericnodes[(2 * MAXLEN) + 1];
    int numericroot = decodenumerictree(length, num, numericnodes);

#if PARANOID
    if (numericroot < 0) {
        return;
    }
#endif

    for (int posit = 0; buffer[posit] != '\0'; ++posit) {
        if (buffer[posit] == '?') {
            index[indexnum--] = posit;
        }
    }
    buffer[0] = 'S';
    strncat(buffer, "x", MAXBUF);
    bufferlen = strlen(buffer);
    {
        size_t bufflen = bufferlen;
        uint_fast32_t rightparen;
        
        // back up from 'x' at end
        // back up one more if not ')'
        if (buffer[--bufflen] != ')') {
            bufflen -= 1;
        }
        while ((position < bufflen) && (skcount < 7)) {
            if (buffer[position] == '(') {
                rightparen = matchingrightparen(position, buffer);
                while (++position < rightparen) {
                    if ((buffer[position] != '(') &&
                        (buffer[position] != ')')) {
                        skcount += 1;
                    }
                }
            } else {
                skcount += 1;
            }
            position += 1;
        }
    }
    for (count = 0; count < maxcount; ++count) {
        for (unsigned i = 0; i < length; ++i) {
            buffer[index[i]] = (count & (1 << i)) ? 'K' : 'S';
        }
        if ((skcount >= 7) && (position < bufferlen)) {
            if (checkforneverends(bufferlen, buffer, position)) {
                continue;
            }
        }
        if (numericnodehasKorSK(numericnodes, numericroot,
                               (unsigned)count, 1)) {
            continue;
        }
        if (((atomic_fetch_add(&checked, 1) + 1) % 1000) == 0) {
            pthread_mutex_lock(&printlock);
            printf("\rChecked:%" PRIuFAST32 " %s                                     ",
                   atomic_load(&checked), buffer);
            fflush(stdout);
            pthread_mutex_unlock(&printlock);
        }
#if SINGLE_THREAD
        dooneSK(length, num, (unsigned)count, buffer);
#else
        uint64_t thmt = atomic_load(&threadempty);
        unsigned threadnum;
        uint64_t mask;
        
        // there is a critical race at this point
        // a thread running here could now turn on a bit in threadwaiting
        // then check that masterwaiting == 0
        // and do a sem_wait(threadsem) without doing a sem_push(mastersem)
        while (thmt == 0) {
            atomic_store(&masterwaiting, 1);
            // to fix the critical race, check threadwaiting again
            // after setting masterwaiting to 1
            thmt = atomic_load(&threadempty);
            if (thmt == 0) {
                // mac os doesn't handle sigaction(SA_RESTART) correctly for sem_wait
                // so, if errno == EINTR, try again
                for (;;) {
                    int sw = sem_wait(mastersem);
                    
                    if (sw == 0) {
                        break;
                    }
                    if (errno != EINTR) {
                        fprintf(stderr, "sem_wait on mastersem failed: ");
                        perror(NULL);
                        INT3
                        exit(EXIT_FAILURE);
                    }
                }
            }
            atomic_store(&masterwaiting, 0);
            // there could have been many sem_post(mastersem)
            // while masterwaiting was 1
            // ignore all the rest
            while ((sem_trywait(mastersem) == 0) || (errno == EINTR)) {
                // nothing to do, keep going
            }
            thmt = atomic_load(&threadempty);
        }
        
        threadnum = ctz64(thmt);
        
#if PARANOID
        if (threadnum >= MAXTHREADS) {
            fprintf(stderr, "*** Programmer error: threadnum == %u >= MAXTHREADS == %d\n",
                    threadnum, MAXTHREADS);
            INT3
            exit(EXIT_FAILURE);
        }
#endif
        
        mask = (uint64_t)1 << threadnum;
        strcpy(workbuf[threadnum], buffer);
        worklen[threadnum] = length;
        worknum[threadnum] = num;
        workcount[threadnum] = (unsigned)count;
        (void)atomic_fetch_and(&threadempty, ~mask);
        
        /*
         * If the producer cleared the waiting bit, it owns the wakeup
         * and must post.
         */
        if (atomic_fetch_and(&threadwaiting, ~mask) & mask) {
            if (sem_post(threadsem[threadnum]) != 0) {
                fprintf(stderr, "sem_post on threadsem[%02u] failed: ", threadnum);
                perror(NULL);
                INT3
                exit(EXIT_FAILURE);
            }
        }
#endif
    }
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
            unsigned mylen = worklen[mythreadnum];
            uint_fast32_t mynum = worknum[mythreadnum];
            unsigned mycount = workcount[mythreadnum];
            char mybuf[MAXBUF+1];
            
            strcpy(mybuf, workbuf[mythreadnum]);
            (void)atomic_fetch_or(&threadempty, mask);
            if (atomic_load(&masterwaiting)) {
                if (sem_post(mastersem) != 0) {
                    fprintf(stderr, "sem_post on mastersem failed: ");
                    perror(NULL);
                    INT3
                    exit(EXIT_FAILURE);
                }
            }
            dooneSK(mylen, mynum, mycount, mybuf);
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
    if ((errno = pthread_attr_setstacksize(&threadattr, (63 * 1024 * 1024)))) {
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
    for (int i = 0; i < MAXTHREADS; ++i) {
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
    for (int i = 0; i < MAXTHREADS; ++i) {
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
        }
    }
    if (sem_close(mastersem) != 0) {
        fprintf(stderr, "sem_close of /mastersem failed: ");
        perror(NULL);
        INT3
    }
    for (int i = 0; i < MAXTHREADS; ++i) {
        if (sem_close(threadsem[i]) != 0) {
            fprintf(stderr, "sem_close of %s failed: ", threadname[i]);
            perror(NULL);
            INT3
        }
    }
}
#endif // !SINGLE_THREAD

int main(void) {
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

    neverends = set_create(INITIAL_CAPACITY);
    if (neverends == NULL) {
        fprintf(stderr,
                "failed to allocate never-ending expression set with capacity %u\n",
                INITIAL_CAPACITY);
        exit(EXIT_FAILURE);
    }
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
    initlen = buflen;
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
    initlen = buflen;
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
    initlen = buflen;
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
    for (unsigned length = 0; length <= MAXLEN; ++length) {
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
            buffer[0] = '\0';
            num2str(nextnum, (int_fast32_t)(2 * length), buffer, 1);
            //puts(buffer);
            generateallSK(length, nextnum, buffer);
        }
#if !SINGLE_THREAD
        // wait for all threads to finish
        // check threadempty first to make sure no thread is still working
        while (atomic_load(&threadempty) != ALL_THREADS_MASK) {
            sched_yield();
        }
        // then make sure all threads are waiting
        while (atomic_load(&threadwaiting) != ALL_THREADS_MASK) {
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
        printf("Total infinites:%" PRIuFAST32 " Infinite matches:%" PRIuFAST32 "\n",
               atomic_load(&neverendscount), atomic_load(&neverendsmatch));
#endif // PRINTMAXES
        fflush(stdout);
    }
#endif // DOSEARCH
#if !SINGLE_THREAD
    threadfinal();
#endif
#if DOTESTS || DOSEARCH
    set_destroy(neverends);
#endif
    if (ISATTY(FILENO(stdin))) {
        puts("\nPress any key to exit");
        getchar();
    }
    return 0;
}
