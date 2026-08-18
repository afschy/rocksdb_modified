# Key Lookup Tracer v2 -- Extension Plan

Extends the shipped v1 tracer (see `key_lookup_tracer.md` for the v1 usage
guide and `key_lookup_tracer_plan.md` for the original build plan) into a
trace suitable for driving an offline LSM block cache simulator.

The v1 tracer answers "which files and blocks did this `Get()` touch, and what
was the outcome". v2 adds the three things a cache simulator additionally
needs: how many bytes each access moves, a total order over all accesses, and
the accesses that do not come from user `Get()` at all.

Output stays plain text. A binary format was considered and rejected: the
reader, dumper tool, and four build-system updates are not worth the
maintenance for an experiment runner. Optional on-the-fly zstd compression
(Phase 0) covers the size problem instead, at a better ratio than the binary
format would have achieved and for a fraction of the code.

---

## 0. What you are building

Five independent increments on top of v1, in this order:

| Phase | Adds | Hot path | Risk |
|-------|------|----------|------|
| 0 | Optional on-the-fly zstd compression | no (writer only) | low |
| 1 | Block sizes: bytes read and bytes cached | yes, trivial | low |
| 2 | Global sequence numbers | yes, one atomic | low |
| 3 | File lifecycle records (`create`/`delete`/`move`) | no | low |
| 4 | Iterator access records, grouped per file | yes, new | moderate |

Phase 0 goes first because it is confined to `KeyLookupTraceWriter`, is
testable against the existing v1 format, and makes the much larger output of
Phases 1-4 tolerable while they are being developed.

Phases 1 and 2 both change the `G` line, so they ship as a single format
version bump (v1 -> v2) implemented in two commits. Phases 3 and 4 are
additive record types and do not disturb `G` parsing.

---

## 1. Scope

### In scope

* Data block accesses from user `Get()` (already built, extended here).
* Data block accesses from every `BlockBasedTableIterator`, which covers user
  iterators, compaction, external SST ingestion, verification, and prefetch.
* Every change to the live file set, including trivial moves.
* Byte counts for both the on-disk read and the in-cache footprint.
* A total order across all record types.

### Out of scope

* Index and filter blocks. Deliberately excluded, as in v1. Section 8 of the
  v1 plan sketches the extension if it is ever wanted.
* `MultiGet` and merge operands from user commands. Assumed absent for this
  workload; the code paths still function, they are simply not a design target.
* Manual compaction. Assumed absent.
* Cross-file scan identity. `A` records group per file, not per user iterator.
  See section 2, decision 5.
* Replay. This trace is consumed by an offline simulator, not fed back into
  RocksDB.

---

## 2. Decisions already made

Recorded here so the rationale is not lost. All were settled before
implementation started.

**1. Both read size and cache footprint.** `handle.size()` is the *compressed*
size on disk; the block cache stores blocks *uncompressed*; compression
defaults to Snappy. These differ by 2-4x on a typical DB. Simulating I/O cost
needs the first, simulating cache capacity needs the second, so both are
recorded.

**2. Integer bytes, not fractional KB.** Float formatting is roughly an order
of magnitude slower than integer formatting and this is a hot path. Decimal KB
is also lossy (0.001 KB = 1.024 bytes) and longer on the wire than the raw
integer. The header declares `size_unit=bytes`; divide by 1024 in analysis.

**3. Record-level sequence numbers in addition to per-block ones.** A `Get`
whose files are all filtered out reads zero blocks. With only per-block
sequence numbers such a lookup has no position in the global order, and
filter effectiveness is precisely the kind of thing this trace should measure.
The record's sequence number is allocated at operation *start*, so it always
precedes its own block sequence numbers and a sort by sequence number yields
causal order.

**4. One file, not two.** Lifecycle records need sequence numbers from the same
counter as block accesses so that a file deletion is orderable against the
reads around it. Two files would mean reconciling one counter across two
streams for no gain.

**5. `A` records group per file.** A `BlockBasedTableIterator` is constructed
per SST file, so "per file" and "per table iterator" are the same grouping, and
the group boundary is an object that already exists at the hook site and can
own the buffer. This also bounds buffer memory by blocks-per-file rather than
by scan length. The cost is that cross-file scan identity is lost: seven table
iterators belonging to one user scan are not linked. Two mitigations: the
global sequence numbers interleave one scan's blocks across files in a
recognizable pattern, and adding a scan id later is an additive field rather
than a restructure.

**6. Grouped `A` records, flushed at a threshold.** Literal grouping without a
threshold would buffer an entire file's worth of blocks (a 256 MB SST at 4 KB
blocks is ~64K entries, ~1.5 MB) and would not write anything until the
iterator closed. Flushing every `kIterFlushThreshold` blocks caps this, with
`iter_id` stitching the continuation records back together.

**7. Text output.** See preamble.

**8. Compression instead of a binary format.** The trace is comma-separated
small integers with monotonic sequence numbers, file numbers that repeat for
thousands of consecutive records, and sizes clustering at `block_size` -- close
to a best case for zstd. Expect roughly 5-10x, which should exceed what a
binary varint-plus-delta encoding would have achieved (~5x), for roughly 80
lines of writer code instead of a reader, a dumper tool, and four build-system
updates. Text, `check_trace.py`, and human readability via `zstd -d | head` all
survive.

---

## 3. Output format v2

### 3.1 Header

```
# rocksdb_key_lookup_trace v2 block_id_mode=<ordinal|offset> size_unit=bytes rocksdb=<MAJOR>.<MINOR>
```

Bump from `v1` to `v2`. Parsers must reject `v1` rather than misread it: the
`G` line gained a leading type field, so a v1 parser would silently
misinterpret every column.

### 3.2 Shared block tuple

Identical in `G` and `A` records, so one parser handles both:

```
block := <blk_id>/<blk_seq>/<read_bytes>/<uncomp_bytes>
```

* `blk_id` -- ordinal or raw offset per `block_id_mode`; `18446744073709551615`
  (`UINT64_MAX`) when unresolvable, as in v1.
* `blk_seq` -- global sequence number of this block access.
* `read_bytes` -- `handle.size() + BlockBasedTable::kBlockTrailerSize`, i.e.
  bytes pulled from disk including the 5-byte trailer.
* `uncomp_bytes` -- size of the materialized block, i.e. its footprint in the
  block cache. `0` if the block failed to materialize.

### 3.3 `G` records -- point lookups

```
G,<seq>,<ts_us>,<lookup_id>,<cf_id>,<final_result>,<num_probes>,<probe>|<probe>|...

probe := <level>:<file_number>:<outcome>[:<block>]*
```

`seq` is allocated at lookup start. `lookup_id` retains its v1 meaning and
sampling semantics.

### 3.4 `F` records -- file lifecycle

```
F,<seq>,<ts_us>,<op>,<cf_id>,<file_number>,<level>[,<to_level>]

op := create | delete | move
```

`to_level` is present only for `move`, where `level` is the source level.

### 3.5 `A` records -- iterator accesses

```
A,<seq>,<ts_us>,<cf_id>,<caller>,<iter_id>,<level>,<file_number>,<no_insert>,<num_blocks>,<block>:<block>:...
```

* `seq` -- allocated when the group starts, i.e. at each flush.
* `caller` -- numeric `TableReaderCaller` (see 3.6).
* `iter_id` -- unique per `BlockBasedTableIterator` instance. Required both to
  stitch continuation records and to distinguish two concurrent scans over the
  same file, which would otherwise be indistinguishable.
* `no_insert` -- `1` when `read_options.fill_cache` is false. Compaction sets
  this. Such an access performs a cache *lookup* (and can hit, promoting
  recency) but never inserts. See section 5, pitfall 6.

### 3.6 Enumerations

`final_result` (`KeyLookupResult`) and `outcome` (`KeyLookupOutcome`) are
unchanged from v1.

`caller` is `TableReaderCaller` from `include/rocksdb/table_reader_caller.h`,
values 1..14. Note it is `enum : char`, so 16 bits suffice for a mask.

### 3.7 Truncation

Unchanged from v1: a single `# truncated` line, then no further records. The
size limit now applies across all three record types, so a run with `A`
records enabled will hit it far sooner. Size `max_trace_file_size`
accordingly.

With Phase 0 compression enabled, `max_trace_file_size` counts *compressed*
bytes actually written, since that is what the limit exists to bound. The
consequence is that the check happens per batch rather than per record, so the
file can overshoot by at most one compressed batch. The `# truncated` marker
goes into the staging buffer like any other line, so it ends up inside the
final frame and survives decompression.

---

## 4. Implementation phases

### Phase 0 -- Optional on-the-fly zstd compression

**Files:** `include/rocksdb/key_lookup_trace_options.h`,
`trace_replay/key_lookup_tracer.h`, `trace_replay/key_lookup_tracer.cc`.

Confined to the writer. No format change, no hot-path change.

1. **Options.** Add to `KeyLookupTraceOptions`:

   ```cpp
   // kNoCompression (default) writes plain text. kZSTD writes a standard
   // .zst file. Any other value is rejected by StartTrace.
   CompressionType compression = kNoCompression;

   // zstd level. 1 is recommended: the trace is enormously redundant, so
   // most of the ratio is available at a fraction of the CPU cost, which
   // matters because compression runs under trace_writer_mutex_.
   int compression_level = 1;
   ```

2. **Use the in-tree streaming API.** `StreamingCompress::Create(kZSTD, opts,
   compress_format_version, max_output_len)` from `util/compression.h:651`.
   This is live production code backing WAL compression
   (`db/log_writer.cc:140`) with coverage in `db/log_test.cc:1215`, not a dead
   path. Follow `log_writer`'s buffer-management pattern, but *not* its
   per-record `Reset()` -- see step 3.

3. **Batch, do not compress per record.** `ZSTDStreamingCompress::Compress`
   calls `ZSTD_compressStream2` with `ZSTD_e_end`
   (`util/compression.cc:229`), so **every call terminates a frame**.
   Compressing one record at a time would put a 9-13 byte frame header on a
   ~100 byte record and restart the window each time, yielding near-zero or
   negative compression. This is precisely why WAL compression has mediocre
   ratios: `log_writer` needs record-level recoverability and pays for it.

   Instead accumulate records in a staging `std::string` and compress when it
   crosses `kCompressBatchSize` (256 KB recommended). One batch, one frame.

4. **Buffer sizing.** Allocate the output buffer at
   `kCompressBatchSize + kCompressBatchSize / 8 + 1024`, safely above
   `ZSTD_compressBound` for any input of that size, so a single `Compress()`
   call consumes the whole batch. Still honor the documented contract and loop
   while the return value is greater than 0, passing the same input pointer.
   A negative return is an error; propagate it as a `Status` and stop tracing.

5. **Compress the header too**, so the file is a pure `.zst`. A plaintext
   header followed by compressed bytes produces a file that is neither valid
   text nor valid zstd. Use a `.zst` suffix.

6. **Flush** the staging buffer on `EndTrace()` and in
   `~KeyLookupTraceWriter()`, before closing the file.

7. **Fall back cleanly.** `StreamingCompress::Create` returns `nullptr` when
   zstd is not compiled in. Treat that as a `Status::NotSupported` from
   `StartTrace` rather than silently writing plain text under a `.zst` name.

**Why concatenated frames are fine.** Independent zstd frames concatenated
into one file are a documented, standard-decodable format: `zstd -d` and
Python's `zstandard`/`zstd.open()` both handle them transparently. The output
is a genuine `.zst` file, not a RocksDB-specific container. Independent frames
also mean a truncated trace stays decodable up to the last complete frame,
which is better than the torn final line plain text can leave behind.

**Verification:** `zstd -d trace.zst | head -1` prints the header line, and a
decompressed trace is byte-identical to the same workload traced without
compression.

---

### Phase 1 -- Block sizes

**Files:** `trace_replay/key_lookup_tracer.h`, `trace_replay/key_lookup_tracer.cc`,
`table/get_context.h`, `table/block_based/block_based_table_reader_sync_and_async.h`,
`db/version_set_sync_and_async.h`, `db/key_lookup_trace_scope.h`.

1. Replace the flat `autovector<uint64_t, 16>` block buffer with a struct:

   ```cpp
   struct KeyLookupBlockAccess {
     uint64_t block_id = 0;
     uint64_t seq = 0;          // populated in Phase 2
     uint32_t read_bytes = 0;
     uint32_t uncomp_bytes = 0;
   };
   using KeyLookupBlockIds = autovector<KeyLookupBlockAccess, 8>;
   ```

   **Reduce the inline capacity from 16 to 8.** The element grew from 8 to 24
   bytes; keeping 16 inline would put 384 bytes of buffer on the stack of every
   `Version::Get`, traced or not. A typical point lookup reads one to three
   blocks in total, so 8 is ample and spilling is rare.

2. Update `GetContext::SetBlockSink` and `block_sink()` to the new element type.

3. At the existing hook in
   `table/block_based/block_based_table_reader_sync_and_async.h`, push an entry
   with `block_id` and `read_bytes` filled in:

   ```cpp
   sink->push_back({BlockIdForTrace(...), /*seq=*/0,
                    static_cast<uint32_t>(v.handle.size() +
                                          BlockBasedTable::kBlockTrailerSize),
                    /*uncomp_bytes=*/0});
   ```

4. **Patch `uncomp_bytes` after `NewDataBlockIterator` returns.** The
   uncompressed size does not exist before the block materializes. Keep the
   *recording decision* where it is -- that is what preserves cache
   independence -- and write the size back into `sink->back()` once the call
   returns. This stays cache independent because the block materializes on both
   a hit and a miss; only the source differs. Leave `0` if the block failed to
   materialize.

5. Emit both fields in `KeyLookupTraceWriter::WriteLookup` using
   `AppendNumberTo`, per the tuple in 3.2.

**Verification:** a `Get` on a known SST reports `read_bytes` matching
`handle.size() + 5` from `ldb --command=dump_live_files`, and `uncomp_bytes`
near `block_size` and strictly greater than `read_bytes` under Snappy.

---

### Phase 2 -- Sequence numbers

**Files:** `trace_replay/key_lookup_tracer.h`, `trace_replay/key_lookup_tracer.cc`,
`db/key_lookup_trace_scope.h`, `db/version_set_sync_and_async.h`,
`table/block_based/block_based_table_reader_sync_and_async.h`.

1. Add to `KeyLookupTracer`:

   ```cpp
   std::atomic<uint64_t> seq_counter_{1};
   uint64_t NextSeq() { return seq_counter_.fetch_add(1, std::memory_order_relaxed); }
   ```

   Reset in `StartTrace` alongside `lookup_id_counter_`.

2. Allocate a record sequence number in `Version::Get` at the same point
   `lookup_id` is allocated, i.e. before the file loop, and store it on
   `KeyLookupTraceScope`.

3. Allocate a block sequence number at each block hook, into the entry pushed
   in Phase 1.

4. Emit `seq` as the second field of the `G` line and `blk_seq` inside each
   block tuple.

**Why an atomic is acceptable here.** A globally shared atomic on a read path
would normally be a contention concern. It is not one in this context: every
traced access already pays for string formatting and a mutex-serialized
`WritableFile::Append` in `KeyLookupTracer::WriteLookup`, and a relaxed
`fetch_add` is noise next to a mutex acquisition. When tracing is off the
counter is never touched.

**Why timestamps cannot substitute.** The `G` record's timestamp is taken in
`~KeyLookupTraceScope`, that is, *after* its blocks were read. Under
concurrency an `A` record emitted during that lookup receives an earlier
timestamp than the `G` record that logically precedes it. Ordering a replay by
timestamp therefore silently reorders the access stream.

---

### Phase 3 -- File lifecycle records

**Files:** `trace_replay/key_lookup_tracer.h`, `trace_replay/key_lookup_tracer.cc`,
`db/version_set.cc`.

1. Add `KeyLookupTracer::WriteFileLifecycle(...)` and a corresponding
   `KeyLookupTraceWriter` method emitting the `F` line from 3.4.

2. Hook the virtual
   `LogAndApply(cfds, read_options, write_options, edit_lists, ...)` at
   `db/version_set.h:1330`. Both convenience overloads (`:1290`, `:1310`)
   delegate into it, so it is the single funnel for every change to the live
   file set: flush, compaction, trivial move, ingestion, and recovery.

3. **Emit only after the manifest write succeeds**, so an edit that failed
   never appears in the trace.

4. Classify each `VersionEdit`:

   ```cpp
   // VersionEdit::GetDeletedFiles() -> std::set<std::pair<int, uint64_t>>
   // VersionEdit::GetNewFiles()     -> vector<std::pair<int, FileMetaData>>
   //
   // A file number present in BOTH within one edit is a trivial move: the
   // file was not rewritten, only relabeled to a different level.
   ```

   Build a small hash set of new file numbers, then partition the deleted set
   against it. Emit `move` for the intersection (source level from the deleted
   entry, `to_level` from the new entry), `delete` for the remainder of the
   deleted set, and `create` for the remainder of the new set. This is O(n log n)
   on a cold path with n typically in the single digits.

**Why move detection is a correctness requirement, not a nicety.** Without it a
trivial move appears as `delete(f, L)` followed by `create(f, L+1)`. A
simulator that invalidates cached blocks on file deletion would discard blocks
that are byte-identical and still live, injecting phantom misses on exactly the
cold lower-level data that gets trivially moved most often.

**Why lifecycle records are worth more per byte than anything else here.** A
handful of records per compaction rather than millions, on a cold path, and
they unlock something otherwise invisible: RocksDB does not proactively evict
cached blocks belonging to a deleted SST, so they occupy cache as dead weight
until LRU pushes them out. Modelling an invalidating cache manager is
impossible without these records.

---

### Phase 4 -- Iterator access records

**Files:** `include/rocksdb/key_lookup_trace_options.h`,
`trace_replay/key_lookup_tracer.h`, `trace_replay/key_lookup_tracer.cc`,
`table/table_builder.h`, `db/table_cache.cc`,
`table/block_based/block_based_table_reader.h`,
`table/block_based/block_based_table_reader.cc`,
`table/block_based/block_based_table_iterator.h`,
`table/block_based/block_based_table_iterator.cc`.

1. **Options.** Add to `KeyLookupTraceOptions`:

   ```cpp
   // Master switch for A records. Checked at the hook, so false means no
   // collection at all, not merely suppressed output.
   bool record_iterator_accesses = true;

   // Trace an iterator access only if (1 << caller) is set. TableReaderCaller
   // has 14 values, so 16 bits suffice. Defaults to every caller.
   uint16_t iterator_caller_mask = 0xFFFF;
   ```

   The mask matters in practice: compaction will outweigh user iterators by one
   to two orders of magnitude on any write-heavy workload, and
   `~(1u << kCompaction)` recovers user-iterator behavior at a fraction of the
   trace size.

2. **Plumb the tracer to the table reader**, mirroring `block_cache_tracer`
   exactly. It already travels VersionSet -> ColumnFamilySet ->
   ColumnFamilyData -> TableCache -> `TableReaderOptions::block_cache_tracer`
   -> `BlockBasedTable::block_cache_tracer_`
   (`block_based_table_reader.h:537`). Add `key_lookup_tracer` alongside it.
   `db/table_cache.cc:183-192` constructs `TableReaderOptions` and already
   passes `block_cache_tracer_` there, so this is a one-line addition at the
   same site.

3. **Store the file number on `Rep`.** `Rep` has `level` but no file number.
   `TableReaderOptions::cur_file_num` is already populated by `TableCache` from
   `file_meta.fd.GetNumber()` (`db/table_cache.cc:189`); save it on `Rep`.

   Do **not** reach for `BlockCacheTraceRecord::sst_fd_number` as a model: that
   field is declared and serialized but never populated anywhere in `table/`,
   so it is always 0.

4. **Buffer on the iterator.** Add to `BlockBasedTableIterator`:

   ```cpp
   uint64_t iter_id_ = 0;                       // 0 when not tracing
   autovector<KeyLookupBlockAccess, 16> klt_blocks_;
   ```

   `caller` is already available as `lookup_context_.caller` -- the constructor
   at `block_based_table_iterator.h:26` takes `TableReaderCaller caller` and
   stores it. No plumbing needed.

5. **Hook `InitDataBlock`** (`block_based_table_iterator.cc:386`), covering all
   three paths in it: the `multi_scan_read_set_` path, the
   `DoesContainBlockHandles()` prefetched path, and the regular path. Also hook
   `AsyncInitDataBlock` (`:498`). Record `read_bytes` from the handle at the
   hook and patch `uncomp_bytes` after the block materializes, as in Phase 1.

   Do **not** try to unify this with the `Get` hook inside
   `NewDataBlockIterator`. That function does not know which iterator called it,
   and per-file grouping requires iterator identity.

6. **Flush** when `klt_blocks_.size() >= kIterFlushThreshold` (default 1024,
   capping buffer growth at ~24 KB per live iterator) and in
   `~BlockBasedTableIterator` (`block_based_table_iterator.h:53`). Each flush
   allocates a fresh record `seq`; `iter_id` stitches them.

7. **Level.** Use `rep_->level_for_tracing()`, which already returns
   `level >= 0 ? level : UINT32_MAX` and is the established convention. See
   pitfall 1 below.

---

## 5. Invariants and pitfalls

1. **`Rep::level` goes stale after a trivial move.** The field is documented in
   `block_based_table_reader.h:871` as "the level when the table is opened,
   could potentially change when trivial move is involved". So the `level` in
   an `A` record is the level at table-open time, which a Phase 3 `move` record
   may have superseded. Treat `F` records as authoritative for level and the
   `A` record's level as a hint. Document this in the usage guide; do not try
   to fix it by mutating `Rep`.

2. **Destructor ordering in `db/version_set_sync_and_async.h`.** The block and
   probe buffers must be declared *before* `klt_scope`. Destructors run in
   reverse declaration order, so the scope is destroyed first and the buffers
   it reads are still alive. This comment already exists in the file; do not
   let a refactor reorder it.

3. **`autovector` is not contiguous.** It spills to a heap `std::vector` past
   its inline capacity, so it has no `data()`. Pass it by reference and index
   with `operator[]`; never take a pointer-plus-size.

4. **Iterator lifetime versus `EndTrace()`.** A `BlockBasedTableIterator` can
   outlive a call to `EndTrace()`. Its destructor flush must re-check
   `is_tracing_enabled()` and tolerate a null writer rather than assuming the
   tracer that was live at construction is still live. The existing
   `WriteLookup` null-check pattern covers this; follow it.

5. **The no-I/O contract.** `BlockIdForTrace` must keep returning
   `UINT64_MAX` rather than building the offset map when
   `read_options.read_tier == kBlockCacheTier`. Phase 4 multiplies the number
   of call sites that can trip this.

6. **`fill_cache=false` gates insertion, not lookup.** In
   `MaybeReadBlockAndLoadToCache`, `GetDataBlockFromCache` runs whenever a block
   cache exists and `use_block_cache_for_lookup` is true; only the subsequent
   read-and-insert branch tests `ro.fill_cache`. `ShouldUseDataBlockCacheForIterator`
   returns `table_options.block_cache != nullptr` and consults neither
   `fill_cache` nor `for_compaction`. Compaction therefore *does* probe the
   cache and can hit, promoting recency. This is what `no_insert` exists to
   record; do not "optimize" it away on the assumption that compaction bypasses
   the cache.

7. **Ordinal mode costs more in Phase 4.** The per-file offset map is built
   lazily on first use. Compaction touches every file in the DB, so ordinal
   mode will build it for every file. Prefer `kOffset` for runs with `A`
   records enabled and convert offline.

8. **Recovery is not covered.** Several `VersionSet` construction sites
   (`db/repair.cc`, `db/version_util.h`, `tools/ldb_cmd.cc`) pass a null
   tracer, so `F` records will not cover recovery or repair paths. This is
   intentional -- those paths do not represent steady-state workload -- but the
   usage guide must say so, or an analyst will read the gap as a bug.

9. **The truncation limit is now shared.** With `A` records enabled the trace
   reaches `max_trace_file_size` far sooner and truncation will cut the run
   mid-workload. Size it deliberately.

10. **Sampling and cache simulation are in tension.** `sampling_frequency`
    drops whole lookups uniformly, which breaks the per-block histories a cache
    simulator needs. It remains available for cheap workload characterization,
    but a simulation run should use `sampling_frequency = 1` and control volume
    with `iterator_caller_mask` instead.

11. **Never call `Compress()` per record.** `ZSTD_e_end` ends a frame on every
    call. Per-record compression yields near-zero or negative ratios. This is
    the single mistake that makes Phase 0 pointless, and it is easy to make by
    copying `db/log_writer.cc` verbatim, since the WAL deliberately does
    exactly that for record-level recoverability.

12. **Compression runs under `trace_writer_mutex_`.** Flushing a 256 KB batch
    takes a few hundred microseconds at level 1 and blocks every other tracing
    thread for that time. This converts a steady small per-record cost into a
    periodic large one, which is visible in tail-latency measurements of the
    traced DB. Acceptable when the trace contents are the object of study;
    reduce `kCompressBatchSize` or move compression to a background thread if
    the traced DB's own latency distribution matters.

13. **Crash loses a batch, not a line.** With compression enabled, an unflushed
    staging buffer is lost on crash -- up to `kCompressBatchSize` of records
    rather than one line. Tune the batch size down if that matters. The upside
    is that everything up to the last complete frame stays cleanly decodable,
    where plain text can leave a torn final line.

---

## 6. Performance requirements

Unchanged in spirit from v1, with two additions.

* **Tracing off must match baseline within noise.** Every new hook is behind an
  `UNLIKELY` null or bool check. Phase 4 adds checks to the iterator hot path
  that did not previously have any, so this must be measured, not assumed.
* **Phase 4 with `record_iterator_accesses = false` must match baseline within
  noise.** The master switch is checked at the hook, so a disabled run should
  cost one predictable branch per block.

```
# baseline
db_bench --benchmarks=readrandom,seekrandom,fillrandom

# tracing off (must match baseline)
# tracing on, A records off  (must match baseline)
# tracing on, A records on, caller mask = all
# tracing on, A records on, caller mask = ~(1 << kCompaction)
# tracing on, compression = kZSTD (compare throughput and p99 to uncompressed)
```

For the compressed configuration, report p99 and p99.9 read latency alongside
throughput, not just the mean. Pitfall 12 predicts a periodic stall under the
trace mutex; the mean will hide it and the tail will not.

Build release for all of these: `AUTO_CLEAN=1 DEBUG_LEVEL=0 make db_bench`.

---

## 7. Verification checklist

- [ ] `zstd -d trace.zst | head -1` prints the header line.
- [ ] A decompressed trace is byte-identical to the same workload traced with
      `compression = kNoCompression`.
- [ ] Standard tooling reads the concatenated frames: both `zstd -d` and
      Python `zstandard` decode the whole file, not just the first frame.
- [ ] Compression ratio measured and recorded. If it is below ~3x, the writer
      is almost certainly compressing per record rather than per batch
      (pitfall 11).
- [ ] `compression = kZSTD` on a build without zstd returns
      `Status::NotSupported` from `StartTrace` rather than writing plain text
      under a `.zst` name.
- [ ] A trace truncated mid-run by `max_trace_file_size` still decodes, and the
      `# truncated` marker survives decompression.
- [ ] `read_bytes` matches `handle.size() + 5` cross-checked against
      `ldb --command=dump_live_files`.
- [ ] `uncomp_bytes > read_bytes` under Snappy; equal under `kNoCompression`.
- [ ] `uncomp_bytes` is 0 only on a genuine block-read failure.
- [ ] Sequence numbers are strictly increasing and gap-free across the whole
      file when all record types are enabled.
- [ ] Every `G` record's `seq` is less than all of its own block `blk_seq`
      values.
- [ ] A `Get` whose files are all filtered out still emits a `G` record with
      zero probes and a valid `seq`.
- [ ] A trivial move emits exactly one `move` record, not a `delete` plus a
      `create`. Force one with a single-file L1->L2 compaction.
- [ ] Every `file_number` appearing in a `G` or `A` record has a preceding
      `create` or `move` record and no intervening `delete`.
- [ ] `A` records from compaction carry `no_insert=1`; those from user
      iterators carry `no_insert=0`.
- [ ] Two concurrent iterators over the same file produce records with distinct
      `iter_id` values.
- [ ] An iterator reading more than `kIterFlushThreshold` blocks produces
      multiple records sharing one `iter_id`, with `blk_seq` continuous across
      the boundary.
- [ ] An iterator still live across `EndTrace()` does not crash on destruction.
- [ ] `record_iterator_accesses = false` produces zero `A` records.
- [ ] `iterator_caller_mask = ~(1u << kCompaction)` produces no `A` record with
      `caller = 10`.
- [ ] `COERCE_CONTEXT_SWITCH=1 build_tools/rockstest.sh key_lookup_tracer_test
      --gtest_repeat=100` passes.
- [ ] `AUTO_CLEAN=1 ASSERT_STATUS_CHECKED=1 make -j16 check` passes.
- [ ] `AUTO_CLEAN=1 make -j16 check` passes.
- [ ] `make check-sources` passes (no non-ASCII characters).
- [ ] `make format-auto` applied.
- [ ] `db_bench` numbers recorded for all five configurations in section 6.

Note: this toolchain (GCC 16) emits pre-existing `-Wmaybe-uninitialized` false
positives in `db/blob/blob_file_reader.cc` and
`db/compaction/compaction_picker_level.cc` on a clean tree. Build with
`DISABLE_WARNING_AS_ERROR=1`.
