### 🚨 Current Focus & Active Blockers
- **Context:** Hardening `main.c`, an exhaustive multithreaded search and evaluator for S/K combinator expressions and fixed-point candidates.
- **Next Step:** No active blocker; resource exhaustion semantics are intentionally unchanged.

### 🛠️ Architectural Discoveries (Codex Insights)
- The evaluator stores expression trees in per-thread indexed cell arenas (`nxt`/`cnts`) with a custom freelist; values below `FREEMIN` are characters and larger values identify nested lists.
- The default search enumerates full binary application shapes, substitutes every S/K labeling, filters reducible forms and known divergent prefixes, and evaluates candidates on worker threads.
- A strict C11 syntax build with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread` reports no warnings on the current macOS toolchain.

### 🧠 Lessons Learned & Avoided Traps
- Any build that enables `DOTESTS` or `DOSEARCH` must initialize `neverends` before evaluation and destroy it only after worker shutdown.
- Resource exhaustion being classified as nontermination is intentional and must not be changed.

### 🏆 Completed Feature Milestones
- 2026-08-22: Completed initial structural review, strict compiler check, and AddressSanitizer/UndefinedBehaviorSanitizer test run of `main.c`.
- 2026-08-22: Added fail-fast diagnostics for never-ending set allocation failure, string duplication failure, and fixed-capacity exhaustion.
- 2026-08-22: Fixed standalone `DOTESTS=1, DOSEARCH=0` by sharing `neverends` initialization across test/search modes and cleaning it up after worker shutdown; sanitizer tests pass.
