[![ci](https://github.com/SeraFRA/ddbb/actions/workflows/ci.yml/badge.svg)](https://github.com/SeraFRA/ddbb/actions/workflows/ci.yml)

**An in-memory key-value store in pure C11 — native TTL, a WAL that survives crashes, a shared blackboard for agents, and an MCP layer that turns it into a tool any LLM agent can call.**

Zero dependencies: C11 and the standard library, one binary. Every claim in this README is a gate that runs in CI at every push; every number is measured, never promised. The full story of the project — the educational path, all fourteen chapters with their post mortems, real agent runs — is in Italian in [didattica/README.md](didattica/README.md).

## TL;DR

- **What it is**: an in-process KV engine with TTL as a *contract* (timing wheel, batch O(1) death — expired memory is gone, not down-ranked), range scans, syntactic similarity with **confidence declared on every single answer**, a write-ahead log, and an MCP server with two shells: stdio and a single-owner TCP gateway (N clients, one custode).
- **What it is NOT**: no SQL, no schema, no embeddings (similarity is syntactic — it catches typos, not synonyms), no replication, no threads, single file. The limits are declared, not discovered in production.
- **Who it's for**: agent working memory — the profile that writes a lot and queries selectively. A `put` is microseconds and zero LLM calls; extraction-based memory systems (Mem0/Zep-class) pay an LLM round per ingested message instead.

```bash
make motore_demo && ./motore_demo   # engine + bench: EXIT!=0 if a contract breaks
make mcp_server                    # the MCP server over stdio: ./mcp_server [file.db [tick]]
make demo_mcp                      # the MCP layer bench: gates [M1]..[M11] + MEASUREMENT
```

You need `gcc` and `make` (on Windows: MinGW + `mingw32-make`). Any chapter also builds alone: `make database_quotient`.

## Measured, not promised

| what | the number | proven by |
|---|---|---|
| exact get | p50 2.1 µs / p99 2.3 µs over the MCP wire (0.10 µs in-process) | `demo_mcp`, every CI run |
| fuzzy query (one-digit typos) | p50 9.5 µs, truth in the top-8 at 88% | `demo_mcp` |
| batch presence, 64 keys | p50 15 µs, zero false negatives | `demo_mcp` |
| TTL death | 20,000 expiries in the same tick in 0.1 ms, **without reading a key** | `motore_demo` |
| durable put (WAL) | p50 216 µs vs 0.90 µs naked — a 236× ratio, declared not hidden | `prove/misura_fsync.c` |
| crash replay | 212/212 records exact, rotten tail truncated | `demo_wal` |
| across the gateway door | in-process 4.27 µs → p50 12.46 µs over TCP (the kernel sits in the middle) | `demo_gateway` |
| cold start with the prefetcher | hit-rate 1.00 with 3 talking keys (+81 bytes); a tie at 40; noise at 200 — the hump is published, not promised | `demo_multiagente`, real GLM-agent runs |

The economics in one line: an LLM round-trip costs 10²..10³ ms of generation; the engine answers under the millisecond at p99 — so the cost of any task is **the round**, and the layer is built to pay as few as possible (one `chiedi` returns key AND value AND confidence; `intervallo` returns the batch plus the total).

Positioning, stated honestly: this is not the extraction plane of Mem0/Zep/Letta — it is **the storage plane below them**. The fair comparison is Redis as a backend, for embedded/edge memory where one LLM call per `add()` is unacceptable. The full study — use cases, field numbers, feature-by-feature comparison with the memory-server market — is [`studio-utilita.md`](studio-utilita.md) (Italian only).

## The product

```
motore.h / motore.c              the engine: public API, opaque handle, written contract
wal.h / wal.c                    the write-ahead log: [W1]..[W6], exact replay
json.h / json.c                  the minimal JSON parser: one source of truth for the wire
mcp_server.h / mcp_server.c      the MCP core: 10 tools, contract [M1]..[M11]
mcp_stdio.c                      shell-1: stdin/stdout, one JSON object per line
prefetch.h / prefetch.c          the speculative prefetcher: [P1]..[P10]
prefetch_hook.c                  the pre_agent hook: reads the agent's reasoning trace,
                                 predicts up to 4 keys, stages them with a TTL
gateway.h / gateway.c            the blackboard: one custode [I11], N clients, [L1]..[L6]
mcp_gateway.c                    shell-2: the TCP door on 127.0.0.1
prove/                           the benches: a numbered gate per invariant, + MEASUREMENTs
didattica/                       the educational path (closed) + the full story, in Italian
```

Signature contract lines:

- **[I7] every live entry IS the current version of its key** — every death (delete, expiry, supersede) marks and counts at the same instant.
- **[I11] one db, one owner** — the second process to open the same file falls loudly, never two silent writers on one WAL.
- **Honesty as an API**: exact hit → confidence 1.0; fuzzy → the value with a declared similarity; nothing → silence; the Bloom filter's "maybe" is distinct from the index's "no". The audit tool makes a broken contract visible to the agent too.
- **The clock belongs to the caller [I8]**: no wall-clock time inside the engine's decisions.
- **The speculative prefetcher (chapter 12)**: a hook watches the agent's reasoning trace and stages the keys it will likely need — with built-in forgetting (the wrong predictions die on their own in the TTL) and a measured byte balance. The gain is a cold-start hump, and the disproof is published with the same font as the proof.

Every gate above runs in CI on every push, with `-Werror` and ASan+UBSan: a broken contract turns the badge red. The contracts were written before the code — the full list with post mortems is in [didattica/README.md](didattica/README.md).

## The chapters (the educational path, in [didattica/](didattica/))

Each builds alone, each is a bench. The metaphors that generated them — and every post mortem — are in [didattica/README.md](didattica/README.md).

| # | the real world calls it | file |
|---|---|---|
| 0 | the fossil: the original code, untouched, with its didactic bugs | `didattica/database.c` |
| 2 | **Bitcask** (append-only log + hash in RAM) | `didattica/database_indexed.c` |
| 3 | the LSM **merge** | `didattica/database_compact.c` |
| 4 | checksum + snapshot (SQLite, RocksDB) | `didattica/database_persist.c` |
| 5 | **learned index** (Kraska 2017) | `didattica/database_learned.c` |
| 6 | **MinHash/LSH banding** (Broder 1997) | `didattica/database_semantic.c` |
| 7 | **timing wheel** (Linux kernel) | `didattica/database_ring.c` |
| 8 | **Bloom 1970 / quotient filter** (Bender 2012) | `didattica/database_quotient.c` |
| 9 | the fusion: the invariants-first experiment | `didattica/database_persist_semantic.c` |
| 11 | **MCP** — the engine as an agent tool (capitoli 11-14 in the story) | `mcp_server.c` |

## Where to read more

- [didattica/README.md](didattica/README.md) (Italian) — the full story: the human-AI collaboration, metaphors translated into mechanics, every chapter's post mortem, the living-bench runs with real small agents on the blackboard.
- [studio-utilita.md](studio-utilita.md) (Italian) — the utility study: when this helps, how much (field numbers), how original it is, and the honest comparison with the memory-server market.
- [AGENTS.md](AGENTS.md) — the collaboration method this repo runs on, reusable elsewhere.

## License

MIT — see [LICENSE](LICENSE).
