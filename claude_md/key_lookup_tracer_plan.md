# Key Lookup Tracer -- Implementation Plan

A self-contained implementation spec for a new RocksDB tracing subsystem that
records, for every point `Get()`, the ordered sequence of SST files searched and
the data blocks read from each.

Written for a Claude Code session with no prior context on this feature. All
line numbers are as of commit `323d915db`; they will shift as you edit, so each
reference also gives anchor text to search for.

---

## 0. What you are building

A new `KeyLookupTracer`, started and stopped through a public `DB` API, that
appends one plain-text line per `Get()` request to a trace file:

```
timestamp_us,lookup_id,cf_id,final_result,num_probes,<probe>|<probe>|...
```

where each `<probe>` is `level:file_number:outcome[:block[:block...]]`.

A concrete line:

```
1755212345678901,42,0,1,3,0:1201:0|1:1150:0:4:5|2:1043:1:12
```

Read as: lookup 42 on column family 0 searched three files in this order --
L0 file 1201 (no data blocks read, so the bloom filter excluded it), L1 file
1150 (read data blocks 4 and 5, key not present), then L2 file 1043 (read data
block 12, found the value). The request overall returned a value.

"Block 4" means the 4th data block of that file in offset order, zero-based.

---

## 1. Scope

These decisions are settled. Implement exactly this scope; do not expand it.

### In scope

- Point `Get()` only, including read-only and secondary DB instances.
- SST files only.
- Four distinct per-file outcomes: not found, value, tombstone, merge operand.
- One overall per-request result.
- Data blocks read per file, reported as zero-based ordinals.
- Plain-text output. No binary format, no reader class, no analyzer tool.

### Out of scope

- **`MultiGet`.** Deliberately excluded. It has three separate code paths, one
  of which (`table_cache_->MultiGetFilter`) prunes filter-negative keys before
  they ever reach the per-file lookup, and the coroutine path completes files
  out of order. Supporting it correctly roughly doubles the hook surface. Do not
  add partial `MultiGet` support -- partial support here produces *silently
  wrong* traces (missing exactly the bloom-filter-negative probes), which is
  worse than no support.
- **Memtables.** A request served from a memtable logs zero probes. That is
  correct and intended.
- **Iterators, compaction, and other `TableReaderCaller` values.**
- **Distinguishing "bloom filter said no" from "read data blocks, key absent".**
  Both are `not_found`. This is a deliberate simplification; see section 6.
- **Index and filter blocks.** Optional extension, section 8. Do not build it
  unless explicitly asked.

---

## 2. Background: the `Get()` read path

You need this to place the hooks correctly.

### 2.1 Where files are chosen

`Version::Get` is defined in `db/version_set_sync_and_async.h:14`
(`DEFINE_SYNC_AND_ASYNC(void, Version::Get)`). Its core is a single loop over
files, at line 65 (`while (f != nullptr) {`):

```cpp
FdWithKeyRange* f = fp.GetNextFile();
while (f != nullptr) {
  if (*max_covering_tombstone_seq > 0) { break; }
  ...
  *status = CO_AWAIT(table_cache_->Get, read_options, ..., &get_context, ...);
  ...
  switch (get_context.State()) { ... }
  f = fp.GetNextFile();
}
```

Every iteration is exactly one file searched, already in chronological order.
`FilePicker::GetNextFile()` (`db/version_set.cc:348`) has already skipped files
whose key range excludes the key, so what reaches this loop is precisely the set
of files whose filter/index gets consulted.

Per iteration you have:

| Datum | Expression |
|---|---|
| File number | `f->fd.GetNumber()` (`db/version_edit.h:219`) |
| Level | `fp.GetHitFileLevel()` |
| Cumulative lookup state | `get_context.State()` |
| Merge operand count | `merge_context->GetNumOperands()` (`db/merge_context.h:67`) |

### 2.2 Why this one hook covers everything in scope

Read-only and secondary DBs both route through the same function
(`db/db_impl/db_impl_readonly_sync_and_async.h:125` and
`db/db_impl/db_impl_secondary_sync_and_async.h:182`, both
`CO_AWAIT(super_version->current->Get, ...)`). No extra hooks needed.

### 2.3 Where blocks are read

Two layers below, in `BlockBasedTable::Get`, at
`table/block_based/block_based_table_reader_sync_and_async.h:571`:

```cpp
for (iiter->Seek(key); iiter->Valid() && !done; iiter->Next()) {
  IndexValue v = iiter->value();
  ...
  CO_AWAIT(NewDataBlockIterator, read_options, v.handle, &biter, ...);
```

`v.handle.offset()` is the block's byte offset in the file, available for free
at the exact moment the block is accessed. There is no return channel from here
back up to `Version::Get`; section 4 solves that.

This loop can legitimately run more than once per file -- when collecting merge
operands, or when the `first_internal_key` check advances past a block boundary
-- so a single probe can list several blocks.

### 2.4 Critical constraint: double inclusion

`db/version_set_sync_and_async.h` is included **twice in the same translation
unit** (`db/version_set.cc:8272` and `:8275`), once per coroutine variant.

**Therefore: do not define any class, struct, enum, or non-inline free function
in that file.** It would be a redefinition error. Only the function body itself
belongs there. All helper types go in separate include-guarded headers.

---

## 3. Architecture

```
DBImpl                     owns KeyLookupTracer, exposes Start/EndKeyLookupTrace
  |
  +-- VersionSet           holds KeyLookupTracer* const (raw, non-owning)
        |
        +-- Version::Get   reads vset_->key_lookup_tracer_
              |            owns the per-request probe + block buffers
              |            RAII scope emits one record on every exit path
              |
              +-- GetContext      carries a raw pointer to the block sink
                    |
                    +-- BlockBasedTable::Get   appends block ids to the sink
```

Three deliberate choices, each with a reason you should not undo:

**The tracer pointer stops at `VersionSet`.** `Version::Get` already has `vset_`
in scope. `BlockBasedTable` never learns about this tracer -- it only sees a
sink pointer on `GetContext`, so a null check is the entire integration.

**The block sink is a raw pointer on `GetContext`, not an embedded container.**
`GetContext` is constructed per key in `MultiGet` batches
(`autovector<GetContext, 16> get_ctx` at `db/version_set_sync_and_async.h:406`).
Embedding an `autovector<uint64_t, 16>` there would add roughly 1 KB of stack
per 16-key batch that `MultiGet` will never use. A raw pointer costs 8 bytes and
is null when tracing is off.

**Block ids live in one flat buffer for the whole request.** Each probe stores
`(first_block_idx, num_blocks)` into it. `GetContext` persists across all files
in the loop, so a "drain and clear after each file" scheme would silently
attribute the previous file's blocks to the current one if any code path misses
the clear. A flat buffer with index ranges makes that class of bug
unrepresentable.

---

## 4. Output format

### 4.1 Header line

The first line of every trace file:

```
# rocksdb_key_lookup_trace v1 block_id_mode=ordinal rocksdb=10.7
```

Starts with `#` so parsers skip it. `block_id_mode` is `ordinal` or `offset`
(see section 5.5). Record the RocksDB major.minor version.

### 4.2 Record lines

```
timestamp_us,lookup_id,cf_id,final_result,num_probes,<probe>|<probe>|...
```

Six comma-separated fields. The sixth is the probe list, `|`-separated;
within a probe, fields are `:`-separated:

```
level:file_number:outcome[:block_id[:block_id...]]
```

A probe with no blocks read has exactly three fields. Emit no trailing `:`.

If `num_probes` is 0 (request served entirely from memtables, or the file picker
found no candidate files), the sixth field is empty and the line ends with a
comma.

### 4.3 Enumerations

`outcome` (per file):

| Value | Name | Meaning |
|---|---|---|
| 0 | `not_found` | Filter excluded it, or blocks read and key absent |
| 1 | `found_value` | A plain value was found here |
| 2 | `found_tombstone` | A deletion marker was found here |
| 3 | `found_merge_operand` | One or more merge operands were found here |
| 4 | `error` | I/O error, corruption, or unexpected blob index |

`final_result` (per request):

| Value | Name |
|---|---|
| 0 | `not_found` |
| 1 | `found` |
| 2 | `deleted` |
| 3 | `error` |
| 4 | `range_deleted` |

`range_deleted` means the scan stopped early because a range tombstone covered
the key -- detectable via `*max_covering_tombstone_seq > 0`. It is a genuinely
different reason for "not found" than an exhausted search, so keep it distinct.

### 4.4 Truncation

When the file exceeds `max_trace_file_size`, stop writing. Before stopping,
append one final line:

```
# truncated
```

so the analyzer can distinguish truncation from a clean end. (The existing
`BlockCacheTraceWriterImpl` truncates silently at
`trace_replay/block_cache_tracer.cc:119`; do better here.)

---

## 5. Implementation phases

Do these in order. Each phase should compile on its own.

### Phase 1 -- Tracer core

**Create `include/rocksdb/key_lookup_trace_options.h`** (public header, needs
the standard Meta dual-license header):

```cpp
namespace ROCKSDB_NAMESPACE {

// How a data block is identified in a key lookup trace.
enum class KeyLookupBlockIdMode : uint8_t {
  // Zero-based index of the block among the file's data blocks, in offset
  // order. Requires building a per-file offset map on first traced access.
  kOrdinal,
  // Raw byte offset of the block within the file. No setup cost, but the
  // mapping back to a block index requires the SST file to still exist.
  kOffset,
};

struct KeyLookupTraceOptions {
  // Capture one in every `sampling_frequency` lookups. 0 or 1 captures all.
  uint64_t sampling_frequency = 1;

  // Stop writing once the trace file exceeds this size.
  uint64_t max_trace_file_size = uint64_t{64} * 1024 * 1024 * 1024;

  // Whether to record the data blocks read from each file. Setting this to
  // false records only the file sequence and skips all block-capture work.
  bool record_blocks = true;

  KeyLookupBlockIdMode block_id_mode = KeyLookupBlockIdMode::kOrdinal;
};

}  // namespace ROCKSDB_NAMESPACE
```

**Create `trace_replay/key_lookup_tracer.h` / `.cc`.** This layer must not
depend on `db/` or `table/` headers -- it deals in plain structs only.

```cpp
enum class KeyLookupOutcome : uint8_t {
  kNotFound = 0, kFoundValue = 1, kFoundTombstone = 2,
  kFoundMergeOperand = 3, kError = 4,
};

enum class KeyLookupResult : uint8_t {
  kNotFound = 0, kFound = 1, kDeleted = 2, kError = 3, kRangeDeleted = 4,
};

struct KeyLookupProbe {
  uint32_t level = 0;
  uint64_t file_number = 0;
  KeyLookupOutcome outcome = KeyLookupOutcome::kNotFound;
  uint32_t first_block_idx = 0;  // index into the flat block id buffer
  uint32_t num_blocks = 0;
};

// Writes key lookup records as plain text. Not thread safe; callers
// serialize through KeyLookupTracer.
class KeyLookupTraceWriter {
 public:
  Status NewWritableFile(const std::string& path, Env* env,
                         KeyLookupBlockIdMode mode);
  Status WriteLookup(uint64_t timestamp_us, uint64_t lookup_id, uint32_t cf_id,
                     KeyLookupResult final_result,
                     const KeyLookupProbe* probes, size_t num_probes,
                     const uint64_t* block_ids, size_t num_block_ids,
                     uint64_t max_trace_file_size);
  ~KeyLookupTraceWriter();

 private:
  std::unique_ptr<WritableFile> file_;
  std::string line_buffer_;  // reused across records; no per-record alloc
  uint64_t bytes_written_ = 0;
  bool truncated_ = false;
};

class KeyLookupTracer {
 public:
  Status StartTrace(const KeyLookupTraceOptions& options,
                    const std::string& trace_file_path, Env* env);
  void EndTrace();

  bool is_tracing_enabled() const {
    return writer_.load(std::memory_order_relaxed) != nullptr;
  }
  const KeyLookupTraceOptions& options() const { return trace_options_; }

  // Returns 0 if this lookup should not be traced (sampling), otherwise a
  // nonzero id. Cycles like BlockCacheTracer::NextGetId; 0 is reserved.
  uint64_t NextLookupId();

  Status WriteLookup(...);  // takes the mutex, forwards to the writer

 private:
  KeyLookupTraceOptions trace_options_;
  InstrumentedMutex trace_writer_mutex_;
  std::atomic<KeyLookupTraceWriter*> writer_{nullptr};
  std::atomic<uint64_t> lookup_id_counter_{1};
};
```

Model the lifecycle on `BlockCacheTracer`
(`trace_replay/block_cache_tracer.h:203`, implementation at
`trace_replay/block_cache_tracer.cc:449-500`): atomic writer pointer for the
lock-free enabled check, `InstrumentedMutex` for writes, re-check the pointer
under the lock in `WriteLookup`, `EndTrace()` from the destructor.

Build the file with `Env::NewWritableFile` exactly as
`BlockCacheHumanReadableTraceWriter::NewWritableFile` does
(`trace_replay/block_cache_tracer.cc:320`). Flush and close in the destructor
with `PermitUncheckedError()`.

For `WriteLookup`, clear and reuse `line_buffer_` rather than allocating. Do not
use a fixed stack buffer -- `num_probes` and block counts are unbounded in
principle (L0 can hold many overlapping files).

`NextLookupId()` applies sampling: take `fetch_add`, skip the reserved 0 value,
and return 0 when `id % sampling_frequency != 0`. Sampling must be decided here,
*before* any capture work, so unsampled requests pay nothing.

Temporal sampling is correct for this tracer. (Block cache tracing must sample
*spatially* by block key to preserve complete per-block histories -- see
`ShouldTrace` at `trace_replay/block_cache_tracer.cc:24`. That constraint does
not apply here, because the unit of analysis is the request itself.)

### Phase 2 -- Plumbing

Four files, mirroring how `block_cache_tracer_` is threaded.

1. `db/db_impl/db_impl.h:1532` -- add `KeyLookupTracer key_lookup_tracer_;`
   next to `BlockCacheTracer block_cache_tracer_;`. Declare the two public
   API overrides (phase 6).
2. `db/db_impl/db_impl.cc:292` -- add `&key_lookup_tracer_` to the `VersionSet`
   constructor call (anchor: `versions_.reset(new VersionSet(`).
3. `db/version_set.h:1251` -- add a
   `KeyLookupTracer* const key_lookup_tracer` parameter to the `VersionSet`
   constructor; add the member at `:1884` next to
   `BlockCacheTracer* const block_cache_tracer_;`.
4. `db/version_set.cc:5381` -- add the initializer next to
   `block_cache_tracer_(block_cache_tracer),`.

Search for every other `new VersionSet(` / `VersionSet(` construction site
(tests, `db_impl_secondary`, repair, `ldb`) and update them. Do not assume there
is only one.

### Phase 3 -- Probe capture in `Version::Get`

**Create `db/key_lookup_trace_scope.h`** -- an include-guarded header, because
of the double-inclusion constraint in section 2.4. It holds the RAII emitter,
which is the piece that needs to know about `GetContext`:

```cpp
// Emits one key lookup trace record when it goes out of scope, covering every
// return path out of Version::Get.
class KeyLookupTraceScope {
 public:
  KeyLookupTraceScope(KeyLookupTracer* tracer, uint64_t lookup_id,
                      uint32_t cf_id, SystemClock* clock,
                      const Status* status, const GetContext* get_context,
                      const SequenceNumber* max_covering_tombstone_seq,
                      const autovector<KeyLookupProbe, 8>* probes,
                      const autovector<uint64_t, 16>* block_ids)
      : tracer_(tracer), ... {}

  ~KeyLookupTraceScope() {
    if (tracer_ == nullptr) return;
    tracer_->WriteLookup(clock_->NowMicros(), lookup_id_, cf_id_,
                         ComputeFinalResult(), probes_->data(), probes_->size(),
                         block_ids_->data(), block_ids_->size())
        .PermitUncheckedError();
  }

 private:
  KeyLookupResult ComputeFinalResult() const;
  ...
};
```

`ComputeFinalResult()`:

- `!status_->ok() && !status_->IsNotFound()` -> `kError`
- `get_context_->State() == kFound` -> `kFound`
- `get_context_->State() == kDeleted` -> `kDeleted`
- `*max_covering_tombstone_seq_ > 0` -> `kRangeDeleted`
- otherwise -> `kNotFound`

Also add a free function here:

```cpp
KeyLookupOutcome ClassifyProbeOutcome(const Status& status,
                                      GetContext::GetState state,
                                      size_t operands_before,
                                      size_t operands_after);
```

| Condition | Result |
|---|---|
| `!status.ok()` | `kError` |
| `state == kFound` | `kFoundValue` |
| `state == kDeleted` | `kFoundTombstone` |
| `state == kMerge && operands_after > operands_before` | `kFoundMergeOperand` |
| `state == kMerge` (no change), `state == kNotFound` | `kNotFound` |
| `kCorrupt`, `kUnexpectedBlobIndex`, `kMergeOperatorFailed` | `kError` |

> **The operand-count delta is load-bearing.** `GetContext::State()` is
> *cumulative* across files -- once it reaches `kMerge` it stays `kMerge`
> whether or not the current file contributed anything. Comparing states alone
> would mark every file after the first merge operand as a hit. This is the
> single easiest thing to get wrong in this plan.

**Now edit `db/version_set_sync_and_async.h`.** Function body only, no type
definitions.

After the `GetContext get_context(...)` construction (currently ends at line 52)
and before the `FilePicker fp(...)` at line 58:

```cpp
autovector<uint64_t, 16> block_ids;
autovector<KeyLookupProbe, 8> probes;
KeyLookupTracer* klt = vset_ ? vset_->key_lookup_tracer_ : nullptr;
uint64_t lookup_id = 0;
if (klt && klt->is_tracing_enabled()) {
  lookup_id = klt->NextLookupId();   // 0 means "not sampled"
  if (lookup_id != 0 && klt->options().record_blocks) {
    get_context.SetBlockSink(&block_ids);
  }
}
KeyLookupTraceScope klt_scope(lookup_id != 0 ? klt : nullptr, lookup_id,
                              cfd_->GetID(), clock_, status, &get_context,
                              max_covering_tombstone_seq, &probes, &block_ids);
```

> **Declaration order is a correctness requirement.** `block_ids` and `probes`
> must be declared *before* `klt_scope`. Destructors run in reverse declaration
> order, so the scope is destroyed first and the buffers it reads are still
> alive. Reversing this is a use-after-free that will not reliably crash in
> tests. Add a comment saying so.

Inside the loop, capture the "before" values immediately before the
`table_cache_->Get` call (before line 80, anchor `*status =`):

```cpp
const size_t blocks_before = block_ids.size();
const size_t operands_before = merge_context->GetNumOperands();
```

Record the probe immediately after the call returns and **before** the
`if (!status->ok())` early return at line 95 -- otherwise error probes are lost:

```cpp
if (lookup_id != 0) {
  probes.push_back({fp.GetHitFileLevel(), f->fd.GetNumber(),
                    ClassifyProbeOutcome(*status, get_context.State(),
                                         operands_before,
                                         merge_context->GetNumOperands()),
                    static_cast<uint32_t>(blocks_before),
                    static_cast<uint32_t>(block_ids.size() - blocks_before)});
}
```

Do **not** record a probe for the `*max_covering_tombstone_seq > 0` break at
line 66. That check fires before the file is searched, so no probe occurred.

### Phase 4 -- Block capture

**`table/get_context.h`** -- add to `GetContext`:

```cpp
// Sink for data block ids read on behalf of this lookup, owned by the caller
// (Version::Get). Null when key lookup tracing is off or not sampled.
autovector<uint64_t, 16>* block_sink_ = nullptr;

public:
  void SetBlockSink(autovector<uint64_t, 16>* sink) { block_sink_ = sink; }
  autovector<uint64_t, 16>* block_sink() const { return block_sink_; }
```

Use a setter, not a constructor parameter. `GetContext` has two constructors
with many call sites; adding a parameter churns all of them for one caller.

**`table/block_based/block_based_table_reader_sync_and_async.h`** -- inside the
index loop at line 571, right after `IndexValue v = iiter->value();` (line 572)
and before the `CO_AWAIT(NewDataBlockIterator, ...)` at line 593:

```cpp
if (get_context != nullptr && get_context->block_sink() != nullptr) {
  get_context->block_sink()->push_back(
      BlockIdForTrace(read_options, v.handle.offset()));
}
```

Place it after the `first_internal_key` early-`break` check at line 574, so a
block that is skipped rather than read is not recorded.

`BlockBasedTable` needs no tracer pointer -- the null sink check is the whole
integration.

### Phase 5 -- Ordinal resolution

RocksDB has no block ordinal anywhere. Blocks are identified by
`BlockHandle{offset, size}` throughout, and `BlockIter` tracks only `current_`
(a byte offset within the index block buffer) and `restart_index_`
(`table/block_based/block.h:464-467`) -- neither is an entry index. Since
`iiter->Seek(key)` jumps to an arbitrary position, you cannot count your way
there either.

So `kOrdinal` mode requires a per-file sorted list of data block offsets.

**Add to `BlockBasedTable::Rep`** (`table/block_based/block_based_table_reader.h`):

```cpp
// Sorted ascending. Built lazily on first traced access, only when key
// lookup tracing is active in kOrdinal mode.
mutable std::mutex data_block_offsets_mutex;
mutable std::vector<uint64_t> data_block_offsets;
enum class OffsetMapState : uint8_t { kUnbuilt, kReady, kFailed };
mutable OffsetMapState offset_map_state = OffsetMapState::kUnbuilt;
```

**Add `BlockBasedTable::BlockIdForTrace(const ReadOptions&, uint64_t offset)`:**

- In `kOffset` mode, return `offset` unchanged.
- In `kOrdinal` mode, ensure the map is built, then
  `std::lower_bound(offsets.begin(), offsets.end(), offset) - offsets.begin()`.
- If the map is `kFailed` or the offset is not found, return `UINT64_MAX` so the
  analyzer sees an explicit sentinel rather than a plausible wrong ordinal.

Building the map:

```cpp
ReadOptions build_ro;
build_ro.fill_cache = false;        // do not pollute the block cache
build_ro.verify_checksums = ro.verify_checksums;
BlockCacheLookupContext ctx(TableReaderCaller::kUncategorized);
IndexBlockIter iiter_on_stack;
auto index_iter = NewIndexIterator(build_ro, /*disable_prefix_seek=*/true,
                                   &iiter_on_stack, /*get_context=*/nullptr,
                                   &ctx);
std::vector<uint64_t> offsets;
for (index_iter->SeekToFirst(); index_iter->Valid(); index_iter->Next()) {
  offsets.push_back(index_iter->value().handle.offset());
}
```

Index order equals offset order for data blocks, so the result is already
sorted; assert that in debug builds rather than re-sorting.

Two details that matter:

- **Use `kUncategorized` as the caller**, so this walk does not inject fake
  `kUserGet` records into any concurrent block cache trace. See the enum comment
  at `include/rocksdb/table_reader_caller.h:35`.
- **Build outside the lock, install under it.** The walk performs I/O; holding
  the mutex across it would stall every concurrent reader of that table. Use
  double-checked installation: read state under lock, build unlocked, re-take
  the lock and install only if still `kUnbuilt`.

> ⚠️ **Partitioned indexes.** With `kTwoLevelIndexSearch`, walking the full index
> reads *every* index partition. That is real I/O that perturbs the workload
> being measured. `fill_cache = false` limits the cache damage but not the I/O.
> Document this in the `KeyLookupBlockIdMode` comment and recommend `kOffset`
> mode for partitioned-index workloads.

Memory: 8 bytes per data block per open table. A 256 MB SST at 4 KB blocks is
~64 K blocks, about 512 KB. Only build when tracing is on.

### Phase 6 -- Public API

**`include/rocksdb/db.h`**, next to `StartBlockCacheTrace` at `:2568`:

```cpp
// Trace the sequence of SST files searched by each Get(), and the data
// blocks read from each. Output is a plain text file at `trace_file_path`.
// Use EndKeyLookupTrace() to stop tracing.
//
// Only point Get() is traced; MultiGet, iterators, and memtable lookups are
// not. Tracing serializes trace writes through a mutex and measurably
// reduces read throughput; it is a diagnostic tool, not a production mode.
virtual Status StartKeyLookupTrace(
    const KeyLookupTraceOptions& /*options*/,
    const std::string& /*trace_file_path*/) {
  return Status::NotSupported("StartKeyLookupTrace() is not implemented.");
}

virtual Status EndKeyLookupTrace() {
  return Status::NotSupported("EndKeyLookupTrace() is not implemented.");
}
```

**`include/rocksdb/utilities/stackable_db.h`** -- forward both, following the
pattern at `:512-526`.

**`db/db_impl/db_impl.cc`** -- implement next to `DBImpl::StartBlockCacheTrace`
at `:7414`.

Follow `claude_md/add_public_api.md` for the full public-API checklist
(C API bindings in `db/c.cc` and `tools/c_api_gen/c_base.cc`, Java bindings).

### Phase 7 -- Build wiring and tooling

Per `CLAUDE.md`, a new `.cc` file must be registered in **all** build systems:

| File | What to add |
|---|---|
| `src.mk` | `trace_replay/key_lookup_tracer.cc` next to `block_cache_tracer.cc` (`:246`); test in the test-sources list near `:630` |
| `CMakeLists.txt` | source near `:991`, test near `:1608` |
| `Makefile` | link rule modeled on `block_cache_tracer_test` at `:2255` |
| `BUCK` | **do not hand-edit**; run `/usr/local/bin/python3 buckifier/buckify_rocksdb.py` after updating `src.mk` |

**`tools/db_bench_tool.cc`** -- add `--key_lookup_trace_file`,
`--key_lookup_trace_sampling_frequency`, and `--key_lookup_trace_record_blocks`,
mirroring the block cache trace flags at `:1321-1330` and their use at
`:4659-4697` and `:4740`.

**`HISTORY.md`** -- one line under the current unreleased "New Features":

```
* Added `DB::StartKeyLookupTrace()` for tracing which SST files and data blocks each `Get()` reads.
```

Keep it to that. Release notes are for what users care about, not
implementation detail.

### Phase 8 -- Tests

**`trace_replay/key_lookup_tracer_test.cc`** -- format-level unit tests, no DB:

- Header line contents for both `block_id_mode` values.
- A record with zero probes (trailing comma, empty sixth field).
- A probe with zero blocks (exactly three colon-separated fields, no trailing colon).
- A probe with several blocks.
- All five `outcome` values and all five `final_result` values.
- Truncation at `max_trace_file_size` appends `# truncated`.
- Sampling: with `sampling_frequency = 4`, exactly one in four `NextLookupId()`
  calls returns nonzero.

**DB-level integration tests** in the same file, using `DBTestBase`:

Build a deterministic LSM with explicit `Put` + `Flush` + `CompactRange` so file
numbers and levels are known, then assert exact probe sequences:

1. Key present only in the bottom level -- assert every higher-level file that
   is probed reports `not_found`, and the last reports `found_value`.
2. Key deleted in L0, present in L2 -- assert `found_tombstone` on the L0 file
   and that the scan stops there (`final_result = deleted`).
3. Merge operands spread across two files -- assert `found_merge_operand` on
   both, and specifically that the *second* file is not misreported. This is the
   regression test for the cumulative-state bug in phase 3.
4. Key absent everywhere with bloom filters on -- assert probes exist with empty
   block lists.
5. Key served from the memtable -- assert `num_probes == 0`.
6. Range tombstone covering the key -- assert `final_result = range_deleted`.
7. Block ordinals: write enough keys to produce several data blocks per file,
   Get a key known to live in a specific block, assert the recorded ordinal.
   Cross-check against `sst_dump --command=raw`.
8. Same as 7 in `kOffset` mode, asserting offsets are distinct and ascending.

Per `CLAUDE.md`: no `sleep` for synchronization -- use sync points. Cap each
test at 60 seconds. Extract shared helpers (LSM construction, trace file
parsing) rather than copy-pasting between cases; do this as a cleanup pass once
all tests are written.

Flakiness check:

```bash
COERCE_CONTEXT_SWITCH=1 build_tools/rockstest.sh key_lookup_tracer_test -r100 \
    --gtest_filter="*KeyLookup*"
```

---

## 6. Invariants and pitfalls

Ordered roughly by how likely they are to bite.

1. **Cumulative `GetContext::State()`.** Covered above. Use the operand delta.
2. **Declaration order of `probes` / `block_ids` vs `klt_scope`.** Buffers first.
3. **No type definitions in `version_set_sync_and_async.h`.** Double inclusion.
4. **Probe recorded before the `!status->ok()` early return**, or error probes
   vanish.
5. **`Version::Get` has 10 `CO_RETURN` exit points** (lines 99, 154, 165, 172,
   175, 181, 184, 194, 199, 223). Do not hand-place emit calls at any of them --
   that is what the RAII scope is for.
6. **Filter-negative and row-cache-hit probes both produce empty block lists.**
   Row cache is served inside `TableCache::Get` (`db/table_cache.cc:566`)
   without ever calling `BlockBasedTable::Get`. The `outcome` field
   distinguishes them; do not treat "no blocks" as "filter said no".
7. **"Files searched" is not exactly "files whose bloom filter ran."** Three
   cases where a file is searched with no filter check:
   `optimize_filters_for_hits` skipping the bottom level
   (`Version::IsFilterSkipped`, `db/version_set.cc:3164`); a file with no
   filter block; a row cache hit. Document this in the public API comment.
8. **File numbers are never reused, but files are compacted away.** In `kOffset`
   mode, the offset-to-ordinal mapping is unrecoverable after the SST is gone.
   This is why `kOrdinal` is the default.
9. **`ASSERT_STATUS_CHECKED`.** Every `Status` must be checked or explicitly
   `PermitUncheckedError()`, including in the scope destructor.
10. **No `dynamic_cast`** in production code. Use `static_cast_with_check` from
    `util/cast_util.h` if you need a downcast.
11. **ASCII only.** No em dashes, no smart quotes, in code or comments.
    `make check-sources` will reject them.

---

## 7. Performance requirements

`CLAUDE.md` treats performance as a hard requirement, and this touches the
hottest read path in the engine.

**When tracing is off, the cost must be:**

- `Version::Get`: one relaxed atomic load plus a predictable branch.
- `BlockBasedTable::Get`: one null-pointer check per data block access.
- `GetContext`: 8 bytes larger. Nothing else.
- Two empty `autovector`s constructed per `Get` (stack only, no allocation).

**When tracing is on:**

- One mutex acquisition per *request*, not per file or per block. This is the
  main reason the record is assembled in a stack buffer and written once.
- No heap allocation in the steady state -- inline `autovector` capacity for the
  common case, and a reused `line_buffer_` in the writer.
- The ordinal map is built at most once per table per trace session.

**Benchmark before and after** with a release build:

```bash
AUTO_CLEAN=1 DEBUG_LEVEL=0 make -j$(nproc) db_bench
# baseline
./db_bench --benchmarks=readrandom --use_existing_db --num=10000000
# tracing off (must match baseline within noise)
# tracing on, sampling_frequency=1 and =100
```

Report `readrandom` throughput for all four configurations. A measurable
regression with tracing *off* is a bug, not a tradeoff.

---

## 8. Optional extension: index and filter blocks

**Do not build this unless explicitly asked.** It changes what "the ith block"
means and needs its own ordinal space.

Index and filter blocks are accessed at different call sites in
`BlockBasedTable::Get`: `NewIndexIterator` at
`table/block_based/block_based_table_reader_sync_and_async.h:545`, and
`FullFilterKeyMayMatch` just before it.

If asked, extend the probe's block list to tagged entries rather than bare
integers -- for example `d12` for data block 12, `i0` for index partition 0,
`f0` for a filter block -- so the three block classes stay distinguishable in
one sequence. Partitioned index and filter blocks would each need their own
lazily built ordinal map, following the same pattern as phase 5.

Keep the existing format valid: an untagged integer must continue to mean a data
block ordinal, so traces from the base implementation stay parseable.

---

## 9. Verification checklist

Run all of these before declaring the work done.

```bash
make format-auto
make check-sources
AUTO_CLEAN=1 make -j$(nproc) check
AUTO_CLEAN=1 ASSERT_STATUS_CHECKED=1 make -j$(nproc) check
```

Use `make check-progress` to poll long runs rather than blocking on them.

- [ ] All four build systems updated (`Makefile`, `CMakeLists.txt`, `src.mk`,
      `BUCK` via `buckify_rocksdb.py`)
- [ ] License header on every new file
- [ ] Public API documented, including the "not exactly bloom filter checks"
      caveat and the throughput warning
- [ ] `make check` passes
- [ ] `ASSERT_STATUS_CHECKED=1 make check` passes
- [ ] `make check-sources` passes (no non-ASCII)
- [ ] 100-iteration `COERCE_CONTEXT_SWITCH=1` run is clean
- [ ] `db_bench` shows no regression with tracing off
- [ ] `db_bench` numbers recorded for tracing on at two sampling rates
- [ ] Trace file from a real `db_bench` run parses and the file sequences are
      consistent with the LSM shape reported by `ldb --command=dump_live_files`
