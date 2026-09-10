#!/bin/sh
# Part of ysearch5; SPDX-License-Identifier: GPL-3.0-or-later
# No existing catalogue/header is needed or modified by these tests.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/ysearch5-header-test.XXXXXX")
cleanup()
{
    # Only remove named artifacts created inside this fresh temporary directory.
    set +e
    for writer in ysearch makeinfh; do
        for fixture in 0 1 2 3; do
            directory="$test_dir/$writer-$fixture"
            for artifact in infinite.h infinite.h.tmp infinite.cmb infinite.cmb.tmp \
                makeinfcmb check_header decoder.log records.actual records.expected; do
                rm -f "$directory/$artifact"
            done
            rmdir "$directory" 2>/dev/null
        done
    done
    for artifact in main.c cuckoo.h infinite.h rejected.log; do
        rm -f "$test_dir/embedded/$artifact"
    done
    rmdir "$test_dir/embedded" 2>/dev/null
    rm -f "$test_dir/ysearch_writer" "$test_dir/makeinfh_writer" "$test_dir/makeinfcmb"
    rmdir "$test_dir" 2>/dev/null
}
trap cleanup EXIT
set -- -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -O2 \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# The renamed, uncalled application contains unused test/search feature helpers.
"${CC:-clang}" "$@" -pthread -Wno-unused-function -Wno-unused-const-variable \
    -DTEST_YSEARCH_WRITER "$test_root/tests/header_writer_test.c" -o "$test_dir/ysearch_writer"
"${CC:-clang}" "$@" "$test_root/tests/header_writer_test.c" -o "$test_dir/makeinfh_writer"
"${CC:-clang}" "$@" "$test_root/makeinfcmb.c" -o "$test_dir/makeinfcmb"

for writer in ysearch makeinfh; do
    for fixture in 0 1 2 3; do
        directory="$test_dir/$writer-$fixture"
        mkdir "$directory"
        "$test_dir/${writer}_writer" "$directory" "$fixture"
        test ! -e "$directory/infinite.h.tmp"
        "${CC:-clang}" "$@" -DHEADER_FIXTURE="$fixture" -I"$directory" \
            "$test_root/tests/header_lookup_test.c" -o "$directory/check_header"
        "$directory/check_header"
        # The decoder resolves its input/output beside its real executable.
        cp "$test_dir/makeinfcmb" "$directory/makeinfcmb"
        if [ "$fixture" -lt 2 ]; then
            "$directory/makeinfcmb" > "$directory/decoder.log" 2>&1
            if [ "$fixture" -eq 1 ]; then
                printf '0x1: S\n0x2: K\n' > "$directory/records.expected"
            else
                : > "$directory/records.expected"
            fi
            awk '/^0x/ { print }' "$directory/infinite.cmb" > "$directory/records.actual"
            cmp "$directory/records.expected" "$directory/records.actual"
        else
            # Zero is legal for the generic hash API, not a closed S/K catalogue key.
            printf 'preexisting catalogue must survive\n' > "$directory/records.expected"
            cp "$directory/records.expected" "$directory/infinite.cmb"
            if "$directory/makeinfcmb" > "$directory/decoder.log" 2>&1; then
                printf 'Decoder unexpectedly accepted zero fixture %s/%s\n' "$writer" "$fixture" >&2
                exit 1
            fi
            grep -F 'hash-table index is out of range' "$directory/decoder.log" > /dev/null
            cmp "$directory/records.expected" "$directory/infinite.cmb"
        fi
        test ! -e "$directory/infinite.cmb.tmp"
    done
done

# Compile the real embedded path against a modern header first, then strip only
# its extra bucket. Unchanged geometry/count must not hide the stale extent.
mkdir "$test_dir/embedded"
cp "$test_root/main.c" "$test_root/cuckoo.h" "$test_dir/embedded/"
cp "$test_dir/ysearch-1/infinite.h" "$test_dir/embedded/infinite.h"
"${CC:-clang}" "$@" -pthread -DSINGLE_THREAD=1 -DHAS_INFINITE_H=1 \
    -fsyntax-only "$test_dir/embedded/main.c"
sed 's/INFINITE_KEY_CAPACITY + INFINITE_KEY_BUCKET_SIZE/INFINITE_KEY_CAPACITY/' \
    "$test_dir/ysearch-1/infinite.h" > "$test_dir/embedded/infinite.h"
if "${CC:-clang}" "$@" -pthread -DSINGLE_THREAD=1 -DHAS_INFINITE_H=1 \
    -fsyntax-only "$test_dir/embedded/main.c" > "$test_dir/embedded/rejected.log" 2>&1; then
    printf 'Embedded compilation unexpectedly accepted a missing metadata bucket\n' >&2
    exit 1
fi
grep -F 'embedded infinite-key storage must include the zero-key bucket' \
    "$test_dir/embedded/rejected.log" > /dev/null
printf 'Both header writers, zero metadata, catalogue decoding, and stale extent checks passed.\n'
