# ysearch5

`ysearch5` searches for fixed-point combinators expressed only with the
S and K combinators. Application is left-associative, so `SSK` means
`(S S) K`. Reduction uses the standard rules:
```text
K a b   -> a
S a b c -> a c (b c)
```
For each candidate closed S/K expression `P`, the program appends an opaque
variable `x` and evaluates `P x`. It reports `P` when it witnesses
```text
P x ->* x Q
Q   ->* P x
```
which establishes the fixed-point equation `P x ->* x (P x)`. Results are
printed as `!!! Y = P`.

The displayed `Length` is the number of S/K leaves in `P`; applications,
parentheses, and the appended `x` do not count. The search enumerates every
supported binary application-tree shape from one through `MAXLEN` (== 12) leaves,
fixes the leftmost leaf to S, and assigns S or K to every remaining leaf. It
skips candidates for which `P x` already contains a reducible `K a b` or
`S K a b` form. Evaluation uses weak-head reduction with call-by-need sharing
for the argument duplicated by S.

Candidates are classified as terminating, structurally repeating, otherwise
recurring, or out of the configured step/cell resources. Resource exhaustion
is operationally classified as `Never ends`; it is not a proof of mathematical
divergence. Divergent and resource-exhausted candidates can be recorded in
`infinite.cmb`. Catalogue matches are exact whole-expression matches; they are
not prefix matches.

## Requirements

- A C11 compiler
- POSIX threads and semaphores
- macOS or another supported POSIX environment

The commands below use Apple Clang through Xcode. On another POSIX system,
replace `xcrun clang` with `clang` or `cc`. `-march=native` is optional.

## Compile with `infinite.h`

When `infinite.h` is beside `main.c`, build the embedded-catalogue version
with:
```sh
xcrun clang \
  -std=c11 -O3 -march=native -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -pthread -DHAS_INFINITE_H=1 \
  main.c -o ysearch5
```
Run it with:
```sh
./ysearch5
```
The packed-key hash table is compiled into the executable. This mode does not
read or write `infinite.cmb` at runtime.

## Bootstrap without `infinite.cmb` or `infinite.h`

These steps use a separate `build` directory. Both `ysearch5` and `makeinfh`
locate their data files relative to their own resolved executable paths, not
relative to the shell's current directory.

### 1. Create the build directory
```sh
mkdir -p build
```
Make sure `build/infinite.cmb` does not already exist if the goal is to start
a new uncached search.

### 2. Compile the file-backed search

Defining `HAS_INFINITE_H=0` prevents `main.c` from trying to include the
missing `infinite.h` header:
```sh
xcrun clang \
  -std=c11 -O3 -march=native -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -pthread -DHAS_INFINITE_H=0 \
  main.c -o build/ysearch5
```
### 3. Run the uncached search
```sh
./build/ysearch5
```
Because no catalogue exists, the program creates `build/infinite.cmb` and
writes each newly classified divergent combinator as:
```text
0x<16-hex-digit-packed-key>: <readable-SK-expression>
```
The file begins with comment lines whose first non-whitespace character is
`*`. Both `main.c` and `makeinfh.c` ignore those lines when reading it.

The catalogue is complete only after the program exits successfully. If the
run is interrupted or crashes, move or remove the partial
`build/infinite.cmb` before restarting an uncached search; a truncated final
record is intentionally rejected.

### 4. Compile the header generator

Place the generator executable beside the completed catalogue:
```sh
xcrun clang \
  -std=c11 -O3 -march=native -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  makeinfh.c -o build/makeinfh
```
### 5. Generate `infinite.h`
```sh
./build/makeinfh
```
`makeinfh` validates every packed key against its readable expression, sorts
and deduplicates the keys, constructs the final 1,048,576-slot hash table, and
writes `build/infinite.h`.

### 6. Compile the embedded-catalogue search

The generated header is in `build`, so add that directory to the include
path:
```sh
xcrun clang \
  -std=c11 -O3 -march=native -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -pthread -DHAS_INFINITE_H=1 -Ibuild \
  main.c -o build/ysearch5-embedded
```
Run the embedded version with:
```sh
./build/ysearch5-embedded
```
## File-backed reuse without generating a header

If a valid `infinite.cmb` already exists beside a file-backed `ysearch5`
executable, the program loads it as a read-only exact-match cache. It does not
truncate or add to an existing catalogue. Newly discovered divergences are
reported in the program's totals but are not appended to that preloaded file.

## Parallelism and limits

At startup, the multithreaded build uses the number of online logical CPUs
minus one worker, clamped to the range 1 through 64. Search length, arena
limits, and step limits are compile-time constants near the top of `main.c`.

## License

`ysearch5` is distributed under the GNU General Public License, version 3 or
later. See `LICENSE`.
