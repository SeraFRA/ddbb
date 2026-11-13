# progettodatabase

[![ci](https://github.com/SeraFRA/ddbb/actions/workflows/ci.yml/badge.svg)](https://github.com/SeraFRA/ddbb/actions/workflows/ci.yml)

English | [Italiano](README.md)

**A database in pure C, started from a contiguous array of values and grown into an engine with persistence, a learned index, a semantic memory — and an MCP layer that turns it into a tool agents can call.**

Self-teaching project: every file is a standalone, compilable chapter, verified with automatic tests and **numbers measured against theory** — never promised, always measured. Zero external libraries: only C11 and the standard library. The history lives in the commits: every chapter carries its post mortem in the commit message.

---

## How it was born: the collaboration

This repo is also the record of a collaboration between a human author and a coding AI (Mistral Vibe). The first request was not "fix my code" but *"give me debugging hints, teach me something"*: the contract was to understand, not to obtain. From there the method, unchanged across ten chapters:

1. **the author brings the direction as a metaphor** — never as a specification;
2. **the AI translates the metaphor into mechanics** (technique name, math, real-world counterpart);
3. **the mechanics go under measurement**: numbers compared against theory, always;
4. **the tests are rigid and bind the AI too**: the demo exits with EXIT≠0 when a contract breaks, and every AI bug ends up in a post mortem written exactly where it happened in the code.

The metaphors were the real engine of the design. The main ones:

| the author said | translated into mechanics, it became | in the chapter |
|---|---|---|
| "an employee who remembers where he puts the products on the shelves, and finds them thanks to the probability of the previous times" | **learned index**: position prediction replaces the random starting point; the invariant is that whoever changes his "memory" rebuilds the table | `database_learned.c` |
| "a database that behaves like human memory: everything connected and close in a probabilistic semantic space" | **MinHash + LSH banding**: the semantic space is the set of positional bigrams; two keys are "close" when they share pieces; the probability of meeting is the signature | `database_semantic.c` |
| "a db that is not purely deterministic: give probable answers, answering questions with its own entries" | **declared confidence**: exact hit → 1.0; typo → top-k with the value, a group of equals when the truth is ambiguous, silence when there is nothing to say | `db_chiedi` / `motore_chiedi` |
| "expiring data should not be checked on every read" | **timing wheel**: expiry stops being a computation and becomes a place on the ring | `database_ring.c` |

The honest point: the AI proposed chapters on its own too (Bloom/quotient, persistence), but the intuitions that turned the project toward the probabilistic are all the author's — and that is where the value of the collaboration lies: human metaphors chose **which** techniques deserved to exist, the AI chose **how** to make them work and paid for their bugs, documenting them.

The method sharpened over time, up to the **invariants-first** experiment of the tenth chapter: contract written before the code. Before, the cycle was *code → bug → post mortem*, for three chapters in a row; then the cycle inverted — 8 traps caught at the design table before writing a line, 1 at the first run, and the escaped bug told exactly which assertion was missing in the tests. The verdict of the experiment is in the tenth chapter's commit and in the engine's header.

What it taught, in one sentence: a human metaphor plus a test that knows how to fail is worth more than a hundred generated lines — because they constrain the direction, not just the syntax.

---

## The chapter map

| # | file | what's in it | the real world calls it | the number to remember |
|---|------|--------------|---------------------------|------------------------|
| 0 | `database.c` | the starting point: contiguous array of entries, the original code with its didactic bugs — left untouched: it is the fossil | — | — |
| 1 | `database_debug.c` | the first cleanup: boundary bugs, inspection tools, `FIX` markers | debugging | — |
| 2 | `database_indexed.c` | append-only buffer + hash index | **Bitcask** (append-only log + in-RAM hash) | O(1) get, probe chains |
| 3 | `database_compact.c` | tombstones, O(n) compaction, p50/p90/p99 percentiles | the LSM **merge** | the distribution beats the average |
| 4 | `database_persist.c` | CRC-32 per entry, compacted snapshots, validated load: safe prefix | checksum + snapshot (SQLite, RocksDB) | 1 corrupted byte = only that entry lost |
| 5 | `database_learned.c` | a spline on the CDF of the keys instead of the hash as starting point | **learned index** (Kraska 2017) | the invariant: change the starting function → rebuild |
| 6 | `database_semantic.c` | positional bigrams + MinHash + LSH banding: the db that understands typos | **MinHash/LSH** (Broder 1997) | typos recovered in the top-8, ambiguity declared |
| 7 | `database_ring.c` | expiry as a PLACE (a ring of ticks) + backshift delete | **timing wheel** (Linux kernel) | batch expiries, amortized O(1) |
| 8 | `database_quotient.c` | Bloom and quotient filter, the accounting of bits | **Bloom 1970 / Bender 2012** | false positives measured 0.777% vs 0.82% theory |
| 9 | `database_persist_semantic.c` | the fusion: the semantic answer comes from disk, with the value — featuring the *invariants-first* experiment | — | 0 wrong out of 60000 disk verifications |
| 11 | `mcp_server.c` + `mcp_stdio.c` | the MCP layer: the engine as a tool for an LLM agent — 9 tools, confidence always declared, batch `presente` | **MCP** (Model Context Protocol) | get p99 2.3 µs vs 10²..10³ ms of the LLM round-trip |

## The single engine

The chapters are one-function demos; `motore.h` + `motore.c` consolidate them into **one system** with a public API (opaque handle) and a written contract:

```c
motore_t *m = motore_apri();
motore_put(m, key, klen, value, vlen, ttl);        /* with TTL */
motore_get(m, key, klen, &vlen);                    /* or NULL: never lies */
motore_chiedi(m, query, qlen, k, answers, &cand);   /* exact -> confidence 1.0;
                                                       miss -> semantic top-k
                                                       with the value */
motore_intervallo(m, lo, llen, hi, hlen, k, out, &tot, NULL); /* [lo..hi]:
                                                       the hash cannot do it */
motore_avanza(m, t);                                 /* the clock is yours [I8] */
motore_salva(m, "m.db");                             /* chapter 9's format */
motore_audit(m, &declared);                          /* invariant [I7] as an API */
```

The architectural heart is invariant **[I7]: every live entry IS the current version of its key** — every death (delete, expiry, supersede) leaves its mark at the same instant it leaves the counting. Consequences: the range scan is a byte walk with zero lookups, and the post-compaction tail merges by itself.

What did **not** make it into the engine, and why (the morgue): the backshift delete (the engine has compaction cleaning up tombstones: backshift only wins without it), the quotient filter (it is a membership index for those who lack an exact one), the linear spline of chapter 5 (here the full-key fence is exact by construction).

## The MCP layer: the engine as a tool (chapter 11)

Metaphor: *the agent must not know the engine, it must be able to call it — and every answer must declare what it is worth.* Mechanics: an MCP server (spec 2025-06-18, JSON-RPC 2.0 over stdio) on top of the engine's **public API**, without touching its internals: `mcp_stdio.c` (the thin shell: one JSON line in, one out), `mcp_server.c` (the pure, in-process-testable core `mcp_gestisci`), `json.c` (a hand-written minimal parser: zero dependencies, and **one single source of truth** — the wire uses it and the bench uses it to verify the answers). The contract is [M1]..[M9], written before the code:

- **as few round-trips as possible**: `chiedi` returns key AND value AND confidence (never keys alone: the second call for the values is the economic bug); `intervallo` returns the batch plus the total; `presente` is batch (up to 256 keys per call).
- **confidence always travels**: low confidence = the agent knows it can retry with another wording. Self-correction for one extra round.
- **`presente` as pre-filter**: "certainly not" [I9] at near-zero cost — the agent decides whether the call is worth it.
- **`audit` as a tool**: a broken contract is visible to the agent too, which may distrust and verify.
- **the limits are the description**: the similarity is SYNTACTIC (positional bigrams + MinHash), not semantic — that is the part the LLM reads before calling.
- **the clock is the caller's [I8]**: `avanza` is a tool; the server never reads a wall clock.

Numbers from the `demo_mcp` bench (measured, gate included): get p50 2.1 µs / p99 2.3 µs, answer 149 bytes (~37 tokens); `chiedi` on typos p50 9.5 µs, truth in the top-8 at 88%; `presente` batch-64 in 15 µs; `intervallo` over 100-key windows in 23 µs. A typical task is **one round-trip only**: the cost of every query is the LLM round (10²..10³ ms of generation), and the layer minimizes it. The bench also measures the bloom's life cycle: 90.6% "maybe" on aliens before recomposition (stale filter, 3072 bits), 0/32 after the disk round — the derived structure straightens itself out, declared not hidden.

Positioning, stated honestly: we do not compete with Mem0/Zep/Letta on their plane (they are LLM-based extraction logic); we are **the plane below** — the in-process, deterministic, zero-dependency, native-TTL storage substrate. The fair comparison is with Redis/Qdrant as a backend, and the niche is embedded/edge memory where one LLM call per `add()` is unacceptable. Our cost is on queries (every interrogation is token generation), theirs on writes (every ingested message goes through an LLM): for an agent that writes a lot and interrogates selectively, our model is cheaper by a huge factor.

And the use that closes the loop: mounted as an MCP server in an agent (Mistral Vibe, for one), the engine becomes natively callable — debugging the project goes through the tool the project provides. The bench stays the measurement; the agent is the testimony.

## Build and run

You need `gcc` (MinGW on Windows, gcc on Linux) and `make` (on Windows MSYS2 provides it as `mingw32-make`; by hand: `gcc -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=199309L -O2 -o motore_demo motore.c demo_motore.c`). No dependencies.

```bash
make motore_demo        # the engine + the test bench: a gate for EVERY invariant
make mcp_server         # the MCP server over stdio: ./mcp_server [file.db [tick]]
make demo_mcp           # the MCP layer bench: gates [M1]..[M9] + MEASUREMENT
make database_quotient  # any chapter, one at a time
./motore_demo           # exits with EXIT!=0 if a contract breaks
```

On Windows: Device Guard may block freshly compiled binaries in the project folder (exit 126). Wait a few seconds and retry, or build in a directory outside the policy.

The push is protected by CI: on every push an ubuntu+gcc job rebuilds every chapter and **runs it as a test bench** — a broken contract turns the badge at the top of this README red. The CI's first two catches: the Linux port (four chapters compiled only on MinGW because the non-Windows branch used `struct timespec` and `clock_gettime` without including `<time.h>`; and even including it, with strict `-std=c11` glibc does not declare `clock_gettime` without `_POSIX_C_SOURCE`) and the linking of `pow()`: on Linux it lives in libm, which MinGW folds into the runtime — hence the local builds working. The Makefile now carries `_POSIX_C_SOURCE` in CFLAGS and `-lm` in LDLIBS.

## The engine's numbers (measured, not promised)

- 50000 live keys on disk (2.46 MB): sorting compaction 64 ms, reload with the rebuild of semantics, gates and ring 64 ms, 0 wrong out of 60000 verifications.
- Range `u0002500..u0003499`: 833 matches == 833 from the independent count, in 865 steps; the merge with the tail holds on three counts.
- Exact gets: p50 0.100 µs; Bloom gate: 99.22% of the aliens answered without touching a slot.
- One-digit typos: 99.9% of answers declared, truth in the top-8 at 94.2%, group of equals 4.9 (the ambiguity is the typo's, and the db declares it instead of pretending).
- 20000 expiries in the same tick: handled in 0.1 ms **without reading a single key**.

## The method lessons

This repo is also a diary of how one learns to build things one does not know how to build yet; chapter by chapter:

- **Measure first, believe later.** Every technique arrives with its number compared against theory (Bloom: 0.777% vs 0.82%; banding: predicted capture vs measured top-k).
- **Printing is not blocking.** A test that prints errors without failing the process is not a test: this lesson cost two chapters in a row in the post mortems.
- **A comparison without a threshold is not a gate.** The step from "0 == 0, green!" to "470/500" is the heart of the engine's post mortem.
- **Invariants-first.** The contract written before the code moves bug-catching to the left (into design). In the tenth chapter: 8 traps caught at the table, 1 at the first run. In the engine: the real bug was found by a `-Wall` warning before any run.
- **Honesty is an API.** Declared confidences, silence instead of invention, the Bloom's "maybe" distinct from the probe's "no".

## Layout

```
database*.c            # the nine chapters + the original: each builds alone
motore.h / motore.c    # the single engine (public API, opaque handle)
demo_motore.c          # the test bench: a gate per invariant
json.h / json.c        # the minimal JSON parser: one source of truth for the wire
mcp_server.h / mcp_server.c  # the MCP core: 9 tools, contract [M1]..[M9]
mcp_stdio.c            # the thin shell: stdin/stdout, one object per line
demo_mcp.c             # the layer's bench: gates and MEASUREMENT (latency, tokens)
Makefile               # make motore_demo | make mcp_server | make <chapter>
.gitignore             # binaries and test files stay out of the repo
```

## Where to go next

The engine declares its limits in `motore.c`'s header: single-threaded, stats via `printf` (teaching project), answers alive until the next put. The natural next steps: a WAL between snapshots, `fsync`, stable iterators, concurrency one extreme at a time. And the twelfth chapter, already declared: the speculative prefetcher that watches the agent's reasoning trace — prediction as a semantic query on the trace, staged with a short TTL, hit-rate above a threshold or nothing.

## License

MIT — see [LICENSE](LICENSE).
