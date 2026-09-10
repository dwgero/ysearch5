#!/bin/sh
# Part of ysearch5; SPDX-License-Identifier: GPL-3.0-or-later
# Run with the complete current catalogue, optionally provided as argument one.
set -eu
if [ "$#" -gt 1 ]; then
    printf 'Usage: %s [infinite.cmb]\n' "$0" >&2
    exit 2
fi
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_catalogue=${1:-"$test_root/infinite.cmb"}
if [ ! -r "$test_catalogue" ]; then
    printf 'Supply the complete infinite.cmb, or create it using makeinfcmb first.\n' >&2
    exit 2
fi
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/ysearch5-cuckoo-test.XXXXXX")
trap 'rm -f "$test_dir/cuckoo_test" "$test_dir/alloc_native" "$test_dir/alloc_windows" "$test_dir/windows/malloc.h"; rmdir "$test_dir/windows" "$test_dir"' EXIT
mkdir "$test_dir/windows"
# The Windows test mocks the two CRT functions; no Windows SDK is required.
printf '/* Windows CRT declarations are supplied by the test harness. */\n' > "$test_dir/windows/malloc.h"
set -- -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -O2 \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
"${CC:-clang}" "$@" -DCUCKOO_ALLOCATOR_TEST "$test_root/tests/cuckoo_test.c" \
    -o "$test_dir/alloc_native"
"$test_dir/alloc_native"
"${CC:-clang}" "$@" -DCUCKOO_ALLOCATOR_TEST -DCUCKOO_MOCK_WINDOWS \
    -I"$test_dir/windows" "$test_root/tests/cuckoo_test.c" -o "$test_dir/alloc_windows"
"$test_dir/alloc_windows"
"${CC:-clang}" "$@" "$test_root/tests/cuckoo_test.c" -o "$test_dir/cuckoo_test"
"$test_dir/cuckoo_test" "$test_catalogue"
