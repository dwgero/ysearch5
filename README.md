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
`S K a b` form. Evaluation uses strong leftmost-outermost graph reduction.
After the outer expression reaches weak-head form, reduction continues through
its arguments from left to right, including arguments of underapplied `S` or
`K` and of the opaque variable `x`. An argument duplicated by `S` is shared
call-by-need rather than copied and reduced independently.

A candidate is accepted immediately when the complete term is `x Q` and `Q`
equals the original `P x`. A winner is not normalized further: doing so would
force the very recurrence that the program has just established.

Otherwise, a candidate terminates only when no reachable S/K redex remains.
`Repeats forever` means the complete expression returned exactly to its
initial `P x` state. Memo cycles and configured step/cell exhaustion are
reported as `Never ends`; other cycles need not be recognized before reaching
a resource limit. Resource exhaustion is an operational classification, not a
proof of mathematical divergence.

An existing `infinite.cmb` can seed the search with divergent and
resource-exhausted candidates. Catalogue lookups are exact
complete-expression matches, never prefix or nested-subexpression matches.
New classifications are retained in memory and written to `infinite.h` at
successful shutdown; `main.c` never creates or modifies `infinite.cmb`.

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

Running `ysearch5` this way took 4 seconds on a MacBook Pro M4 Max with 16 cores.

## Create `infinite.cmb` from `infinite.h`

`makeinfcmb` reads `infinite.h` beside its resolved executable, validates the
declared table size, key count, sparse indices, and packed expression trees,
then creates `infinite.cmb` in the same directory.

For example, create the catalogue in a separate `build` directory:
```sh
mkdir -p build
cp infinite.h build/infinite.h
xcrun clang \
  -std=c11 -O3 -march=native -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  makeinfcmb.c -o build/makeinfcmb
./build/makeinfcmb
```
This writes `build/infinite.cmb`. Keys are sorted so the output is
deterministic. If `build/infinite.cmb` already exists, it is replaced
atomically only after the complete new catalogue has been written
successfully.

The readable-expression order may differ from a catalogue produced during a
search, but the key set is identical.

## Bootstrap without `infinite.cmb` or `infinite.h`

These steps use a separate `build` directory. `ysearch5` locates its catalogue
files relative to its own resolved executable path, not relative to the
shell's current directory.

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
  main.c -o build/ysearch5-noh
```
### 3. Run the uncached search
```sh
./build/ysearch5-noh
```
Because no catalogue exists, the program retains each newly classified
divergent key in memory. It does not create `build/infinite.cmb`. At successful
shutdown it constructs the complete hash table and atomically creates
`build/infinite.h`. If the run is interrupted or crashes, no partial header
replaces an existing `build/infinite.h`.

Catalogues are specific to the evaluator semantics and configured resource
ceilings; the file format does not carry a version or those limits. To
generate a complete current catalogue, start the file-backed executable
with no `infinite.cmb` beside it.

Running the uncached `ysearch5-noh` search took 40 minutes on a MacBook Pro M4
Max with 16 cores. Running it with the complete existing `infinite.cmb` cache
took 5 seconds.

### 4. Compile the embedded-catalogue search

The generated header is in `build`, so add that directory to the include
path:
```sh
xcrun clang \
  -std=c11 -O3 -march=native -DNDEBUG \
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -pthread -DHAS_INFINITE_H=1 -Ibuild \
  main.c -o build/ysearch5
```
Run the embedded version with:
```sh
./build/ysearch5
```
## File-backed reuse with an existing `infinite.cmb`

If a valid `infinite.cmb` already exists beside a file-backed `ysearch5`
executable, the program loads it as a read-only exact-match cache. It does not
truncate or add to an existing catalogue. Newly discovered divergences are
reported in the program's totals but are not appended to that preloaded file.
At successful shutdown it atomically creates or replaces `infinite.h` beside
the executable from the union of the preloaded and newly classified keys.

## Parallelism and limits

At startup, the multithreaded build uses the number of online logical CPUs
minus one worker, clamped to the range 1 through 64. Search length, arena
limits, and step limits are compile-time constants near the top of `main.c`.

## License

`ysearch5` is distributed under the GNU General Public License, version 3 or
later. See `LICENSE`.

Copyright (C) 2026 by David W. Gero

This `README.md` file is licensed under Creative Commons Attribution-ShareAlike
4.0 International. To view a copy of this license, visit
<https://creativecommons.org/licenses/by-sa/4.0/>.
