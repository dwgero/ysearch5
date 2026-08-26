/*
 * makeinfh.c
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

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define PACKED_KEY_LENGTH_BITS 6U
#define PACKED_KEY_LENGTH_SHIFT (64U - PACKED_KEY_LENGTH_BITS)
#define PACKED_KEY_PAYLOAD_BITS PACKED_KEY_LENGTH_SHIFT
#define PACKED_KEY_PAYLOAD_MASK (UINT64_MAX >> PACKED_KEY_LENGTH_BITS)
#define PACKED_KEY_TOKEN_BITS 2U
#define PACKED_TOKEN_APPLICATION 0U
#define PACKED_TOKEN_S 1U
#define PACKED_TOKEN_K 2U
#define PACKED_LEAF_KEY(token) \
    ((((uint64_t)PACKED_KEY_TOKEN_BITS) << PACKED_KEY_LENGTH_SHIFT) | \
     (uint64_t)(token))
#define PACKED_KEY_S PACKED_LEAF_KEY(PACKED_TOKEN_S)
#define PACKED_KEY_K PACKED_LEAF_KEY(PACKED_TOKEN_K)
#define CATALOG_LINE_CAPACITY 128U
#define INFINITE_KEY_CAPACITY 1048576U

_Static_assert((INFINITE_KEY_CAPACITY & (INFINITE_KEY_CAPACITY - 1U)) == 0,
               "infinite-key capacity must be a power of two");

typedef struct {
    uint64_t *keys;
    size_t count;
    size_t capacity;
} KeyArray;

_Noreturn static void fatal(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
}

_Noreturn static void syserror(const char *operation, const char *path,
                               int error)
{
    fprintf(stderr, "%s %s failed: %s\n", operation, path,
            strerror(error ? error : EIO));
    exit(EXIT_FAILURE);
}

static char *getexecutablepath(const char *argv0)
{
    (void)argv0;

#if defined(__APPLE__)
    uint32_t capacity = 1024U;
    char *path = malloc(capacity);

    if (path == NULL) fatal("failed to allocate executable path");
    for (;;) {
        uint32_t required = capacity;

        if (_NSGetExecutablePath(path, &required) == 0) break;
        char *larger = realloc(path, required);

        if (larger == NULL) {
            free(path);
            fatal("failed to allocate executable path");
        }
        path = larger;
        capacity = required;
    }
    char *resolved = realpath(path, NULL);

    if (resolved == NULL) {
        int error = errno;

        fprintf(stderr, "failed to resolve executable path %s: %s\n",
                path, strerror(error));
        free(path);
        exit(EXIT_FAILURE);
    }
    free(path);
    return resolved;
#elif defined(_WIN32) || defined(_WIN64)
    DWORD capacity = 256U;

    for (;;) {
        char *path = malloc((size_t)capacity);

        if (path == NULL) fatal("failed to allocate executable path");
        DWORD length = GetModuleFileNameA(NULL, path, capacity);

        if (length == 0) {
            DWORD error = GetLastError();

            free(path);
            fprintf(stderr, "GetModuleFileNameA failed with error %lu\n",
                    (unsigned long)error);
            exit(EXIT_FAILURE);
        }
        if (length < capacity) return path;
        free(path);
        if (capacity > (UINT32_MAX / 2U)) {
            fatal("executable path is too long");
        }
        capacity *= 2U;
    }
#elif defined(__linux__)
    size_t capacity = 256U;

    for (;;) {
        char *path = malloc(capacity + 1U);

        if (path == NULL) fatal("failed to allocate executable path");
        ssize_t length = readlink("/proc/self/exe", path, capacity);

        if (length < 0) {
            int error = errno;

            free(path);
            fprintf(stderr, "failed to read executable path: %s\n",
                    strerror(error));
            exit(EXIT_FAILURE);
        }
        if ((size_t)length < capacity) {
            path[length] = '\0';
            return path;
        }
        free(path);
        if (capacity > (SIZE_MAX / 2U)) {
            fatal("executable path is too long");
        }
        capacity *= 2U;
    }
#else
    if ((argv0 == NULL) || (*argv0 == '\0')) {
        fatal("cannot determine executable path");
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

static char *adjacentpath(const char *executable, const char *filename)
{
    const char *separator = strrchr(executable, '/');

#if defined(_WIN32) || defined(_WIN64)
    const char *backslash = strrchr(executable, '\\');

    if ((backslash != NULL) &&
        ((separator == NULL) || (backslash > separator))) {
        separator = backslash;
    }
#endif
    size_t directorylength = (separator == NULL)
        ? 0U
        : (size_t)(separator - executable) + 1U;
    size_t filenamelength = strlen(filename) + 1U;

    if (directorylength > (SIZE_MAX - filenamelength)) {
        fatal("adjacent file path is too long");
    }
    char *path = malloc(directorylength + filenamelength);

    if (path == NULL) fatal("failed to allocate adjacent file path");
    memcpy(path, executable, directorylength);
    memcpy(path + directorylength, filename, filenamelength);
    return path;
}

_Noreturn static void invalidline(const char *inputpath, size_t linenumber,
                                  const char *message)
{
    fprintf(stderr, "invalid %s line %zu: %s\n", inputpath, linenumber,
            message);
    exit(EXIT_FAILURE);
}

static int hexvalue(unsigned char character)
{
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

static unsigned packedkeybits(uint64_t key)
{
    return (unsigned)(key >> PACKED_KEY_LENGTH_SHIFT);
}

static uint64_t packedapplicationkey(uint64_t left, uint64_t right,
                                     const char *inputpath,
                                     size_t linenumber)
{
    unsigned rightbits = packedkeybits(right);
    unsigned resultbits = PACKED_KEY_TOKEN_BITS +
        packedkeybits(left) + rightbits;

    if (resultbits > PACKED_KEY_PAYLOAD_BITS) {
        invalidline(inputpath, linenumber,
                    "expression exceeds packed-key capacity");
    }
    uint64_t payload =
        ((uint64_t)PACKED_TOKEN_APPLICATION <<
         (packedkeybits(left) + rightbits)) |
        ((left & PACKED_KEY_PAYLOAD_MASK) << rightbits) |
        (right & PACKED_KEY_PAYLOAD_MASK);

    return ((uint64_t)resultbits << PACKED_KEY_LENGTH_SHIFT) | payload;
}

static uint64_t parseexpression(const char **position,
                                const char *inputpath, size_t linenumber)
{
    uint64_t result = 0;

    while ((**position != '\0') && (**position != ')')) {
        uint64_t term;

        if (**position == 'S') {
            term = PACKED_KEY_S;
            (*position)++;
        } else if (**position == 'K') {
            term = PACKED_KEY_K;
            (*position)++;
        } else if (**position == '(') {
            (*position)++;
            term = parseexpression(position, inputpath, linenumber);
            if (**position != ')') {
                invalidline(inputpath, linenumber,
                            "missing closing parenthesis");
            }
            (*position)++;
        } else {
            invalidline(inputpath, linenumber,
                        "expression contains an invalid character");
        }
        result = (result == 0)
            ? term
            : packedapplicationkey(result, term, inputpath, linenumber);
    }
    if (result == 0) {
        invalidline(inputpath, linenumber, "expression is empty");
    }
    return result;
}

static void appendkey(KeyArray *array, uint64_t key)
{
    if (array->count == array->capacity) {
        size_t newcapacity = array->capacity ? array->capacity * 2U : 1024U;

        if ((newcapacity < array->capacity) ||
            (newcapacity > (SIZE_MAX / sizeof(*array->keys)))) {
            fatal("packed-key array is too large");
        }
        uint64_t *larger = realloc(array->keys,
                                   newcapacity * sizeof(*larger));

        if (larger == NULL) fatal("failed to grow packed-key array");
        array->keys = larger;
        array->capacity = newcapacity;
    }
    array->keys[array->count++] = key;
}

static int parseline(char *line, const char *inputpath, size_t linenumber,
                     uint64_t *key)
{
    size_t length = strlen(line);

    if ((length == 0) || (line[length - 1U] != '\n')) {
        invalidline(inputpath, linenumber,
                    "line is too long or lacks a final newline");
    }
    line[--length] = '\0';
    if ((length != 0) && (line[length - 1U] == '\r')) {
        line[--length] = '\0';
    }
    const unsigned char *first = (const unsigned char *)line;

    while ((*first != '\0') && isspace(*first)) first++;
    if (*first == '*') return 0;
    if ((length <= 20U) || (line[0] != '0') || (line[1] != 'x') ||
        (line[18] != ':') || (line[19] != ' ')) {
        invalidline(inputpath, linenumber,
                    "expected 0x plus 16 hex digits, colon, and expression");
    }
    uint64_t recordedkey = 0;

    for (size_t i = 2U; i < 18U; ++i) {
        int digit = hexvalue((unsigned char)line[i]);

        if (digit < 0) {
            invalidline(inputpath, linenumber,
                        "packed key contains a non-hex digit");
        }
        recordedkey = (recordedkey << 4) | (uint64_t)(unsigned)digit;
    }
    const char *position = line + 20;
    uint64_t expressionkey = parseexpression(&position, inputpath,
                                              linenumber);

    if (*position != '\0') {
        invalidline(inputpath, linenumber,
                    "expression has an unmatched parenthesis");
    }
    if (recordedkey != expressionkey) {
        invalidline(inputpath, linenumber,
                    "packed key does not match expression");
    }
    *key = recordedkey;
    return 1;
}

static KeyArray readkeys(const char *inputpath)
{
    errno = 0;
    FILE *input = fopen(inputpath, "r");

    if (input == NULL) syserror("open", inputpath, errno);
    KeyArray result = { 0 };
    char line[CATALOG_LINE_CAPACITY];
    size_t linenumber = 0;

    for (;;) {
        errno = 0;
        if (fgets(line, (int)sizeof(line), input) == NULL) {
            if (ferror(input)) {
                int error = errno;

                (void)fclose(input);
                syserror("read", inputpath, error);
            }
            break;
        }
        linenumber++;
        uint64_t key;

        if (parseline(line, inputpath, linenumber, &key)) {
            appendkey(&result, key);
        }
    }
    errno = 0;
    if (fclose(input) != 0) {
        syserror("close", inputpath, errno);
    }
    if (result.count == 0) {
        fatal("infinite.cmb contains no packed keys");
    }
    return result;
}

static int comparekeys(const void *first, const void *second)
{
    uint64_t left = *(const uint64_t *)first;
    uint64_t right = *(const uint64_t *)second;

    return (left > right) - (left < right);
}

static void sortanddeduplicate(KeyArray *array)
{
    qsort(array->keys, array->count, sizeof(*array->keys), comparekeys);
    size_t unique = 1U;

    for (size_t i = 1U; i < array->count; ++i) {
        if (array->keys[i] != array->keys[unique - 1U]) {
            array->keys[unique++] = array->keys[i];
        }
    }
    array->count = unique;
}

static size_t packedkeyhash(uint64_t key, size_t mask)
{
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    key *= UINT64_C(0xc4ceb9fe1a85ec53);
    key ^= key >> 33;
    return (size_t)key & mask;
}

static uint64_t *buildtable(const KeyArray *array)
{
    if (array->count > (INFINITE_KEY_CAPACITY / 2U)) {
        fprintf(stderr,
                "infinite.cmb contains %zu unique keys; the %u-slot table "
                "holds at most %u\n",
                array->count, INFINITE_KEY_CAPACITY,
                INFINITE_KEY_CAPACITY / 2U);
        exit(EXIT_FAILURE);
    }
    uint64_t *table = calloc(INFINITE_KEY_CAPACITY, sizeof(*table));

    if (table == NULL) fatal("failed to allocate infinite-key hash table");
    size_t mask = INFINITE_KEY_CAPACITY - 1U;

    for (size_t i = 0; i < array->count; ++i) {
        uint64_t key = array->keys[i];
        size_t index = packedkeyhash(key, mask);

        while (table[index] != 0) {
            index = (index + 1U) & mask;
        }
        table[index] = key;
    }
    return table;
}

static void writeheader(const char *outputpath, const char *temppath,
                        const KeyArray *array, const uint64_t *table)
{
    errno = 0;
    FILE *output = fopen(temppath, "w");

    if (output == NULL) syserror("open", temppath, errno);
    errno = 0;
    int failed = fprintf(output,
        "/* Generated by makeinfh from infinite.cmb. Do not edit. */\n"
        "/*\n"
        "* infinite.h\n"
        "* Part of ysearch5\n"
        "* Copyright (C) 2026 by David W. Gero\n"
        "*\n"
        "* This program is free software: you can redistribute it and/or modify\n"
        "* it under the terms of the GNU General Public License as published by\n"
        "* the Free Software Foundation, either version 3 of the License, or\n"
        "* (at your option) any later version.\n"
        "*\n"
        "* This program is distributed in the hope that it will be useful,\n"
        "* but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "* GNU General Public License for more details.\n"
        "*\n"
        "* You should have received a copy of the GNU General Public License\n"
        "* along with this program.  If not, see <http://www.gnu.org/licenses/>.\n"
        "*/\n"
        "#ifndef __INFINITE_H_\n"
        "#define __INFINITE_H_\n\n"
        "#include <stdint.h>\n\n"
        "#define INFINITE_KEY_CAPACITY %uU\n"
        "#define INFINITE_KEY_COUNT %zuU\n\n"
        "static const uint64_t infinite_keys[INFINITE_KEY_CAPACITY] = {\n",
        INFINITE_KEY_CAPACITY, array->count) < 0;

    for (size_t i = 0; (i < INFINITE_KEY_CAPACITY) && !failed; ++i) {
        if (table[i] == 0) continue;
        failed = fprintf(output,
                         "    [%zu] = UINT64_C(0x%016" PRIx64 "),\n",
                         i, table[i]) < 0;
    }
    if (!failed) {
        failed = fprintf(output,
            "};\n\n"
            "#endif /* __INFINITE_H_ */\n") < 0;
    }
    int writeerror = errno ? errno : EIO;

    errno = 0;
    if (fclose(output) != 0) {
        if (!failed) writeerror = errno ? errno : EIO;
        failed = 1;
    }
    if (failed) {
        (void)remove(temppath);
        syserror("write", outputpath, writeerror);
    }

#if defined(_WIN32) || defined(_WIN64)
    if (!MoveFileExA(temppath, outputpath,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();

        (void)remove(temppath);
        fprintf(stderr, "replace %s failed with error %lu\n", outputpath,
                (unsigned long)error);
        exit(EXIT_FAILURE);
    }
#else
    errno = 0;
    if (rename(temppath, outputpath) != 0) {
        int error = errno;

        (void)remove(temppath);
        syserror("replace", outputpath, error);
    }
#endif
}

int main(int argc, char **argv)
{
    (void)argc;
    char *executable = getexecutablepath(argv[0]);
    char *inputpath = adjacentpath(executable, "infinite.cmb");
    char *outputpath = adjacentpath(executable, "infinite.h");
    char *temppath = adjacentpath(executable, "infinite.h.tmp");

    free(executable);
    KeyArray keys = readkeys(inputpath);

    sortanddeduplicate(&keys);
    uint64_t *table = buildtable(&keys);

    writeheader(outputpath, temppath, &keys, table);
    printf("wrote %zu packed keys in %u slots to %s\n",
           keys.count, INFINITE_KEY_CAPACITY, outputpath);
    free(table);
    free(keys.keys);
    free(temppath);
    free(outputpath);
    free(inputpath);
    return EXIT_SUCCESS;
}
