# Key Lookup Tracer

A tracing subsystem for offline LSM block cache analysis and simulation. It
records three things:

- for every point `Get()`, the ordered sequence of SST files searched, what each
  contributed, and the data blocks read from each;
- every data block read by a table iterator, which covers user scans,
  compaction, ingestion, and verification;
- every change to the live file set, including trivial moves.

**Every access is recorded before any cache is consulted, and regardless of
whether it hit.** The trace therefore describes what the read path *needed*,
not what one particular cache happened to be holding. That property is what
makes it usable as simulator input, and it is the reason this exists alongside
`BlockCacheTracer` rather than reusing it: block cache tracing records inside
`MaybeReadBlockAndLoadToCache`, which `RetrieveBlock` only calls when a cache is
configured, so its output is a function of the cache under test.

Part 1 is a usage guide. Part 2 explains what changed in each file, grouped by
feature. Part 3 records what has and has not been verified.

---

## Part 1: Usage guide

### 1.1 Trace file shape

Plain text, one record per line, three record types distinguished by a leading
type field. All three are interleaved in one file and totally ordered by a
trace-wide sequence number.

```
# rocksdb_key_lookup_trace v3 block_id_mode=ordinal size_unit=bytes rocksdb=11.9
F,1,1755212345678000,create,0,1201,5000,4194304,0
G,7,1755212345678901,42,0,1,3,0:1201:0|1:1150:0:4/8/4101/8192:5/9/4101/8192|2:1043:1:12/10/2048/4096
A,11,1755212345679500,0,10,3,2,1043,1,2,0/12/4101/8192:1/13/4101/8192
```

The header names the format version, how block ids are encoded, the unit for
all size fields, and the RocksDB major.minor version. Lines starting with `#`
are metadata; parsers skip them. The only other one is a trailing `# truncated`
marker, described in 1.6.

### 1.2 Record types

#### `G` -- one point lookup

```
G,seq,timestamp_us,lookup_id,cf_id,final_result,num_probes,<probe>|<probe>|...

probe := level:file_number:outcome[:block[:block...]]
block := block_id/seq/read_bytes/uncomp_bytes
```

The example above reads as: lookup 42 on column family 0 searched three files,
in this order -- L0 file 1201 (no data blocks read, so the filter excluded it),
L1 file 1150 (read blocks 4 and 5, key not present), then L2 file 1043 (read
block 12, found the value). The request overall returned a value.

"Block 4" means the 4th data block of that file in offset order, zero-based.

A probe with no blocks has exactly three colon-separated fields and no trailing
colon. When `num_probes` is 0 the probe list is empty and the line ends with a
comma.

#### `F` -- one change to the live file set

```
F,seq,timestamp_us,create|delete|move,cf_id,file_number,num_entries,file_size,level[,to_level]
```

`num_entries` is the key count the file was built with and `file_size` its size
on disk in bytes. They describe the file the record names, so a `move` repeats
the values its `create` carried. `file_size` is always exact. `num_entries` is
0 when RocksDB has not read the file's table properties yet, which in practice
is rare but is not something a parser can assume away; see 1.11.

`to_level` is present only for `move`, where `level` is the source level.

A `move` is a trivial move: the file was relabeled to a different level, not
rewritten, so its blocks stay byte identical. This distinction is not cosmetic.
Recorded as a delete plus a create, a simulator that invalidates on delete would
discard cached blocks that are still perfectly live -- and trivial moves happen
most to exactly the cold lower-level data that has the least chance of being
re-read into cache.

#### `A` -- data blocks read by one table iterator from one file

```
A,seq,timestamp_us,cf_id,caller,iter_id,level,file_number,no_insert,num_blocks,<block>:<block>:...
```

One `BlockBasedTableIterator` serves exactly one SST file, so records are
grouped per file. A scan reading more than 1024 blocks from one file emits
several records sharing an `iter_id`: group by `iter_id` to reassemble the
scan, and use it to tell two concurrent scans over the same file apart.

The block tuple is byte-identical to the one in `G` records, so one parser
handles both.

### 1.3 Field reference

Common to all record types:

| Field | Meaning |
|---|---|
| `seq` | Position in the trace-wide order. Allocated when the operation *starts*, so a record always precedes the blocks it caused |
| `timestamp_us` | Microseconds from the DB's `SystemClock`, taken when the record is emitted |

`G` records:

| Field | Meaning |
|---|---|
| `lookup_id` | Nonzero, unique and increasing within one trace session. Gaps are expected when sampling is on |
| `cf_id` | Column family id |
| `final_result` | How the whole request ended; see 1.4 |
| `num_probes` | Number of probes in the last field |
| probe list | `\|`-separated, empty when `num_probes` is 0 |

`F` records:

| Field | Meaning |
|---|---|
| `cf_id` | Column family id |
| `file_number` | The SST's number, stable across a trivial move |
| `num_entries` | Internal entries the file was built with, tombstones and merge operands included. 0 means unknown; see 1.11 |
| `file_size` | The SST's size on disk in bytes. Always exact |
| `level` | The file's level, or for a `move` the level it came from |
| `to_level` | Present only on a `move` |

`A` records:

| Field | Meaning |
|---|---|
| `cf_id` | **Always 0**, regardless of the real column family; see 1.11 |
| `caller` | `TableReaderCaller` value. 3 is a user iterator, 10 is compaction; see `include/rocksdb/table_reader_caller.h` |
| `iter_id` | Identifies one table iterator |
| `level` | Level of the file **when its table reader was opened**; see the caveat in 1.10 |
| `no_insert` | 1 when `fill_cache` was false, i.e. the access probes the cache but never inserts. Compaction sets this |
| `num_blocks` | Number of `:`-separated blocks in the last field |

Block tuple:

| Field | Meaning |
|---|---|
| `block_id` | Ordinal or raw offset per `block_id_mode`; `18446744073709551615` when unresolvable |
| `seq` | Position of this block access in the trace-wide order |
| `read_bytes` | Bytes read from disk, including the 5-byte block trailer -- i.e. the *compressed* size |
| `uncomp_bytes` | Size of the materialized block, i.e. its footprint in the block cache. 0 only if the block failed to materialize |

> **`read_bytes` and `uncomp_bytes` are different numbers, and that is the
> default case rather than an edge case.** Blocks are stored compressed on disk
> and uncompressed in the block cache, and `compression` defaults to Snappy.
> Use `read_bytes` to model I/O cost and `uncomp_bytes` to model cache
> capacity. A capacity simulation fed compressed sizes will report a working
> set that fits in cache when it does not.

### 1.4 Enumerations

`outcome`, per file:

| Value | Name | Meaning |
|---|---|---|
| 0 | `not_found` | The filter excluded the file, **or** blocks were read and the key was absent |
| 1 | `found_value` | A plain value was found here |
| 2 | `found_tombstone` | A deletion marker was found here |
| 3 | `found_merge_operand` | One or more merge operands were found here |
| 4 | `error` | I/O error, corruption, or unexpected blob index |

`final_result`, per request:

| Value | Name |
|---|---|
| 0 | `not_found` |
| 1 | `found` |
| 2 | `deleted` |
| 3 | `error` |
| 4 | `range_deleted` |

`range_deleted` means the search stopped early because a range tombstone
covered the key. That is a genuinely different reason for "not found" than an
exhausted search, so it is kept distinct.

### 1.5 C++ API

```cpp
#include "rocksdb/db.h"
#include "rocksdb/key_lookup_trace_options.h"

KeyLookupTraceOptions trace_options;
trace_options.record_blocks = true;
trace_options.block_id_mode = KeyLookupBlockIdMode::kOffset;

// Standard .zst output. Measured 3.7-5.4x at level 1 on the traces in Part 3;
// level 1 gives most of the achievable ratio at a fraction of the CPU.
trace_options.compression = kZSTD;
trace_options.compression_level = 1;

// Iterator accesses are the volume driver. Compaction typically outweighs user
// iterators by one to two orders of magnitude.
trace_options.record_iterator_accesses = true;
trace_options.iterator_caller_mask =
    static_cast<uint16_t>(~(1u << TableReaderCaller::kCompaction));

Status s = db->StartKeyLookupTrace(trace_options, "/tmp/key_lookup.trace.zst");
// ... run the workload ...
s = db->EndKeyLookupTrace();
```

`StartKeyLookupTrace()` creates the file, truncating any existing one, and
writes the header. Calling it while a trace is already running returns
`Status::Busy()`. `EndKeyLookupTrace()` flushes and closes; calling it with no
trace running is a no-op returning OK. The tracer's destructor also ends any
running trace, so a trace left open at DB close is still a valid file.

Both are also available on read-only, secondary, and follower DB instances.

### 1.6 Options

| Option | Default | Meaning |
|---|---|---|
| `sampling_frequency` | 1 | Capture one in every N lookups. 0 or 1 captures all. Decided before any capture work, so unsampled requests pay nothing |
| `max_trace_file_size` | 64 GiB | Stop writing past this size |
| `record_blocks` | true | When false, only the file sequence is recorded and all block-capture work is skipped |
| `block_id_mode` | `kOrdinal` | How a data block is identified; see below |
| `compression` | `kNoCompression` | `kNoCompression` or `kZSTD`; anything else is `InvalidArgument` |
| `compression_level` | 1 | zstd level, used only with `kZSTD` |
| `record_iterator_accesses` | true | When false, no `A` records at all. Checked at the collection site, so this is not merely suppressed output |
| `iterator_caller_mask` | `0xFFFF` | Record an iterator access only if bit `(1 << caller)` is set |

**Block id mode.** `kOrdinal` records the block's zero-based index among the
file's data blocks in offset order. It stays meaningful after the SST is
compacted away, which is why it is the default, but it requires building a
per-file map of data block offsets on the first traced access to each file.
`kOffset` records the raw byte offset instead: no setup cost, but mapping an
offset back to a block index later requires the SST to still exist.

> With a partitioned index (`kTwoLevelIndexSearch`), building the ordinal map
> walks the whole index, which reads *every* index partition. That is real I/O
> against the workload being measured. Prefer `kOffset` there. Prefer it also
> for runs with `A` records enabled, since compaction touches every file in the
> DB and would build the map for all of them.

When an ordinal cannot be determined -- the map failed to build, the offset is
not in it, or the read was made with `read_tier == kBlockCacheTier` before the
map existed -- the block id is `UINT64_MAX`. The explicit sentinel is
deliberate: a plausible but wrong ordinal would be worse.

**Truncation.** On reaching `max_trace_file_size` the writer appends a single
`# truncated` line and stops for good, so a reader can tell truncation from a
clean end. With compression on the limit counts *compressed* bytes and is
checked once per 256 KB batch, so the file can overshoot by at most one batch.

**Compression.** `kZSTD` produces a standard `.zst` file that `zstd -d` and
Python's `zstandard` read transparently. Requesting it on a build without zstd
returns `NotSupported` rather than silently writing plain text under a `.zst`
name.

### 1.7 db_bench flags

```bash
./db_bench --benchmarks=readrandom --use_existing_db --num=1000000 \
    --key_lookup_trace_file=/tmp/key_lookup.trace.zst \
    --key_lookup_trace_record_blocks=true \
    --key_lookup_trace_compress=true \
    --key_lookup_trace_iterator_skip_compaction=true
```

| Flag | Default |
|---|---|
| `--key_lookup_trace_file` | `""` (tracing off) |
| `--key_lookup_trace_sampling_frequency` | 1 |
| `--key_lookup_trace_record_blocks` | true |
| `--key_lookup_trace_compress` | false |
| `--key_lookup_trace_compression_level` | 1 |
| `--key_lookup_trace_record_iterator_accesses` | true |
| `--key_lookup_trace_iterator_skip_compaction` | false |

One trace covers the whole `--benchmarks` list: the trace is started before the
first benchmark and ended after the last, so a `fillrandom,readrandom` run
yields a single file containing the load phase and the queries.

### 1.8 C API

```c
rocksdb_key_lookup_trace_options_t* opt =
    rocksdb_key_lookup_trace_options_create();
rocksdb_key_lookup_trace_options_set_record_blocks(opt, 1);
rocksdb_key_lookup_trace_options_set_block_id_mode(opt, 1);  /* kOffset */
rocksdb_key_lookup_trace_options_set_compression(opt, 7);    /* kZSTD */
rocksdb_key_lookup_trace_options_set_compression_level(opt, 1);
rocksdb_key_lookup_trace_options_set_record_iterator_accesses(opt, 1);

char* err = NULL;
rocksdb_start_key_lookup_trace(db, opt, "/tmp/key_lookup.trace.zst", &err);
/* ... workload ... */
rocksdb_end_key_lookup_trace(db, &err);
rocksdb_key_lookup_trace_options_destroy(opt);
```

Every option field has a generated setter and getter. There are no Java
bindings, matching block cache and IO tracing, which have none either.

### 1.9 Parsing a trace

```python
import io, zstandard

def open_trace(path):
    if path.endswith(".zst"):
        return io.TextIOWrapper(zstandard.ZstdDecompressor().stream_reader(
            open(path, "rb")))
    return open(path)

def parse_block(s):
    block_id, seq, read_bytes, uncomp_bytes = (int(x) for x in s.split("/"))
    return block_id, seq, read_bytes, uncomp_bytes

for line in open_trace(path):
    line = line.rstrip("\n")
    if line.startswith("#"):
        continue
    fields = line.split(",")
    if fields[0] == "G":
        _, seq, ts, lookup_id, cf_id, result, num_probes, probe_list = fields
        for probe in (probe_list.split("|") if probe_list else []):
            parts = probe.split(":")
            level, file_number, outcome = (int(p) for p in parts[:3])
            blocks = [parse_block(b) for b in parts[3:]]
    elif fields[0] == "F":
        # fields[9] (to_level) is present only for a move
        (_, seq, ts, op, cf_id, file_number, num_entries, file_size,
         level) = fields[:9]
    elif fields[0] == "A":
        (_, seq, ts, cf_id, caller, iter_id, level, file_number, no_insert,
         num_blocks, block_list) = fields
        blocks = [parse_block(b) for b in block_list.split(":")] \
            if block_list else []
```

Concatenated zstd frames are a standard format, so a compressed trace needs no
special handling beyond the decompressor.

### 1.10 Invariants worth asserting

On a trace that was not truncated:

- Sequence numbers across all records and all block accesses form `1..N` with
  no gaps and no repeats.
- Every `G` record's `seq` is less than the `seq` of every block it lists, so
  sorting by sequence number yields causal order.
- `num_probes` equals the number of `|`-separated probes; `num_blocks` equals
  the number of `:`-separated blocks.
- Levels within one `G` record are non-decreasing -- files are searched in LSM
  order.
- Every `file_number` in a `G` or `A` record has a preceding `create`. It may
  *also* have a preceding `delete` -- see the note in 1.11. Asserting "no
  intervening delete" is wrong and will fire on a correct trace.
- `uncomp_bytes` is 0 only on a genuine block read failure.

### 1.11 Interpreting results: things that will mislead you

**"No blocks read" does not mean "the filter said no."** A row cache hit is
served inside `TableCache::Get` without ever reaching `BlockBasedTable::Get`,
so it also records zero blocks. The `outcome` field is what distinguishes them.

**"Files searched" is not exactly "files whose bloom filter ran."** Three cases
search a file with no filter check: `optimize_filters_for_hits` skipping the
bottommost level, a file with no filter block, and a row cache hit.

**A probe can list several blocks.** The data block loop in
`BlockBasedTable::Get` legitimately runs more than once per file when
collecting merge operands, or when the `first_internal_key` check advances past
a block boundary.

**A memtable-served lookup produces no record at all.** `DBImpl::GetImpl` never
reaches `Version::Get` on a memtable hit. A record with `num_probes == 0` means
something different: `Version::Get` ran but the file picker found no candidate
files, for example because the key falls outside every file's range.

**An `A` record's `level` can be stale.** It comes from
`BlockBasedTable::Rep::level`, the level at *table open* time, which the reader
itself documents as "could potentially change when trivial move is involved" --
and that is exactly what an `F` `move` reports. Treat `F` records as
authoritative for level and the `A` record's level as a hint.

**Compaction probes the block cache even though it never inserts into it.**
`fill_cache = false` gates only the read-and-insert branch in
`MaybeReadBlockAndLoadToCache`; the `GetDataBlockFromCache` lookup above it
still runs, and `ShouldUseDataBlockCacheForIterator` consults neither
`fill_cache` nor `for_compaction`. So a compaction access can *hit*, and on an
LRU cache a hit promotes recency, mutating eviction order for every other
reader. That is what `no_insert` exists to let you model. Whether compaction
should touch the cache at all is a live question -- `use_block_cache_for_lookup`
is not user-exposed, so today it cannot be turned off -- and it cannot be
studied with a trace that omits compaction.

**`num_entries` can be 0, and 0 means "unknown", not "empty".** `file_size` is
known the moment the file is written, but `num_entries` is not: RocksDB fills
`FileMetaData::num_entries` in lazily, from the SST's table properties, in
`Version::PrepareAppend`, and caps that at `kMaxInitCount` = 20 files per
version to bound the I/O (the cap is lifted when `max_open_files == -1`). The
tracer reads whatever is there and never forces the load, so a file the budget
did not reach is reported with `num_entries = 0`. In practice the budget
scans from L0 upward, which is exactly where new files are: on a multi-level
200k-key workload at default `max_open_files`, 0 of 946 `F` records came out
unset. Treat it as reliable but check for 0 rather than dividing by it.

**`num_entries` counts internal entries, not live keys.** Tombstones and merge
operands are included, so it is the count the file was *built* with. Summing it
over the live file set overcounts the DB's logical key count by whatever has
not yet been compacted away.

**Deleting a file does not evict its cached blocks.** RocksDB does not
proactively drop blocks belonging to a deleted SST; they occupy the cache until
LRU pushes them out. `F` `delete` records are what let a simulator model an
invalidating cache manager instead.

**A file can be accessed after its `delete` record.** A `Get` runs against the
`Version` it acquired, and a concurrent compaction can install a new one --
emitting `delete` records -- while that reader is still searching the old one.
The SST is not unlinked until refcounts drop, so the access is real. Measured on
a 2M-lookup run: 35 post-delete accesses over 5 files, all within 53
microseconds of the `delete`, 0.0018% of accesses. A block access can even carry
a *lower* sequence number than the `delete`, because a `G` record's `seq` is
allocated at lookup start and the record is emitted at the end.

> For a simulator this means `F` `delete` is not a barrier. Treating it as
> immediate eviction will produce accesses to blocks the model has already
> discarded. Model it as "stop inserting" plus a grace window, or reconcile
> against the access stream rather than assuming the delete is final.

**A record write that fails is silently dropped.** Every call site ends in
`.PermitUncheckedError()`, so an `Append` failure -- a full disk on a multi-GB
trace, most plausibly -- loses that record with no error surfaced anywhere and
tracing continues. The dense sequence-number check in 1.10 is what detects this
after the fact; there is no signal at the time.

**`A` records always report `cf_id = 0`.** `BlockBasedTableIterator` does not
know its column family, so the field is hardcoded. `G` and `F` records carry the
real id. On a single-column-family DB this is harmless; on a multi-CF DB, do not
group `A` records by `cf_id`.

**A pure-merge result is recorded as `final_result = not_found`.** When a key
resolves entirely from merge operands with no base value, `GetContext`'s state
is still `kMerge` when the record is emitted, and `final_result` has no merge
value. The per-file `found_merge_operand` outcomes still show what happened. If
this matters for your analysis, a sixth `final_result` value is the fix.

### 1.12 What is not traced

- **`MultiGet`.** One of its three code paths (`table_cache_->MultiGetFilter`)
  prunes filter-negative keys before they reach the per-file lookup, and the
  coroutine path completes files out of order. Partial support would produce
  *silently wrong* traces, missing exactly the filter-negative probes, which is
  worse than no support.
- **Memtables**, per above.
- **Index and filter blocks.** Only data blocks are recorded.
- **Non-block-based table formats.** Probes are still recorded; block ids are
  not, because the sink is only consulted in `BlockBasedTable::Get`.
- **Recovery and repair.** `db/repair.cc`, `db/version_util.h`, and
  `tools/ldb_cmd.cc` construct their `VersionSet` with a null tracer, so `F`
  records do not cover those paths. Deliberate -- they are not steady-state
  workload -- but do not read the gap as a bug.

### 1.13 Performance and trace volume

With tracing **off**, the cost is one relaxed atomic load plus a predictable
branch in `Version::Get`, one null check per data block access in
`BlockBasedTable::Get` and in `BlockBasedTableIterator::InitDataBlock`, three
pointers added to `GetContext`, a pointer plus a buffer added to each table
iterator, and two empty `autovector`s on the stack per `Get`.

With tracing **on**, there is one mutex acquisition per *record*: per `Get`, per
file lifecycle event, and per 1024 blocks an iterator reads. Each record is
assembled in a reused buffer and written once. The ordinal map is built at most
once per table per trace session, at 8 bytes per data block -- a 256 MB SST at
4 KB blocks is ~64K blocks, about 512 KB.

With compression on, one thread in every 256 KB of output also compresses that
batch while holding the writer lock, a few hundred microseconds at level 1.
That converts a steady small per-record cost into a periodic large one, which
shows up in tail latency and not in the mean. If the traced DB's own latency
distribution matters to you, leave compression off.

Tracing measurably reduces read throughput. It is a diagnostic tool, not a
production mode.

**Controlling volume.** `iterator_caller_mask` is the effective lever:
compaction typically outweighs user iterators by one to two orders of magnitude
on a write-heavy workload, so masking out `kCompaction` is the main way to cut
trace size. `sampling_frequency` drops whole lookups uniformly, which breaks
the per-block access histories a cache simulator needs -- use it for cheap
workload characterization, not for a simulation run.

---

## Part 2: Changes by file, grouped by feature

### 2.1 Tracer core and record writing

**`trace_replay/key_lookup_tracer.h` / `.cc`** (new). The whole tracer, with no
`db/` or `table/` dependency.

- `KeyLookupOutcome`, `KeyLookupResult`, `KeyLookupFileOp` -- the three
  enumerations.
- `KeyLookupProbe` -- one searched file: level, file number, outcome, and a
  `(first_block_idx, num_blocks)` range into a separate flat block buffer.
- `KeyLookupProbes` -- `autovector<KeyLookupProbe, 8>`, sized so a typical
  lookup allocates nothing.
- `KeyLookupTraceWriter` -- owns the `WritableFile`, formats each record into a
  reused `line_buffer_` with no per-record allocation, and has one method per
  record type plus a shared `AppendBlock()` so the block tuple is emitted
  identically in `G` and `A` lines. `AppendLine()` enforces
  `max_trace_file_size` and appends `# truncated` exactly once.
- `KeyLookupTracer` -- lifecycle modeled on `BlockCacheTracer`: an atomic
  writer pointer for the lock-free `is_tracing_enabled()` check, an
  `InstrumentedMutex` for writes, a re-check of the pointer under the lock, and
  `EndTrace()` from the destructor. `trace_options_` is set before the writer is
  published, so any reader that has observed a non-null writer sees stable
  options.

> **`autovector` is not contiguous.** It spills to a heap `std::vector` past its
> inline capacity, so it has no `data()`. Every buffer is passed by const
> reference and indexed with `operator[]`, never as pointer plus size.

**Block buffers live in their own header.** `KeyLookupBlockAccess` and the two
buffer aliases are in **`trace_replay/key_lookup_block_access.h`** (new) rather
than in the tracer header, so that `table/get_context.h` -- included very
widely -- does not pull in the tracer and its mutex dependency.

### 2.2 Point lookup capture

The tracer pointer stops at `VersionSet`, because `Version::Get` already has
`vset_` in scope. `BlockBasedTable` never holds one.

| File | Change |
|---|---|
| `db/db_impl/db_impl.h` | `KeyLookupTracer key_lookup_tracer_;` next to `block_cache_tracer_` |
| `db/db_impl/db_impl.cc` | Passes `&key_lookup_tracer_` to the `VersionSet` constructor |
| `db/db_impl/db_impl_secondary.cc`, `db/db_impl/db_impl_follower.cc` | Pass `&impl->key_lookup_tracer_` to `ReactiveVersionSet`, so secondary and follower instances trace too |
| `db/version_set.h` / `.cc` | `KeyLookupTracer* const` constructor parameter and member on both `VersionSet` and `ReactiveVersionSet` |
| `db/key_lookup_trace_scope.h` | New; see below |
| `db/version_set_sync_and_async.h` | The hook in `Version::Get` |

**`db/key_lookup_trace_scope.h`** exists because
`db/version_set_sync_and_async.h` is included **twice in the same translation
unit**, once per coroutine variant, and so must not define any type or
non-inline function. Everything that needs to know about `GetContext` lives
here:

- `ClassifyProbeOutcome(status, state, operands_before, operands_after)` maps
  one file's result to a `KeyLookupOutcome`.

  > **The operand-count delta is load-bearing.** `GetContext::State()` is
  > *cumulative* across files: once it reaches `kMerge` it stays `kMerge`
  > whether or not the current file contributed anything. Comparing states alone
  > would mark every file after the first merge operand as a hit.

- `KeyLookupTraceScope` is an RAII emitter that writes one record in its
  destructor, covering all 10 `CO_RETURN` exit points of `Version::Get` without
  a single hand-placed emit call. `ComputeFinalResult()` checks, in order:
  non-OK non-NotFound status, `kFound`, `kDeleted`, then
  `*max_covering_tombstone_seq > 0` for `kRangeDeleted`, else `kNotFound`. A
  null tracer makes the destructor a no-op.

In `Version::Get` itself there are three insertions:

1. Before the `FilePicker`: the two per-request buffers, the tracer lookup, the
   sampling decision, the sequence number, the block sink hookup, and the scope.

   > **Declaration order is a correctness requirement.** `blocks` and `probes`
   > are declared *before* `klt_scope`. Destructors run in reverse declaration
   > order, so the scope is destroyed first and the buffers it reads are still
   > alive. Reversing this is a use-after-free that would not reliably crash in
   > tests. There is a comment in the code saying so.

2. Immediately before `table_cache_->Get`: `blocks_before` and
   `operands_before`, both guarded by an `UNLIKELY` check so tracing-off costs
   stay at one branch.
3. Immediately after it and **before** the `if (!status->ok())` early return,
   or error probes would be lost: the `probes.push_back(...)`.

No probe is recorded for the `*max_covering_tombstone_seq > 0` break, because
that check fires before the file is searched.

**Twenty-five further `VersionSet` construction sites** across eleven files
needed the new argument: `db/repair.cc` (1), `db/version_util.h` (1),
`db/version_set.cc` (1, the offline `DumpManifest` helper), `tools/ldb_cmd.cc`
(3), `tools/ldb_cmd_test.cc` (1), `db/memtable_list_test.cc` (2),
`db/version_set_test.cc` (11, including one `ReactiveVersionSet`),
`db/wal_manager_test.cc` (1), `db/db_wal_test.cc` (1), `db/flush_job_test.cc`
(1), `db/compaction/compaction_job_test.cc` (2).

> `grep "VersionSet("` does **not** find them all -- stack constructions read
> `VersionSet versions(...)`, which lacks that substring. The six sites in
> `tools/ldb_cmd.cc`, `tools/ldb_cmd_test.cc`, and `db/memtable_list_test.cc`
> are also invisible to `make static_lib`, which does not compile those files.

### 2.3 Data block capture on the `Get` path

**`table/get_context.h`** carries `block_sink_` (a `KeyLookupBlockAccesses*`,
null when tracing is off or the lookup was not sampled), `block_id_mode_`, and
`key_lookup_tracer_`, set together by `SetBlockSink()`.

A raw pointer rather than an embedded container, because `GetContext` is
constructed per key in `MultiGet` batches (`autovector<GetContext, 16>`) and
embedding the block buffer would add roughly 1 KB of stack per 16-key batch
that `MultiGet` will never use. A setter rather than constructor parameters,
because `GetContext`'s telescoping positional constructors have many call sites
and only one of them traces.

The tracer pointer rides along because block accesses need sequence numbers.
This is the one place `BlockBasedTable::Get` sees the tracer, still without
`BlockBasedTable` holding one.

**`table/block_based/block_based_table_reader_sync_and_async.h`** pushes an
entry inside the index loop of `BlockBasedTable::Get`, placed after the
`first_internal_key` early-`break` so a block that is skipped rather than read
is not recorded, and before `NewDataBlockIterator`. A null sink check is the
entire integration.

### 2.4 Block identification

RocksDB has no block ordinal anywhere: blocks are identified by
`BlockHandle{offset, size}` throughout, and `BlockIter` tracks only `current_`
(a byte offset within the block buffer) and `restart_index_`, neither of which
is an entry index. Since `iiter->Seek(key)` jumps to an arbitrary position, you
cannot count your way there either. So `kOrdinal` needs a per-file sorted list
of data block offsets.

**`table/block_based/block_based_table_reader.h`** -- `Rep` gains
`data_block_offsets_mutex`, `data_block_offsets`, and `offset_map_state`
(`kUnbuilt` / `kReady` / `kFailed`). `BlockIdForTrace()` is public, because
table iterators resolve their own block ids.

**`table/block_based/block_based_table_reader.cc`** --
`EnsureDataBlockOffsetsBuilt()` walks the index with `fill_cache = false` so
the walk does not pollute the block cache, and `TableReaderCaller::kUncategorized`
so it does not inject fake `kUserGet` records into a concurrent block cache
trace. It builds **outside** the lock, because the walk performs I/O and
holding the mutex across it would stall every concurrent reader of that table,
then installs under the lock only if the state is still `kUnbuilt`. Index order
equals offset order for data blocks, so the result is already sorted; that is
asserted in debug builds rather than re-sorted.

`offset_map_state` is a `std::atomic` written with release and read with
acquire, and `data_block_offsets` is immutable once `kReady`, so the
steady-state path is one acquire load plus a binary search with **no** mutex
acquisition.

`BlockIdForTrace()` returns the offset unchanged in `kOffset` mode, and in
`kOrdinal` mode ensures the map exists and does a `std::lower_bound`. It returns
the `UINT64_MAX` sentinel rather than building the map when
`read_options.read_tier == kBlockCacheTier`: building it walks the index, which
would violate the caller's no-I/O contract for the sake of a trace.

### 2.5 Block sizes

`read_bytes` is `handle.size() + BlockBasedTable::kBlockTrailerSize` and is
available at the hook. `uncomp_bytes` is not: the uncompressed size does not
exist until the block materializes.

**`table/block_based/block.h`** -- `BlockIter` gains `block_size_` with a setter
and getter. The size is not derivable from `restarts_` and `num_restarts_`,
because a hash index suffix and a footer may have been stripped before those
were computed.

**`table/block_based/block_based_table_reader_sync_and_async.h`** -- both
`NewDataBlockIterator` overloads call `iter->SetBlockSize()` once the block
materializes. That is one place serving both the `Get` and the iterator path.
The `Get` hook records `read_bytes` before the call and reads `uncomp_bytes`
back off the iterator after it.

> Keeping the *recording decision* before the call is what preserves cache
> independence. The size is merely filled in afterwards, and the block
> materializes on both a cache hit and a miss, so nothing about whether an
> access is recorded depends on cache state.

### 2.6 Trace-wide sequence numbering

**`trace_replay/key_lookup_tracer.h`** -- a `seq_counter_` atomic and
`NextSeq()`, reset by `StartTrace()`.

A record's sequence number is allocated at operation *start*, so it precedes
the sequence numbers of the blocks that operation reads, and so an operation
that reads no blocks at all -- a `Get` whose candidate files were all filtered
out -- still occupies a position in the trace-wide order. Filter effectiveness
is exactly the kind of thing this trace should be able to measure.

> **Timestamps cannot substitute.** A `G` record's timestamp is taken in
> `~KeyLookupTraceScope`, i.e. *after* its blocks were read. Under concurrency
> an `A` record emitted during that lookup carries an earlier timestamp than
> the lookup that logically precedes it, so replaying by timestamp silently
> reorders the access stream.

The shared atomic is affordable here because every traced access already pays
for string formatting and a mutex-serialized `Append`; a relaxed `fetch_add` is
noise next to a mutex acquisition, and when tracing is off the counter is never
touched.

### 2.7 File lifecycle capture

**`db/version_set.h` / `.cc`** -- `RecordFileLifecycleForTrace()`, called from
`ProcessManifestWrites` inside the `if (s.ok())` install block, after the
manifest write succeeded and the new versions are installed, so an edit that
failed never reaches the trace. This is the single funnel for every change to
the live file set: flush, compaction, trivial move, and ingestion all pass
through it.

Trivial move detection: a file number present in both `GetDeletedFiles()` and
`GetNewFiles()` of one `VersionEdit` was relabeled, not rewritten. The
implementation builds a map of new file numbers to their metadata, emits `move`
for the intersection while erasing it from the map, `delete` for the rest of
the deleted set, and `create` for whatever remains in the map.

Per-file stats: both come from `FileMetaData`, but not from the copy the
`VersionEdit` carries. That copy is taken when the file is written, and at that
moment `num_entries` is still 0 -- RocksDB fills it in afterwards, from the
file's table properties, in `Version::PrepareAppend`. So `create` and `move`
read the *live* metadata out of the version that was just installed, via
`VersionStorageInfo::GetFileMetaDataByNumber()`, and fall back to the edit's
copy when the file is not there. Reading the edit's copy directly is the
obvious implementation and it reports `num_entries = 0` on every create.

`GetDeletedFiles()` holds only `(level, file_number)`, so a deleted file's
metadata has to come out of the version it is being dropped *from* -- which is
why `CollectDeletedFileStatsForTrace()` runs before the `AppendVersion` loop
while `RecordFileLifecycleForTrace()` runs after it. A file that lookup misses,
which means it was created and deleted inside the same batch, is still
recorded, with both stats 0; the delete matters to a simulator whether or not
the size is known.

Nothing here forces a table-properties load, so the feature adds no I/O: it
reports what `PrepareAppend` had already read, and 0 when it had not.

This is the cheapest feature here -- a handful of records per compaction, on a
cold path -- and per byte the most useful, because it is the only way to see
that a deleted file's blocks are still sitting in the cache.

### 2.8 Iterator access capture

**Plumbing.** `ColumnFamilySet` holds the tracer, set by `VersionSet`
immediately after construction and before any column family exists, so it needs
no synchronization. `ColumnFamilyData` hands it to its `TableCache`;
`TableCache` puts it on `TableReaderOptions`; `BlockBasedTable::Open` stores it
on `Rep` along with the file number, which `Rep` did not previously keep
because `base_cache_key` folds it together with the DB session id.

| File | Change |
|---|---|
| `db/column_family.h` / `.cc` | `key_lookup_tracer_` on `ColumnFamilySet` with a setter and getter; `ColumnFamilyData` forwards it to its `TableCache` |
| `db/table_cache.h` / `.cc` | `SetKeyLookupTracer()` and the member; passed into `TableReaderOptions` |
| `table/table_builder.h` | `key_lookup_tracer` on `TableReaderOptions`, set after construction like `blob_source` so the many positional callers are unaffected |
| `table/block_based/block_based_table_factory.cc` | Forwards it to `BlockBasedTable::Open` |
| `table/block_based/block_based_table_reader.h` / `.cc` | `Open` parameter; `Rep::key_lookup_tracer` and `Rep::file_number` |
| `table/block_based/block_based_table_iterator.h` / `.cc` | The buffer, the hooks, and the flush |

> `BlockCacheTraceRecord::sst_fd_number` was not usable as a model for the file
> number: it is declared and serialized but never populated anywhere in
> `table/`, so it is always 0.

**The iterator.** One `BlockBasedTableIterator` serves one SST file, so the
iterator is the grouping unit and owns the buffer. `TraceBlockAccess()` records
the access, `FinishTracedBlock()` fills in the materialized size and flushes at
`kIterFlushThreshold` (1024) blocks, and `FlushTracedBlocks()` emits a record.
The destructor flushes whatever remains.

The caller identity needed no plumbing at all: the constructor already takes a
`TableReaderCaller` and stores it as `lookup_context_.caller`.

Hooks sit in `InitDataBlock` -- all three paths: multi-scan, prefetched
handles, and regular -- and in `AsyncInitDataBlock`. The async path records
*after* the call rather than before, because a first pass returning `TryAgain`
has read nothing and the second pass records that access instead; recording
before would consume a sequence number that then had to be discarded, leaving a
gap in the trace-wide order.

> The hooks are deliberately **not** unified into `NewDataBlockIterator`, even
> though it is the common funnel for both `Get` and iterator paths. It does not
> know which iterator called it, and per-file grouping needs iterator identity.

**Trace sessions.** A table iterator can outlive the trace session it was
created in, and `StartTrace()` resets both the sequence and iterator id
counters. A stale iterator flushing into a *later* session would inject records
with a colliding `iter_id` and block sequence numbers from outside that trace's
range, silently breaking the dense-ordering invariant a simulator depends on.
`KeyLookupTracer::session_id()` increments on every `StartTrace()`; the iterator
captures it at construction and discards its buffer when it no longer matches.

### 2.9 Trace compression

**`trace_replay/key_lookup_tracer.h` / `.cc`** -- `KeyLookupTraceWriter` holds a
`staging_` buffer, a `StreamingCompress`, an output buffer, and
`FlushStaging()` / `CloseFile()`. `NewWritableFile()` takes the whole options
struct, creates the compressor, and rejects a compression type other than
`kNoCompression` or `kZSTD` with `InvalidArgument`, or an unavailable zstd with
`NotSupported`.

> **The load-bearing detail: `ZSTDStreamingCompress::Compress` calls
> `ZSTD_compressStream2` with `ZSTD_e_end`, so every call terminates a frame.**
> Records are therefore batched into `kCompressBatchSize` (256 KB) and
> compressed one batch at a time. Compressing per record would put a 9-13 byte
> frame header on a ~100 byte record and restart the window each time -- which
> is why WAL compression, which needs every record independently decodable and
> so resets per record, has mediocre ratios. Concatenated frames are a standard
> format, so the output is a real `.zst`.

The output buffer is sized `kCompressBatchSize + kCompressBatchSize / 8 + 1024`,
safely above `ZSTD_compressBound` for that input, so one `Compress()` call
consumes a whole batch. The loop still calls repeatedly with the same input
pointer until nothing remains.

> **`FlushStaging()` must call `compress_->Reset()` after every completed
> frame, and this is a correctness requirement, not a tuning knob.**
> `ZSTDStreamingCompress::Compress` decides whether it has been handed new
> input by comparing the input *pointer*, not the contents. `staging_.clear()`
> keeps the allocation, so the next batch arrives with an identical `data()`,
> is taken for the batch just consumed, and yields only an 8-byte frame
> epilogue. Every batch after the first is then discarded and the trace stops
> after 256 KB, with no error returned and a perfectly valid `.zst` on disk.
> `db/log_writer.cc` calls `Reset()` for the same reason; per *record* there
> rather than per batch only because of the granularity it needs.

The header line goes through the same path as every other record, so a
compressed trace is a pure `.zst` rather than plaintext followed by compressed
bytes.

**`util/compression.h`** -- `ZSTDStreamingCompress` accepted a
`CompressionOptions` but never applied `opts.level`; it set only
`ZSTD_c_checksumFlag`. It now applies the level when one is explicitly
configured, guarded on `kDefaultCompressionLevel` so the existing WAL caller is
unaffected. This is the only change here outside the tracer's own footprint.

### 2.10 Sampling and volume control

`NextLookupId()` applies `sampling_frequency` *before* any capture work, taking
a `fetch_add`, skipping the reserved 0, and returning 0 when the lookup is not
sampled -- so unsampled requests pay nothing beyond the counter.

`ShouldTraceIterator(caller)` combines `record_iterator_accesses` with
`iterator_caller_mask` and is checked when the iterator is *constructed*, so a
masked-out caller never allocates an id and never buffers anything.

> Temporal sampling is correct for `G` records because the unit of analysis is
> the request. It is *not* correct for cache simulation, which needs complete
> per-block histories -- which is why `iterator_caller_mask`, not
> `sampling_frequency`, is the volume lever for a simulation run. Block cache
> tracing samples *spatially* by block key for this reason.

### 2.11 Public API and language bindings

| File | Change |
|---|---|
| `include/rocksdb/key_lookup_trace_options.h` | New public header: `KeyLookupBlockIdMode` and `KeyLookupTraceOptions` |
| `include/rocksdb/db.h` | `StartKeyLookupTrace()` / `EndKeyLookupTrace()` virtuals defaulting to `NotSupported` |
| `include/rocksdb/utilities/stackable_db.h` | Forwards both to `db_` |
| `db/db_impl/db_impl.h` / `.cc` | Overrides next to the block cache trace ones |
| `tools/c_api_gen/c_base.h` / `.cc` | The `rocksdb_key_lookup_trace_options_t` wrapper, create/destroy, and the two trace functions |
| `tools/c_api_gen/auto_simple_bindings.py` | A `FamilyConfig` for `KeyLookupTraceOptions`, so every field's setter and getter is generated. Also adds `CompressionType` to the family's `enum_types` and teaches `scalar_return_type()` about `uint16_t`, which the generator did not previously support |
| `include/rocksdb/c.h`, `db/c.cc`, `c_api_gen/*.inc` | **Generated.** Do not hand-edit; run `python3 tools/c_api_gen/regen_all.py` and verify with `tools/c_api_gen/verify_generated_up_to_date.py` |

### 2.12 Build wiring and tooling

| File | Change |
|---|---|
| `src.mk` | `trace_replay/key_lookup_tracer.cc` in the source list; `key_lookup_tracer_test.cc` in the test list |
| `CMakeLists.txt` | The same two entries |
| `Makefile` | `key_lookup_tracer_test` link rule modeled on `block_cache_tracer_test` |
| `BUCK` | **Generated.** Do not hand-edit; run `python3 buckifier/buckify_rocksdb.py` after updating `src.mk` |
| `tools/db_bench_tool.cc` | Seven flags, the start block next to the block cache trace start, and the matching end block. The start is guarded by a `key_lookup_trace_started` flag declared outside the per-benchmark loop: `EndKeyLookupTrace()` runs after that loop, so starting again for the second benchmark in a list would return `Busy` and `ErrorExit()` the run |
| `unreleased_history/new_features/key_lookup_trace.md` | One-line release note. Not `HISTORY.md`, which says entries for the next release go here instead |

The three headers added since -- `key_lookup_block_access.h`,
`key_lookup_trace_scope.h`, and the public options header -- need no build
entries; only `.cc` files are listed.

### 2.13 Tests

**`trace_replay/key_lookup_tracer_test.cc`** (new) -- 43 tests in three
fixtures.

Shared helpers keep the cases from duplicating setup: `SplitOn`,
`ReadTraceLines`, `ParseBlock`, the three per-type record parsers behind
`ParseTrace` / `ParseRecords`, `DecompressTraceFile`, `SplitTraceText`, and on
the fixtures `FlushToLevel`, `FlushSpanningFile` (writes one file spanning
"a".."z" with a caller-supplied middle write), `TraceOneGet`,
`EndTraceAndParse`, `EndTraceAndParseAll`, `BlockIdFor`, `BlockAccessFor`, and
`FileNumbersAtLevel`.

**`KeyLookupTracerTest`** -- format level, no DB. Exact line assertions for all
three record types, including that a zero-probe `G` line ends with a comma,
that a zero-block probe has exactly three colon-separated fields with no
trailing colon, that `to_level` appears only on a `move`, and that
`num_entries` and `file_size` sit between the file number and the level. All
five `outcome` and all five `final_result` values. Truncation appending `#
truncated` exactly once. Sampling at frequency 4 yielding exactly one in four.
The caller mask, and that the master switch overrides it. The not-tracing and
double-start paths.

`ZstdCompressionRoundTrips` traces the same workload plain and compressed, then
asserts the zstd magic number, that decompressing reproduces the plain trace
byte for byte including the header, and that the ratio exceeds 3x.

> **That ratio assertion is a tripwire, not a performance check.** A ratio near
> 1 on this payload is the signature of compressing per record rather than per
> batch, which is the mistake that makes the whole feature pointless.

`ZstdCompressionSpansMultipleBatches` writes 60000 records, several times the
batch size, and asserts the decompressed trace equals the plain one line for
line.

> **This case exists because `ZstdCompressionRoundTrips` structurally cannot
> catch a dropped batch.** Its 5000 records come to roughly 200 KB against a
> 256 KB batch, so the entire trace fits in one staging buffer and the only
> flush is the one in the destructor -- the second-batch path never executes.
> A missing `compress_->Reset()` that silently discarded every batch after the
> first passed that test. The multi-batch case asserts
> `plain_size > 4 * 256 * 1024` before comparing, so if the payload or the
> batch size ever changes it fails on the premise rather than quietly
> degenerating back into the single-batch case.

**`KeyLookupTraceDBTest`** -- integration on `DBTestBase`, building a
deterministic LSM with `disable_auto_compactions`,
`level_compaction_dynamic_level_bytes = false`, and explicit `Put` + `Flush` +
`MoveFilesToLevel`.

Point lookups: `FoundInBottomLevel`, `TombstoneStopsTheSearch`,
`FilterNegativeProbesReadNoBlocks`, `MemtableHitIsNotTraced`,
`NoCandidateFilesGivesZeroProbes` (which needs more than one non-empty level,
because `FilePicker` skips its key-range check when there are few files),
`RangeTombstoneStopsTheSearch`, `SamplingSkipsRequests`,
`EndTraceWithoutStartIsOk`.

`MergeOperandsAcrossFiles` is **the regression test for the cumulative-state
bug**: four files -- L0 merge operand, L1 spanning "k" but holding no entry for
it, L2 merge operand, L3 base value. The L1 file must report `not_found`; code
that compares `GetContext` states alone would report `found_merge_operand`.

Block sizes: `BlockSizesRecordedWithoutCompression` asserts
`read_bytes == uncomp_bytes + kBlockTrailerSize` with compression off, and
`CompressedBlockReportsBothSizes` asserts `uncomp_bytes > read_bytes` under
Snappy -- the case a capacity simulation depends on.

Sequence numbers: `SequenceNumbersAreDenseAndOrdered` collects every sequence
number the trace hands out, records and blocks alike, and checks they form
`1..N` with no gaps or repeats. `RecordSeqPrecedesItsBlockSeqs` checks causal
order survives a sort. `LookupReadingNoBlocksStillGetsSeq` covers the
filtered-out lookup.

File lifecycle: `FlushEmitsCreateRecord`,
`RewritingCompactionEmitsDeleteAndCreate`, and
`TrivialMoveEmitsMoveNotDeleteAndCreate` -- which asserts the whole chain,
since `MoveFilesToLevel(2)` walks the file down one level at a time and so
reaching L2 is two trivial moves, with the file keeping its number throughout.
All three also check the per-file stats: the entry count matches the number of
keys flushed and the size is nonzero, on deletes as well as creates, and a
trivially moved file reports the same size before and after the move.

Iterator accesses: `IteratorEmitsGroupedAccesses`,
`LongScanEmitsContinuationRecords` (a scan crossing `kIterFlushThreshold` emits
several records sharing an `iter_id`, each full one at exactly the threshold,
with block sequence numbers ordered across the boundary),
`ConcurrentIteratorsOverOneFileGetDistinctIds`,
`CompactionAccessesAreMarkedNoInsert`, `IteratorAccessesCanBeDisabled`,
`IteratorCallerMaskExcludesCompaction` (and that masking compaction does not
suppress user iterators), `IteratorLiveAcrossEndTraceDoesNotCrash`, and
`StaleIteratorDoesNotWriteIntoNextSession`.

**`KeyLookupTraceBlockIdTest`** -- builds one L0 file with 128-byte blocks and
200 keys, then checks that the first key maps to ordinal 0, that ordinals are
ascending and dense over the key range, that `kOffset` yields strictly
ascending distinct offsets, and that `record_blocks = false` still records the
file sequence but no blocks.

---

## Part 3: Verification

```bash
make format-auto          # needs clang-format-diff.py in the repo root
make check-sources
AUTO_CLEAN=1 make -j$(nproc) check
AUTO_CLEAN=1 ASSERT_STATUS_CHECKED=1 make -j$(nproc) check
COERCE_CONTEXT_SWITCH=1 DISABLE_WARNING_AS_ERROR=1 AUTO_CLEAN=1 \
    make -j$(nproc) key_lookup_tracer_test
./key_lookup_tracer_test --gtest_repeat=100
```

### What has been run

| Check | Result |
|---|---|
| `key_lookup_tracer_test` | 43 tests, all passing |
| Same, 100x under `COERCE_CONTEXT_SWITCH=1` | 4200 runs, 0 failures (42-test suite) |
| `db_bench --benchmarks=fillrandom,readrandom,seekrandom` with compression | 49947 `G`, 104604 `A`, 9427 `F` over 48.5 compressed batches; decodes to a complete trace |
| Compressed vs plain trace of one workload | identical on every workload-determined field across all lookups |
| 2,000,000-read traced run, all invariants checked | 1998923 `G` (1077 memtable hits), 7455 `A`, 15067 `F`; 5,789,503 sequence numbers dense with no gaps or repeats |
| 300,000-seek traced run, all invariants checked | 749249 `A`, 299649 `G`, 5110 `F`; 3,093,877 sequence numbers dense; `no_insert` matches caller on every record; 0 failures |
| `version_set_test` | 212 passing |
| `table_test` | 6952 passing |
| `db_basic_test` | 285 passing (normal build) |
| `c_test` | passing, after `regen_all.py` |
| `verify_generated_up_to_date.py` | generated C API is current |
| `make check-sources` | clean |
| `make format-auto` | clean as of the pre-compression-fix tree; clang-format is not installed on the current machine, so later edits are hand-formatted |

### What has not been run

- A full `make check`.
- The `COERCE_CONTEXT_SWITCH=1` flakiness sweep **since
  `ZstdCompressionSpansMultipleBatches` was added**. The 4200-run figure above
  predates it, and that test is the slowest in the suite.
- Any `db_bench` *performance* measurement. The db_bench row above is a
  functional check of trace completeness, not a timing run. Section 1.13
  describes the expected costs; **none of it is measured.** The configurations
  worth comparing are baseline, tracing off, tracing on, tracing on with `A`
  records off, tracing on with compaction masked out, and tracing on with
  compression -- reporting p99 and p99.9
  alongside the mean for the compressed case, since the periodic batch
  compression lands in the tail and not in the mean.

### Environment notes

- **This toolchain (GCC 16) fails the build on pre-existing
  `-Wmaybe-uninitialized` false positives** in `db/blob/blob_file_reader.cc`
  and `db/compaction/compaction_picker_level.cc`. Verified pre-existing by
  stashing all changes and rebuilding. Use `DISABLE_WARNING_AS_ERROR=1`.
- **Three `db_basic_test` cases fail under `ASSERT_STATUS_CHECKED=1`** on this
  tree: `ReuseManifestOnOpenDisabledByBestEffortsRecovery`,
  `OptimizeManifestForRecoveryDisabledByBestEffortsRecovery`, and
  `IncrementalRecoveryNoCorrupt`. Also verified pre-existing by stashing. They
  are unrelated to this work, but an ASC sweep will surface them.
- `build_tools/rockstest.sh <bin> -r100` does **not** repeat the test.
  `rockstest.sh` runs the binary directly and gtest ignores `-r100`. Use
  `--gtest_repeat=100`.
- Building only `static_lib` does not compile `tools/ldb_cmd.cc` or several
  test files. A change to a widely constructed type like `VersionSet` needs a
  full `make check` build to surface every call site.
- `make format-auto` needs `clang-format-diff.py` in the repo root. On this
  system it lives at `/usr/share/clang/clang-format-diff.py`; symlink it, run
  the target, then remove the symlink so it does not linger as an untracked
  file.
