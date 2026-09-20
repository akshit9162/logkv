# logkv

A crash-safe, append-only key–value store in C++20, built to make one question
answerable with a number rather than an opinion: **what does durability
actually cost, and what exactly is guaranteed when a write returns?**

Every mutation is appended to a single log file as a self-describing,
CRC-protected record. The in-memory index maps each key to the location of its
most recent record and is rebuilt by replaying the log at startup. A torn tail
left behind by a crash is detected and discarded rather than silently served.

```cpp
#include "logkv/store.hpp"

logkv::Store s("data.log", {.sync = logkv::Store::Sync::Batched, .batch_n = 128});
s.put("user:1", "akshit");
auto v = s.get("user:1");   // std::optional<std::string>
s.erase("user:1");
s.compact();
```

## Design

### Record layout

Little-endian, 13-byte header:

```
offset  size   field
0       4      crc32    over bytes [4, end) of this record
4       1      type     0 = PUT, 1 = TOMBSTONE
5       4      klen
9       4      vlen     (0 for a tombstone)
13      klen   key
13+klen vlen   value
```

The checksum covers the length fields as well as the payload, so a corrupt
`klen` cannot be used to mis-parse the rest of the log.

### Recovery

`open()` replays the log from offset 0, validating each record structurally and
by checksum. The first record that fails any check ends the replay: everything
from that point is a torn tail from a crash mid-append, and the file is
truncated back to the last known-good offset.

This is what keeps a partial write invisible to the caller. A store that
indexes past a torn record will eventually hand back garbage; truncating means
the failure surfaces as *absence*, which callers already handle.

Reads verify the checksum too. The index says where a record is; it does not
say the bytes are still good.

### Durability

`Sync` is explicit because the guarantee attached to a returning `put()` is the
only thing that matters under a crash:

| Mode | Guarantee on return | Worst case loss |
|---|---|---|
| `None` | in the page cache | everything not yet flushed by the OS |
| `Batched` | flushed every `batch_n` appends | up to `batch_n` records |
| `Always` | flushed to stable media | nothing acknowledged |

On macOS, `fsync()` only pushes data to the drive's write cache. `F_FULLFSYNC`
asks the drive to flush that cache to media. The difference is roughly two
orders of magnitude, so it is an explicit option rather than a hidden default.

### Compaction

`compact()` rewrites the log with only the live version of each key, then swaps
it in with `rename(2)`. The parent directory is fsynced after the rename —
without that, the rename itself can be lost on power failure even though the
new file's contents were durable.

## Results

Apple M2, 8 cores, macOS 26.5, APFS on internal SSD. 20,000 records,
128-byte values, `batch_n = 128`. Reproduce with `./build/bench 20000 128 128`.

| Mode | ops/sec | µs/op |
|---|---:|---:|
| `None` | 284,629 | 3.51 |
| `Batched` (n=128) | 34,387 | 29.08 |
| `Always` (F_FULLFSYNC) | 456 | 2195.27 |

| Operation | ops/sec | µs/op |
|---|---:|---:|
| point `get` | 647,856 | 1.54 |
| recovery replay | 490,949 | 2.04 |

**Durability costs 624× throughput on this hardware.** Batched group commit
recovers 75× of that gap while bounding worst-case loss to 128 records — which
is the actual engineering decision this store exists to make legible.

The `Always` figure is ~2.2 ms per write, which is a real media flush and not a
measurement artefact. A store reporting six-figure "durable" writes per second
on a laptop is measuring the drive's write cache, not durability.

## Crash testing

`tools/crashtest.sh` runs the writer under `SIGKILL` at unpredictable points
mid-append. SIGKILL is the point: no destructor runs, no buffer is flushed on
the way out, nothing gets a chance to tidy up. What survives is exactly what
the durability path actually guaranteed.

The writer prints an index after every `put()` that returns. The verifier then
asserts that **every acknowledged write is present with the correct value**.

```
$ ./tools/crashtest.sh 8 build
round 3: PASS  acked=115 present=115 missing=0 wrong=0 recovered=116 truncated=0
round 4: PASS  acked=223 present=223 missing=0 wrong=0 recovered=224 truncated=0
...
all rounds passed: no acknowledged write was lost
```

Note `recovered=116` against `acked=115`. One record landed but the process
died before printing its acknowledgement. That asymmetry is correct and
deliberate: an acknowledged write is never lost, and an unacknowledged one may
survive. The reverse would be the bug.

## Tests

40 assertions covering round-trips, overwrite ordering, tombstones surviving
reopen, empty and large values, move semantics, compaction correctness, and two
failure-injection cases:

- **torn tail** — bytes chopped off the log; earlier records survive, the
  partial record is discarded, the file is truncated to the last good offset.
- **bit flip** — a single flipped bit inside a committed record is caught by
  the checksum rather than returned to the caller.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/test_store
./tools/crashtest.sh 10 build
./build/bench
```

## Sanitizers

UBSan is clean across the suite. ASan cannot run on this machine — Apple clang
14.0.3 on macOS 26 fails inside the ASan runtime's own initialisation
(`sanitizer_malloc_mac.inc:189`), which is a toolchain incompatibility rather
than a finding. Both sanitizers run in CI on Linux, where that runtime works:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOGKV_SANITIZE=ON
cmake --build build-asan -j4 && ./build-asan/test_store
```

## Limitations

Honest scope. This is a single-node, single-threaded store built to explore
durability and crash recovery, not to compete with a production engine.

- **Not thread-safe.** One writer, no internal locking.
- **The index is fully in memory**, so the key set must fit in RAM. Values live
  on disk and are read on demand.
- **Recovery is O(log size)** — it replays every record. A production engine
  would checkpoint the index (a hint file) and replay only the tail.
- **No range scans or ordered iteration.** A hash index, not an LSM tree, so
  there is no sorted structure to scan.
- **Compaction is stop-the-world** and holds the full new index in memory while
  running.
- **No group-commit thread.** `Batched` flushes on the writing thread every
  `batch_n` appends; a real implementation would flush on a timer too, so a
  quiet period cannot leave writes unflushed indefinitely.

## Next

The natural progressions, roughly in order of value:

1. **Hint file** on compaction so recovery is O(live keys) rather than O(log).
2. **Time-based group commit** alongside the count-based trigger.
3. **Multiple log segments** with background compaction, instead of one file.
4. **An LSM path** — memtable, sorted SSTables, levelled compaction, bloom
   filters — which buys ordered iteration and range scans.
5. **Raft replication** across three nodes, at which point the interesting
   question becomes linearizability rather than durability.
