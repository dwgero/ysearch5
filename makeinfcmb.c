/*
 * makeinfcmb.c
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

#define PACKED_KEY_BITS 64U
#define PACKED_KEY_TOKEN_BITS 2U
#define PACKED_TOKEN_X 0U
#define PACKED_TOKEN_S 1U
#define PACKED_TOKEN_K 2U
#define PACKED_TOKEN_APPLICATION 3U
#define HEADER_LINE_CAPACITY 256U
#define MAX_PACKED_NODES (PACKED_KEY_BITS / PACKED_KEY_TOKEN_BITS)
#define MAX_EXPRESSION_LENGTH (2U * MAX_PACKED_NODES)

typedef struct {
    uint64_t *keys;
    size_t count;
    size_t capacity;
} KeyArray;

typedef struct {
    unsigned token;
    int left;
    int right;
} Node;

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

_Noreturn static void invalidheader(const char *path, size_t line,
                                    const char *message)
{
    fprintf(stderr, "invalid %s line %zu: %s\n", path, line, message);
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

        free(path);
        syserror("resolve", argv0, error);
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
            syserror("read", "/proc/self/exe", error);
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

    if (path == NULL) syserror("resolve", argv0, errno);
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
        ? 0U : (size_t)(separator - executable) + 1U;
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

static void appendkey(KeyArray *array, uint64_t key)
{
    if (array->count == array->capacity) {
        size_t capacity = array->capacity ? array->capacity * 2U : 1024U;

        if ((capacity < array->capacity) ||
            (capacity > (SIZE_MAX / sizeof(*array->keys)))) {
            fatal("packed-key array is too large");
        }
        uint64_t *larger = realloc(array->keys,
                                   capacity * sizeof(*array->keys));

        if (larger == NULL) fatal("failed to grow packed-key array");
        array->keys = larger;
        array->capacity = capacity;
    }
    array->keys[array->count++] = key;
}

static int comparekeys(const void *first, const void *second)
{
    uint64_t a = *(const uint64_t *)first;
    uint64_t b = *(const uint64_t *)second;

    return (a > b) - (a < b);
}

static int parseunsigneddefine(const char *line, const char *name,
                               size_t *value)
{
    char prefix[96];
    int length = snprintf(prefix, sizeof(prefix), "#define %s ", name);

    if ((length < 0) || ((size_t)length >= sizeof(prefix)) ||
        (strncmp(line, prefix, (size_t)length) != 0)) return 0;
    const char *position = line + length;
    char *end = NULL;

    errno = 0;
    uintmax_t parsed = strtoumax(position, &end, 10);
    if ((errno != 0) || (end == position) || (parsed > SIZE_MAX)) return -1;
    if (*end == 'U') ++end;
    if ((*end != '\n') && (*end != '\0')) return -1;
    *value = (size_t)parsed;
    return 1;
}

static int parseencodingdefine(const char *line, const char *path,
                               size_t linenumber, const char *name,
                               size_t expected, int *found)
{
    size_t value;
    int status = parseunsigneddefine(line, name, &value);

    if (status < 0) {
        invalidheader(path, linenumber,
                      "invalid packed-key encoding definition");
    }
    if (status > 0) {
        if (*found) {
            invalidheader(path, linenumber,
                          "duplicate packed-key encoding definition");
        }
        if (value != expected) {
            invalidheader(path, linenumber,
                          "incompatible packed-key encoding definition");
        }
        *found = 1;
    }
    return status;
}

static int parsetableentry(const char *line, size_t *index, uint64_t *key)
{
    static const char entryprefix[] = "] = UINT64_C(0x";
    const char *position = line;

    while (isspace((unsigned char)*position) && (*position != '\n')) {
        ++position;
    }
    if (*position != '[') return 0;
    ++position;
    char *end = NULL;

    errno = 0;
    uintmax_t parsedindex = strtoumax(position, &end, 10);
    if ((errno != 0) || (end == position) || (parsedindex > SIZE_MAX) ||
        (strncmp(end, entryprefix, sizeof(entryprefix) - 1U) != 0)) return -1;
    position = end + sizeof(entryprefix) - 1U;
    uint64_t parsedkey = 0;
    unsigned digitcount = 0;

    for (;;) {
        unsigned char character = (unsigned char)*position;
        unsigned digit;

        if ((character >= '0') && (character <= '9')) {
            digit = (unsigned)(character - '0');
        } else if ((character >= 'a') && (character <= 'f')) {
            digit = (unsigned)(character - 'a') + 10U;
        } else if ((character >= 'A') && (character <= 'F')) {
            digit = (unsigned)(character - 'A') + 10U;
        } else {
            break;
        }
        if (digitcount == 16U) return -1;
        parsedkey = (parsedkey << 4) | digit;
        ++digitcount;
        ++position;
    }
    if (digitcount == 0U) return -1;
    if (strncmp(position, "),", 2U) != 0) return -1;
    position += 2U;
    while ((*position == ' ') || (*position == '\t') || (*position == '\r')) {
        ++position;
    }
    if ((*position != '\n') && (*position != '\0')) return -1;
    *index = (size_t)parsedindex;
    *key = parsedkey;
    return 1;
}

static KeyArray readheader(const char *path)
{
    errno = 0;
    FILE *input = fopen(path, "r");

    if (input == NULL) syserror("open", path, errno);
    KeyArray result = {0};
    char line[HEADER_LINE_CAPACITY];
    size_t linenumber = 0;
    size_t declaredcapacity = 0;
    size_t declaredcount = 0;
    int havecapacity = 0;
    int havecount = 0;
    int havetokenbits = 0;
    int haveapplicationtoken = 0;
    int havestoken = 0;
    int havektoken = 0;
    int havextoken = 0;
    size_t previousindex = 0;
    int haveindex = 0;

    while (fgets(line, sizeof(line), input) != NULL) {
        ++linenumber;
        size_t length = strlen(line);

        if ((length == 0U) || (line[length - 1U] != '\n')) {
            invalidheader(path, linenumber, "line is too long or unterminated");
        }
        if (parseencodingdefine(line, path, linenumber,
                                "INFINITE_KEY_TOKEN_BITS",
                                PACKED_KEY_TOKEN_BITS,
                                &havetokenbits) > 0) continue;
        if (parseencodingdefine(line, path, linenumber,
                                "INFINITE_KEY_APPLICATION_TOKEN",
                                PACKED_TOKEN_APPLICATION,
                                &haveapplicationtoken) > 0) continue;
        if (parseencodingdefine(line, path, linenumber,
                                "INFINITE_KEY_S_TOKEN", PACKED_TOKEN_S,
                                &havestoken) > 0) continue;
        if (parseencodingdefine(line, path, linenumber,
                                "INFINITE_KEY_K_TOKEN", PACKED_TOKEN_K,
                                &havektoken) > 0) continue;
        if (parseencodingdefine(line, path, linenumber,
                                "INFINITE_KEY_X_TOKEN", PACKED_TOKEN_X,
                                &havextoken) > 0) continue;
        int status = parseunsigneddefine(line, "INFINITE_KEY_CAPACITY",
                                         &declaredcapacity);
        if (status < 0) invalidheader(path, linenumber, "invalid capacity");
        if (status > 0) {
            if (havecapacity) invalidheader(path, linenumber,
                                            "duplicate capacity");
            havecapacity = 1;
            continue;
        }
        status = parseunsigneddefine(line, "INFINITE_KEY_COUNT",
                                     &declaredcount);
        if (status < 0) invalidheader(path, linenumber, "invalid key count");
        if (status > 0) {
            if (havecount) invalidheader(path, linenumber,
                                         "duplicate key count");
            havecount = 1;
            continue;
        }
        size_t index;
        uint64_t key;

        status = parsetableentry(line, &index, &key);
        if (status < 0) invalidheader(path, linenumber,
                                     "invalid hash-table entry");
        if (status == 0) continue;
        if (!havecapacity || (index >= declaredcapacity)) {
            invalidheader(path, linenumber, "hash-table index is out of range");
        }
        if (haveindex && (index <= previousindex)) {
            invalidheader(path, linenumber,
                          "hash-table indices are not strictly increasing");
        }
        if (key == 0) invalidheader(path, linenumber, "packed key is zero");
        appendkey(&result, key);
        previousindex = index;
        haveindex = 1;
    }
    if (ferror(input)) syserror("read", path, errno);
    if (fclose(input) != 0) syserror("close", path, errno);
    if (!havecapacity) fatal("infinite.h has no INFINITE_KEY_CAPACITY");
    if (!havecount) fatal("infinite.h has no INFINITE_KEY_COUNT");
    if (!havetokenbits || !haveapplicationtoken || !havestoken ||
        !havektoken || !havextoken) {
        fatal("infinite.h has no complete packed-key encoding schema");
    }
    if (result.count != declaredcount) {
        fatal("infinite.h key count does not match its table entries");
    }
    qsort(result.keys, result.count, sizeof(*result.keys), comparekeys);
    for (size_t i = 1U; i < result.count; ++i) {
        if (result.keys[i] == result.keys[i - 1U]) {
            fatal("infinite.h contains a duplicate packed key");
        }
    }
    return result;
}

static unsigned packedkeybits(uint64_t key)
{
    if (key <= PACKED_TOKEN_K) {
        return PACKED_KEY_TOKEN_BITS;
    }
    unsigned bits = 0;

    while (key != 0) {
        ++bits;
        key >>= 1;
    }
    return bits;
}

static int decodenode(uint64_t key, unsigned bits, unsigned *offset,
                      Node *nodes, unsigned *nodecount)
{
    if ((*offset + PACKED_KEY_TOKEN_BITS) > bits ||
        *nodecount >= MAX_PACKED_NODES) return -1;
    unsigned shift = bits - *offset - PACKED_KEY_TOKEN_BITS;
    unsigned token = (unsigned)((key >> shift) & UINT64_C(3));
    int index = (int)(*nodecount);

    ++*nodecount;
    *offset += PACKED_KEY_TOKEN_BITS;
    nodes[index].token = token;
    nodes[index].left = -1;
    nodes[index].right = -1;
    if (token == PACKED_TOKEN_APPLICATION) {
        nodes[index].left = decodenode(key, bits, offset, nodes,
                                       nodecount);
        nodes[index].right = decodenode(key, bits, offset, nodes,
                                        nodecount);
        if ((nodes[index].left < 0) || (nodes[index].right < 0)) return -1;
    } else if ((token != PACKED_TOKEN_S) && (token != PACKED_TOKEN_K)) {
        return -1;
    }
    return index;
}

static int appendcharacter(char *output, size_t capacity, size_t *length,
                           char character)
{
    if ((*length + 1U) >= capacity) return 0;
    output[(*length)++] = character;
    output[*length] = '\0';
    return 1;
}

static int renderexpression(const Node *nodes, int node, char *output,
                            size_t capacity, size_t *length);

static int renderterm(const Node *nodes, int node, char *output,
                      size_t capacity, size_t *length)
{
    if (nodes[node].token != PACKED_TOKEN_APPLICATION) {
        return appendcharacter(output, capacity, length,
                               nodes[node].token == PACKED_TOKEN_S ? 'S' : 'K');
    }
    return appendcharacter(output, capacity, length, '(') &&
        renderexpression(nodes, node, output, capacity, length) &&
        appendcharacter(output, capacity, length, ')');
}

static int renderexpression(const Node *nodes, int node, char *output,
                            size_t capacity, size_t *length)
{
    if (nodes[node].token != PACKED_TOKEN_APPLICATION) {
        return renderterm(nodes, node, output, capacity, length);
    }
    return renderexpression(nodes, nodes[node].left, output, capacity, length) &&
        renderterm(nodes, nodes[node].right, output, capacity, length);
}

static void keytoexpression(uint64_t key,
                            char output[MAX_EXPRESSION_LENGTH + 1U])
{
    unsigned bits = packedkeybits(key);

    if ((bits < PACKED_KEY_TOKEN_BITS) ||
        (bits > PACKED_KEY_BITS) ||
        ((bits % PACKED_KEY_TOKEN_BITS) != 0U)) {
        fatal("infinite.h contains an invalid packed-key width");
    }
    if ((bits > PACKED_KEY_TOKEN_BITS) &&
        (((key >> (bits - PACKED_KEY_TOKEN_BITS)) & UINT64_C(3)) !=
         PACKED_TOKEN_APPLICATION)) {
        fatal("infinite.h contains an invalid packed expression tree");
    }
    Node nodes[MAX_PACKED_NODES];
    unsigned offset = 0;
    unsigned nodecount = 0;
    int root = decodenode(key, bits, &offset, nodes, &nodecount);

    if ((root < 0) || (offset != bits)) {
        fatal("infinite.h contains an invalid packed expression tree");
    }
    size_t length = 0;

    output[0] = '\0';
    if (!renderexpression(nodes, root, output, MAX_EXPRESSION_LENGTH + 1U,
                          &length)) {
        fatal("decoded expression exceeds its output buffer");
    }
}

static void writecatalog(const char *path, const char *temppath,
                         const KeyArray *array)
{
    errno = 0;
    FILE *output = fopen(temppath, "w");

    if (output == NULL) syserror("open", temppath, errno);
    int failed = fprintf(output,
        "* Generated by makeinfcmb from infinite.h. Do not edit.\n"
        "*\n"
        "* infinite.cmb\n"
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
        "*\n") < 0;

    for (size_t i = 0; (i < array->count) && !failed; ++i) {
        char expression[MAX_EXPRESSION_LENGTH + 1U];

        keytoexpression(array->keys[i], expression);
        failed = fprintf(output, "0x%" PRIx64 ": %s\n",
                         array->keys[i], expression) < 0;
    }
    int writeerror = errno ? errno : EIO;

    errno = 0;
    if (fclose(output) != 0) {
        if (!failed) writeerror = errno ? errno : EIO;
        failed = 1;
    }
    if (failed) {
        (void)remove(temppath);
        syserror("write", path, writeerror);
    }
#if defined(_WIN32) || defined(_WIN64)
    if (!MoveFileExA(temppath, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();

        (void)remove(temppath);
        fprintf(stderr, "replace %s failed with error %lu\n", path,
                (unsigned long)error);
        exit(EXIT_FAILURE);
    }
#else
    errno = 0;
    if (rename(temppath, path) != 0) {
        int error = errno;

        (void)remove(temppath);
        syserror("replace", path, error);
    }
#endif
}

int main(int argc, char **argv)
{
    (void)argc;
    char *executable = getexecutablepath(argv[0]);
    char *inputpath = adjacentpath(executable, "infinite.h");
    char *outputpath = adjacentpath(executable, "infinite.cmb");
    char *temppath = adjacentpath(executable, "infinite.cmb.tmp");

    free(executable);
    KeyArray keys = readheader(inputpath);

    writecatalog(outputpath, temppath, &keys);
    printf("wrote %zu packed keys to %s\n", keys.count, outputpath);
    free(keys.keys);
    free(temppath);
    free(outputpath);
    free(inputpath);
    return EXIT_SUCCESS;
}
