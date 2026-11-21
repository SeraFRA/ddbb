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
| 11 | `mcp_server.c` + `mcp_stdio.c` | the MCP layer: the engine as a tool for an LLM agent — 10 tools, confidence always declared, batch `presente` | **MCP** (Model Context Protocol) | get p99 2.3 µs vs 10²..10³ ms of the LLM round-trip |
| 12 | `prefetch.c` + `prefetch_hook.c` | the speculative prefetcher: a pre_agent hook reads the agent's reasoning trace, predicts the keys it will need and stages them next to the engine with a TTL in requests; the MCP server consumes the staging and counts the hits | **speculative prefetch** (the CPU one, applied to the agent's IO) | hit-rate 1.00 on the bench's full wire, threshold 0.50 |
| 13 | `wal.c` + `misura_fsync.c` | the write-ahead log: intents are inked one by one (fwrite+fflush+fsync) before touching the engine; the replay rebuilds everything between two snapshots, the rotten tail is truncated | **WAL** (Bitcask, Postgres, SQLite) | 216 µs per durable put (p50), the replay is exact: 212/212 |
| 14 | `demo_multiagente.c` | the bench that decided the gateway: the cold-start hump measured BEFORE building — short and long sessions, same task, prefetch ON/OFF, costs from the real wire | benchmark-first ("measure first, build after") | the hump: margin 70.9% (short sessions) vs 38.4% (long) |
| 14 | `gateway.c` + `mcp_gateway.c` | the single-custode blackboard: ONE process owns the engine [I11], N clients speak the unchanged MCP wire [L3] through a TCP port on 127.0.0.1 — A writes, B reads with no save | **blackboard** (Hearsay-II, Linda 1985) — Redis's single-server model | get in-process 4.27 µs, across the door p50 12.46 µs |

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

## The speculative prefetcher: memory that watches the thoughts (chapter 12)

Metaphor: *the CPU prefetches instructions by watching the stream; the database prefetches by watching the agent's reasoning stream.* Extracting costs ~nothing, reasoning costs a lot: so we extract more than needed, now — but into a staging area, not into context: the LLM's window is the expensive resource, what is extracted does not enter until it is needed.

The geometry is two processes and one file:

- **the producer** is the `pre_agent` hook (`prefetch_hook.c`): every turn it receives the prompt, reads the recent reasoning trace from the session journal, predicts up to 4 keys and writes them to an atomic staging file (`<db>.prefetch`, tmp + rename); if it has something, it injects a one-liner handle into the context ("these are also available, confidence 0.4/0.31/0.2") — the agent pulls only what it needs. Fully fail-open: every hook error is silence, the session never stops because of speculation.
- **the consumer** is the MCP server: every request it consumes the staging (one generation, one consumption), parks the predictions next to the engine and counts them; a successful `get` on a staged key is a **hit**; the `prefetch_stat` tool exposes hit-rate and the byte balance (savings from avoided discoveries minus the cost of the injected handles).
- **speculation expires on its own**: predictions are born with a TTL of 32 requests and die in amortized O(1) batches — the wrong prefetcher disposes of itself, speculation with built-in forgetting. And the dedup does not prefetch what the agent already has in its trace.

The chapter's post mortem, which is also its lesson: the first version predicted with the engine's `chiedi` — but the engine's grams are **positional** (`pos` enters the hash), so `chiedi` is a fuzzy-PREFIX, not a topic search: it only predicted keys already verbatim in the trace, exactly the ones the dedup discards. Topic similarity lives in the layer: non-positional bigrams, windows of 5 tokens over the last 64, Jaccard with a 0.12 threshold, key scan via `motore_intervallo`. Ephemeral, never a second persistent index.

Numbers from the `demo_prefetch` bench (measured, gate included): `predici` over an 8 KB trace p50 906 µs / p99 1.16 ms — **once per turn**, against the LLM round's 10²..10³ ms; `consuma` p50 61 µs per request; `tocca` (the hit) ~0 µs. The end-to-end gate crosses the MCP wire: hit 1, cost 120 bytes, saving 500 bytes, balance +380, hit-rate 1.00 against a 0.50 threshold — EXIT≠0 if a tool lies about confidence or the balance diverges. And the declared limit: predictions are "maybe" (syntactic similarity on the trace, not semantic), a low hit-rate means speculation pollutes more than it saves — and the number is declared even when it disproves the idea.

The real session's verdict arrived, and it is honest in both directions. On a real two-day project (voice transcription, a single agent, debugging code already in context) the measurement protocol collected: hit-rate 0.10-0.11 against the 0.50 threshold, balance **−4231 bytes** — when the work is all on known context, speculation pollutes more than it saves, and the number says so. In sessions that resume topics days apart (the memory holds things OUTSIDE the context: handoffs, post mortems, research) the same tool sits at hit-rate 1.00 and balance +1874. The field lesson: **the prefetcher pays on the distance between memory and context** — and the numbered threshold exists precisely so this can be declared. In the red, two merits: the 7 wrong predictions died on their own in the TTL (built-in forgetting disposed of the pollution), and the disappointing data is the declared starting point for the future: a threshold that cannot adapt to the hit-rate, and a dedup that watches the keys but not the values already in context.

### The living bench: real LLM agents on the blackboard ([P10])

Since chapter 14 the hook knows how to be a **client of the blackboard**: keys are no longer read from the snapshot but asked from the custode over the wire ([P10] in `prefetch.h`) — and what it sees is the live arena. Three runs with real GLM agents (cold start, one turn each, same task per regime), each with its honest verdict:

| blackboard | requests | bytes read from the wire | parking hits | outcome |
|---|---|---|---|---|
| 3 keys, short values | 4 (3 get + audit) | — | **3/3, hit-rate 1.00** | the agent pulled the offers and nothing else: balance **+81 bytes** |
| 40 keys, 4 namespaces | 4 | 6926 | 2 of 4 offers used | a tie (−35): a blackboard that fits in one bite is bought whole with a single `intervallo` |
| 200 keys, paragraph values | 25 | 88043 | 1 pulled, and it was noise | the agent used-checked-discarded the offer, then paid the scan; OFF (opaque keys): 13 requests, 85436 bytes, all zeros in the parking |

Answers were 8/8 exact in every run, the trap (the boars of the vegetable garden) always answered with silence, confidences always reported — honesty as an API, held by real small models. The measured verdict: **the prefetcher's gain does not scale with the number of keys — it scales with the distance between memory and context, and with the names of the keys**. With 3 talking keys the opening is a gift; at 40 the full scan costs what two offers would; at 200 template-named keys (the craftsman's repeated frags) the syntactic similarity proposed pure noise, and the 64-slot window (keys in birth order) missed the mark. Two hard facts for the declared future: descriptive keys pay only if they are also SINGULAR (repeated-name noise captures the similarity), and a wire that pages 64 items leaves the blackboard's client blind to everything born after the first page.

## The write-ahead log: memory that survives the crash (chapter 13)

Metaphor: *the ship's logbook.* The ship may sink at any wave, but the logbook is written line by line and each line is inked before the next: whoever reads it rebuilds the voyage up to the last readable line. The pain, numbered in the bench: every `salva` rewrites the whole file (64 ms at 50k keys) and between two saves everything lives only in the arena — a crash loses everything since the last snapshot.

The mechanics are one append-only file next to the db (`<db>.wal`), with contract [W1]..[W6] written before the code in `wal.h`:

- **the log comes first** [W1]: the layer writes the record BEFORE touching the engine — a crash in between re-applies at replay, the other way around would lose the accepted put.
- **the durability trittico** [W2]: `fwrite + fflush + fsync` per record. The chapter's post mortem lives in the measurement itself: fsync without fflush synchronizes a kernel that does not have the bytes yet (still in the process buffer) — the first measurement said 70 µs, the honest one 216 µs. The durability policy is closed by the numbers of `misura_fsync`, not by taste: fsync per put (216 µs, invisible at agent pace), group commit excluded because its batch NEVER fills at that pace — it would degenerate into "fsync only at save", i.e. no WAL at all.
- **the prefix is safe** [W3]: CRC-32 per record (chapter 4's contract): a broken record stops the replay right there, only the not-yet-inked tail is lost — and the rotten tail is **truncated**, because a new record must never land behind a tail the replay would never read.
- **the tick is in the record** [W4]: the replay rebuilds each entry's `nato` — TTLs come back with their true time, not the loading time. Declared limit: loading the snapshot at a tick above the records extends the replayed TTLs (the tick inside the PDB1 file is format v2, in the morgue).
- **cut after the snapshot, never before** [W5]: the WAL lives between two `salva`; a crash between snapshot and cut re-applies the last batch (idempotent), the other way around would lose it.
- **one writer** [W6]: single-threaded by contract, like the engine.

Numbers from the `demo_wal` bench (measured, gate included): exact replay of 212/212 records (200 puts + 12 deletes, one of them on a never-existing key — legal), 239 live entries rebuilt with 199 values verified one by one; the replayed TTL born at tick 1 dies at tick 6 like the live one; 50 bytes of rotten tail discarded, declared and truncated; the cut after the snapshot zeroes the segment and the next replay applies only the new batch. The price: naked put p50 0.90 µs vs inked put p50 212 µs — a 236x ratio, and at 200 puts the WAL costs 42 ms in total: durability is not free, and the bench says so instead of hiding it. From the MCP wire (`demo_mcp`): the put after the save survives the crash with no save in between — gate [W1] crosses the full wire.

## The single-custode blackboard (chapter 14)

Metaphor: *the building with one archivist.* N agents walk in through the door, each with its own counter; behind the desk there is ONE live arena, ONE WAL, ONE lock. What agent A writes, agent B reads before anyone saves: the blackboard is the live arena — the opposite of the snapshot, which is the photograph of the dead.

The road was marked by chapter 13 and by field data. Two processes on the same db were not "unsupported": they were **silent corruption** — two WALs writing over each other, no lock, nobody noticing. The arc moved one extreme at a time, each with its own contract and its own commit:

1. **[I11] one db, one padrone**: a lock (flock on Linux, LockFile on Windows) on `<db>.lock` — whoever arrives second falls loudly, with the pid tag of whoever holds the door, instead of two writers on the same file. The silent bug becomes a contract.
2. **[I12] the snapshot is all-or-nothing**: `fopen("wb")` burned half a file at a time — a crash mid-`salva` left a truncated db that the CRC rebuild declared dead at the first torn entry. Now it writes to `<nome>.tmp` and renames: rename is atomic, the window is closed.
3. **The bench before the gateway** (`demo_multiagente`, [B1]..[B6]): the question that decides a month of work — does the prefetcher's gain have a hump? — measured before building the socket. The answer: **yes** — ON-vs-OFF margin 70.9% in short, lookup-dense sessions (the sub-agent profile), 38.4% in long ones where writing turns pay handles without hitting (parking budget: −29, −19). The field verdict reproduced deterministically: the prefetcher is a cold-start optimizer. The blackboard pays, and pays most where a gateway would serve it.
4. **The gateway** (`gateway.h`/`gateway.c` + `mcp_gateway.c`, [L1]..[L6]): Redis's architecture applied to working memory — one process owns the engine, N clients speak over a TCP socket on 127.0.0.1 with the **chapter-11 wire unchanged** (`mcp_gestisci` does not change by one line: an existing MCP client just connects and works). The `mcp_stdio.c` shell remains shell-1 (one client, stdin/stdout); `mcp_gateway` is shell-2.

The blackboard contract, written before the code:

- **[L1] the blackboard is the live arena**: state written by one client is visible to the next client's get, with no save in between. Declared consequences: the WAL is the padrone's (EVERY client's puts are inked in the single segment [W1]); one client's save cuts the segment for everyone [W5] — the memory convention stays one: "save after heavy writes", whoever wrote them.
- **[L2] one custode [I11]**: the gateway IS the padrone. N clients are not N padroni: clients never touch the file.
- **[L3] the wire is chapter 11's**: JSON-RPC 2.0, one object per line, same methods.
- **[L4] nobody speaks in someone else's time**: event-loop select; a torn line stays in ITS buffer, an unread reply in ITS output queue — the slow client consumes its own queue, not other people's time.
- **[L5] the clock is the custode's lap [I8]**: everyone's requests feed the same counter — one client's TTL is shortened by other people's traffic: the blackboard is a commons even in consumption.
- **[L6] the door is narrow**: bind on 127.0.0.1, no authentication — which is exactly why the door does not face the world.

Numbers from the `demo_gateway` bench (gate included, real processes and sockets): A writes and B reads from another socket with no save [L1]; envelope and confidence cross intact [L3]; torn and interleaved lines served in order, B's queue in the right order [L4]; the custode crashing without a save loses NO client's writes — the WAL replay returns both [W1]; the second gateway falls loudly with its pid tag [L2]/[I11]. Door MEASUREMENT: in-process get 4.27 µs vs p50 12.46 µs across the socket (p99 108.88 µs — on MinGW ~96/371 µs): the kernel sits in the middle, and the event loop serves it without disdain.

The chapter's post mortem is 40 years old: **Nagle + delayed-ACK** (1984, plus Linux's delayed-ACK): the 1-byte send of the `'\n'` sat in the kernel's stomach for up to 40 ms — the bench would have hung in CI, and on Windows a benign timing hid the hole. Cure: `TCP_NODELAY` on accepted and client sockets (Redis's same pill), and deadline-bounded waits in the bench. Obituary: zero threads (one huge reply delays the others — the same renunciation as [W6]), zero keepalive in v1, no authentication (hence [L6]).

## Build and run

You need `gcc` (MinGW on Windows, gcc on Linux) and `make` (on Windows MSYS2 provides it as `mingw32-make`; by hand: `gcc -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=199309L -O2 -o motore_demo motore.c demo_motore.c`). No dependencies.

```bash
make motore_demo        # the engine + the test bench: a gate for EVERY invariant
make mcp_server         # the MCP server over stdio: ./mcp_server [file.db [tick]]
make demo_mcp           # the MCP layer bench: gates [M1]..[M9] + MEASUREMENT
make prefetch_hook      # the pre_agent hook: ./prefetch_hook [file.db]
make demo_prefetch      # the prefetcher bench: gates [P1]..[P7] + MEASUREMENT
make demo_multiagente   # the multi-session bench: the cold-start hump
make mcp_gateway        # shell-2: ./mcp_gateway [file.db [tick]] — the blackboard listening
make demo_gateway       # the blackboard bench: gates [L1]..[L6] + door MEASUREMENT
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
mcp_server.h / mcp_server.c  # the MCP core: 10 tools, contract [M1]..[M9]
mcp_stdio.c            # the thin shell: stdin/stdout, one object per line
demo_mcp.c             # the layer's bench: gates and MEASUREMENT (latency, tokens)
prefetch.h / prefetch.c      # the speculative prefetcher: contract [P1]..[P9]
prefetch_hook.c        # the pre_agent hook: trace -> prediction -> staging
demo_prefetch.c        # the prefetcher bench: gates and MEASUREMENT (hit-rate, balance)
wal.h / wal.c          # the write-ahead log: contract [W1]..[W6], the exact replay
demo_wal.c             # the WAL bench: simulated crashes, rotten tail, the cut
misura_fsync.c          # the MEASUREMENT that closed the durability decision
demo_multiagente.c      # the multi-session bench: the cold-start hump, [B1]..[B6]
gateway.h / gateway.c  # the gateway: the single-custode blackboard, [L1]..[L6]
mcp_gateway.c           # shell-2: the TCP door on 127.0.0.1
demo_gateway.c          # the blackboard bench: real processes, real sockets, door MEASUREMENT
.vibe/hooks.toml       # the hook mounted: pre_agent, fail-open
Makefile               # make motore_demo | make mcp_server | make <chapter>
.gitignore             # binaries and test files stay out of the repo
```

## Where to go next

The engine declares its limits in `motore.c`'s header: single-threaded, stats via `printf` (teaching project), answers alive until the next put. "Concurrency one extreme at a time" arrived with chapter 14 (lock, atomic snapshot, gateway) — but declared in its limits, not promised: zero threads, zero keepalive, no authentication. The natural next steps: the **living bench** — real (small, low-effort) LLM agents writing on each other through the blackboard, and the prefetcher's speculative hand-off between one agent's trace and another's get; then the tick inside the PDB1 file (format v2, for replayed TTLs under a moving clock) and stable iterators.

## License

MIT — see [LICENSE](LICENSE).
