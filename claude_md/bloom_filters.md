# Bloom Filters in RocksDB

A walkthrough of every place Bloom filters (and their Ribbon sibling) appear in
this RocksDB tree: what each class and function does, and how filters are
built, stored, cached and consulted.

All paths are relative to `lib/rocksdb/`. Line numbers refer to branch
`memory-project` at commit `76be640a7`.

---

## Contents

1. [The short version](#1-the-short-version)
2. [Background](#2-background)
3. [Where filters live](#3-where-filters-live)
4. [Source map](#4-source-map)
5. [Public API](#5-public-api)
6. [Filter policies](#6-filter-policies)
7. [Bits builders and bits readers](#7-bits-builders-and-bits-readers)
8. [Hashing and probing algorithms](#8-hashing-and-probing-algorithms)
9. [On-disk format](#9-on-disk-format)
10. [Filter blocks: full and partitioned](#10-filter-blocks-full-and-partitioned)
11. [Write path: building filters](#11-write-path-building-filters)
12. [Read path: opening a table](#12-read-path-opening-a-table)
13. [Read path: Get](#13-read-path-get)
14. [Read path: MultiGet](#14-read-path-multiget)
15. [Read path: iterators and prefix seek](#15-read-path-iterators-and-prefix-seek)
16. [Memtable Bloom filter (DynamicBloom)](#16-memtable-bloom-filter-dynamicbloom)
17. [PlainTable Bloom filter](#17-plaintable-bloom-filter)
18. [Caching and memory accounting](#18-caching-and-memory-accounting)
19. [Options reference](#19-options-reference)
20. [Statistics, perf counters, properties](#20-statistics-perf-counters-properties)
21. [Correctness rules and gotchas](#21-correctness-rules-and-gotchas)
22. [Call-chain cheat sheet](#22-call-chain-cheat-sheet)
23. [Implementing a new filter policy](#23-implementing-a-new-filter-policy)
24. [Per-file access history for filter block cache admission](#24-per-file-access-history-for-filter-block-cache-admission)

---

## 1. The short version

- RocksDB has **three independent Bloom filter implementations**:
  1. **SST filters for block-based tables.** This is the main one. A
     `FilterBitsBuilder` builds the filter during flush or compaction, it is
     stored as a meta block in the SST, and it is read through a
     `FilterBlockReader`. Configured by `BlockBasedTableOptions::filter_policy`.
  2. **Memtable filter.** `DynamicBloom` is an in-memory filter that supports
     concurrent inserts. Configured by `memtable_prefix_bloom_size_ratio`.
  3. **PlainTable filter.** `PlainTableBloomV1` is legacy and used only by
     PlainTable SST files.
- A filter answers "definitely not here" or "maybe here". It only speeds up
  lookups for keys that are absent. If anything goes wrong while reading a
  filter, RocksDB treats the answer as "maybe here".
- A filter stores **user keys with the timestamp stripped**: no sequence
  number and no value type. It can also store their **prefixes** (from
  `prefix_extractor`), or both.
- An SST filter has one of two shapes. A **full filter** is one filter block
  per SST. A **partitioned filter** is many small filter blocks plus a
  top-level index over them.
- There are two current SST Bloom encodings: **FastLocalBloom**
  (`format_version >= 5`, the default) and **LegacyBloom**
  (`format_version < 5`). **Ribbon** is a space-saving alternative in the same
  family. Every built-in policy can read every built-in format.

---

## 2. Background

### 2.1 What a Bloom filter is

A Bloom filter is an array of `m` bits plus `k` hash functions.

- **Add(key):** set the `k` bits chosen by the hashes.
- **Query(key):** if any of the `k` bits is 0, the key is definitely absent.
  Otherwise it *may* be present.

With `b = m / n` bits per key, the false-positive (FP) rate is about
`(1 - e^(-k/b))^k`, and the best `k` is about `b * ln 2`. Ten bits per key
gives roughly a 1% FP rate. `BloomMath::StandardFpRate` implements this
formula.

### 2.2 Why an LSM-tree needs them

A point lookup may have to check the memtable, the immutable memtables, every
overlapping L0 file and one file in each of L1..Ln. For each candidate SST that
the filter does not rule out, `BlockBasedTable::Get` seeks the index and then
reads the data block the index points at. A filter lets RocksDB skip files that
do not contain the key, so it removes both of those accesses for a file.

How many block accesses the index costs depends on
`BlockBasedTableOptions::index_type` (table.h:298):

| `index_type` | Index blocks per file | Block accesses per lookup |
|---|---|---|
| `kBinarySearch` (default) | 1, however large | 1 index + 1 data |
| `kBinarySearchWithFirstKey` | 1 | 1 index + 1 data, but the data block is skipped when the key falls in the gap before the block's first key (..._reader_sync_and_async.h:585) |
| `kTwoLevelIndexSearch` | 1 top-level + N partitions | 2 index (top-level, then one partition) + 1 data |
| `kHashSearch` | 1, plus two prefix meta blocks read once at open | 1 index + 1 data |

**Which index block to read.** With the default `kBinarySearch` the question
does not arise: `ShortenedIndexBuilder::Finish` returns `OK` with a single block
(index_builder.h:437), so `WriteIndexBlock`
(block_based_table_builder.cc:2951) writes one index block and never enters its
`Incomplete` loop. There is nothing to choose. That block grows with the number
of data blocks and can reach hundreds of KB on a large SST, but it is still one
block, which is the cost that motivated partitioned indexes.

Many index blocks per file happens only with `kTwoLevelIndexSearch`, where
`PartitionedIndexBuilder::Finish` returns `Incomplete` once per partition
(index_builder.h:696), cutting them at `metadata_block_size`
(index_builder.cc:206), and emits a top-level index over them. Choosing a
partition works exactly like choosing a filter partition
([10.8](#108-partitionedfilterblockreader-partitioned_filter_blockcc)): the
top-level index maps a separator key to each partition's `BlockHandle`, and
`PartitionedIndexIterator::SeekImpl` (partitioned_index_iterator.cc:16) seeks
the top level, calls `InitPartitionedIndexBlock` to fetch just that one
partition, then seeks inside it. `partition_filters` requires this index type
(block_based_table_factory.cc:495), so partitioned filters and partitioned
indexes are always seen together.

**The index access is usually not disk I/O.** `cache_index_and_filter_blocks`
defaults to false, in which case `BinarySearchIndexReader::Create` reads the
index at open and the table reader owns it for its lifetime
(binary_search_index_reader.cc, same pattern as filters in
[section 12](#12-read-path-opening-a-table)). The lookup then hits memory. When
that option is true the index block is a block-cache lookup, which can miss and
go to disk; index partitions always go through the block cache.

So the accurate version of the usual claim: without filters, each candidate SST
costs one or two index accesses (normally memory) plus one data block read
(often disk). Filters are worth it because they eliminate that data block read
for files that do not hold the key, leaving a typical `Get` reading about one
data block in total.

### 2.3 Cache-local ("blocked") Bloom filters

Confining all `k` probes for a key to one 64-byte cache line makes a query cost
one cache miss instead of `k`. The price is a slightly higher FP rate, because
keys are not spread perfectly evenly across cache lines.
`BloomMath::CacheLocalFpRate` estimates it.

Every filter on the block-based-table path is cache-local: FastLocalBloom uses
explicit 64-byte blocks, and LegacyBloom uses `LegacyLocalityBloomImpl<false>`.
The memtable's `DynamicBloom` is cache-local in effect rather than by
construction -- it probes `kNumDoubleProbes` words at bit-xor offsets from a
start word, which with the hard-coded 6 probes is 3 words inside an aligned
32-byte block, so one cache line.

The exception is the PlainTable filter with `bloom_locality = 0`, which is the
default for that option. `PlainTableBloomV1::SetTotalBits` then leaves
`kNumBlocks = 0` and queries fall through to `LegacyNoLocalityBloomImpl`, which
spreads probes over the whole bit array at one cache miss per probe
([8.4](#84-legacynolocalitybloomimpl-utilbloom_implh354)). No block-based table
ever uses that path.

### 2.4 Vocabulary used in the code

| Term | Meaning |
|---|---|
| Whole-key filtering | The full user key is added (`whole_key_filtering`, default true) |
| Prefix filtering | `prefix_extractor->Transform(key)` is added, which allows filtering for prefix seeks |
| Full filter | One filter covering the whole SST file, stored as exactly one SST block however large it gets (`partition_filters = false`, the default) |
| Partitioned filter | The filter is split into partitions of about `metadata_block_size`, each its own SST block, plus an index over the partitions |
| Filter policy | `FilterPolicy`: decides which kind of filter to build and creates readers |
| Bits builder / bits reader | `FilterBitsBuilder` / `FilterBitsReader`: the raw filter algorithm working on bytes |
| Filter block builder / reader | `FilterBlockBuilder` / `FilterBlockReader`: the SST layer that feeds keys to a bits builder, and that finds and loads filter blocks |
| millibits per key | bits per key * 1000, kept as an integer internally |
| "Alt" entry | A prefix added together with its key through `AddKeyAndAlt` |

**Careful: "block" means two different things.** The filter code uses the word
for both, and they are not nested units of the same thing.

| "Block" | What it is | How many in a full filter |
|---|---|---|
| SST block | A unit of I/O and block-cache residency, addressed by a `BlockHandle` from the metaindex, with its own 5-byte trailer and checksum | Exactly 1 |
| Bloom filter block | The 64-byte region a key's probes are confined to, addressed as a byte offset inside the filter buffer | Thousands (`len / 64`) |

A full filter is subdivided into 64-byte filter blocks -- that is why
`CalculateSpace` rounds its length up to a multiple of 64 and why
`GetBloomBitsReader` accepts only `log2_block_bytes == 6`. But those are offsets
into one contiguous buffer, not separately addressable SST blocks: nothing can
read, cache or evict one of them on its own. When this document says "filter
block" unqualified it means the SST block.

The Ribbon trailer's `num_blocks` field is a third, unrelated use of the word.
`GetNumBlocks()` on the interleaved solution returns `num_slots / kCoeffBits`
(ribbon_impl.h:837), so one block there is a group of `kCoeffBits` slots -- 128
for Standard128Ribbon, since its `CoeffRow` is `Unsigned128`.

---

## 3. Where filters live

```
                        Get / MultiGet / Iterator
                                  |
       +--------------------------+-------------------------------+
       |                          |                               |
  Mutable memtable         Immutable memtables              SST files (Version)
  DynamicBloom             DynamicBloom (one each)          L0 files, L1..Ln
  (db/memtable.cc)                                                |
                                                +-----------------+------------+
                                                |                              |
                                          BlockBasedTable                 PlainTable
                                          FilterBlockReader               PlainTableBloomV1
                                           (Full / Partitioned)
                                            -> FilterBitsReader
                                               FastLocalBloom | LegacyBloom | Ribbon
```

The SST filter stack, from top to bottom:

```
 Layer          Main classes                                       Responsibility
 -------------  -------------------------------------------------  ---------------------------
 DB             Version::Get / MultiGet, LevelIterator              decides skip_filters
                  (db/version_set.cc)
 Table cache    TableCache::Get / MultiGetFilter                     opens table readers
                  (db/table_cache*.{cc,h})
 Table          BlockBasedTable, BlockBasedTableBuilder              SST layout, stats
                  (table/block_based/block_based_table_*.cc)
 Filter block   FullFilterBlock{Builder,Reader}                      which keys go in, where
                PartitionedFilterBlock{Builder,Reader}               the blocks are
                  (table/block_based/*filter_block*.{h,cc})
 Policy         FilterPolicy -> BloomFilterPolicy / RibbonFilterPolicy   which algorithm
                  (include/rocksdb/filter_policy.h, filter_policy.cc)
 Bits           FilterBitsBuilder / FilterBitsReader                 filter bytes
                  (table/block_based/filter_policy.cc)
 Algorithm      FastLocalBloomImpl, LegacyLocalityBloomImpl,         bit math, hashing
                BloomMath (util/bloom_impl.h), util/hash.h
```

---

## 4. Source map

| File | What is in it |
|---|---|
| `include/rocksdb/filter_policy.h` | Public API: `FilterPolicy`, `FilterBuildingContext`, `NewBloomFilterPolicy`, `NewRibbonFilterPolicy` |
| `include/rocksdb/table.h` | Filter settings in `BlockBasedTableOptions` (`filter_policy`, `whole_key_filtering`, `partition_filters`, ...) |
| `include/rocksdb/advanced_options.h` | `memtable_prefix_bloom_size_ratio`, `memtable_whole_key_filtering`, `optimize_filters_for_hits`, `bloom_locality` |
| `include/rocksdb/options.h` | `prefix_extractor`; `ReadOptions::total_order_seek`, `auto_prefix_mode`, `prefix_same_as_start` |
| `include/rocksdb/statistics.h`, `perf_context.h` | Bloom tickers and perf counters |
| `include/rocksdb/cache.h` | `CacheEntryRole::kFilterBlock`, `kFilterMetaBlock`, `kFilterConstruction` |
| `table/block_based/filter_policy_internal.h` | `FilterBitsBuilder`, `FilterBitsReader`, the policy class hierarchy |
| `table/block_based/filter_policy.cc` | All built-in builders and readers, policy logic, reader selection from metadata, string factory |
| `util/bloom_impl.h` | `BloomMath`, `FastLocalBloomImpl`, `LegacyNoLocalityBloomImpl`, `LegacyLocalityBloomImpl` |
| `util/hash.h` | `GetSliceHash64` (64-bit XXH3), `BloomHash` (legacy 32-bit) |
| `util/dynamic_bloom.{h,cc}` | `DynamicBloom` (memtable filter) |
| `table/block_based/filter_block.h` | Abstract `FilterBlockBuilder` and `FilterBlockReader` |
| `table/block_based/filter_block_reader_common.{h,cc}` | Shared reader logic: load or cache the filter block, `RangeMayExist` |
| `table/block_based/parsed_full_filter_block.{h,cc}` | The cached object: filter bytes plus a `FilterBitsReader` |
| `table/block_based/full_filter_block.{h,cc}` | Full filter builder and reader |
| `table/block_based/partitioned_filter_block.{h,cc}` | Partitioned filter builder and reader |
| `table/block_based/block_cache.{h,cc}` | How cached filter objects are created (`ParsedFullFilterBlock`, `Block_kFilterPartitionIndex`) |
| `table/block_based/block_based_table_builder.cc` | Creates the filter builder, feeds it keys, writes filter blocks and the metaindex entry |
| `table/block_based/block_based_table_reader.{h,cc}` | Finds the filter on open, creates the reader, `FullFilterKeyMayMatch`, `PrefixRangeMayMatch`, stats |
| `table/block_based/block_based_table_reader_sync_and_async.h` | `BlockBasedTable::Get` and `::MultiGet` (the filter check is the first step) |
| `table/block_based/block_based_table_iterator.{h,cc}` | `CheckPrefixMayMatch` on `Seek` / `SeekForPrev` |
| `table/block_based/block_based_table_factory.cc` | Option registration, `partition_filters` sanitization, property names |
| `db/memtable.{h,cc}` | Memtable bloom: creation, insertion, and checks in Get, MultiGet and iterators |
| `db/version_set.cc`, `db/version_set_sync_and_async.h` | `IsFilterSkipped`, `skip_filters` plumbing, filter pass in async MultiGet |
| `db/table_cache.cc`, `db/table_cache_sync_and_async.h` | `TableCache::Get`, `MultiGetFilter`, `skip_filters` plumbing |
| `db/column_family.cc` | Clamps `memtable_prefix_bloom_size_ratio` to [0, 0.25] |
| `db/compaction/compaction_job.cc`, `db/flush_job.cc` | Pass `is_bottommost`, level and reason into `TableBuilderOptions` |
| `table/plain/plain_table_bloom.{h,cc}`, `plain_table_{builder,reader}.cc` | PlainTable bloom |
| `monitoring/statistics.cc`, `monitoring/perf_context.cc` | Stat names |
| `trace_replay/key_lookup_tracer.h` | (This branch) `KeyLookupOutcome::kNotFound` covers both "the filter excluded the file" and "data blocks were read and the key was absent" |

---

## 5. Public API

File: `include/rocksdb/filter_policy.h`

### 5.1 `FilterPolicy` (filter_policy.h:100)

`class FilterPolicy : public Customizable`. It has three pure virtual methods.

| Method | Role |
|---|---|
| `CompatibilityName()` | The name written into the SST metaindex key. A reader looks for a filter block under this name. Every built-in policy returns `"rocksdb.BuiltinBloomFilter"`, so they can all read each other's filters. |
| `GetBuilderWithContext(const FilterBuildingContext&)` | Returns a new `FilterBitsBuilder` for one filter (one SST, or the partitions of one SST). Returning `nullptr` means "no filter". |
| `GetFilterBitsReader(const Slice& contents)` | Returns a `FilterBitsReader` over raw filter bytes. The caller keeps ownership of the bytes. |

`static FilterPolicy::CreateFromString()` (filter_policy.cc:2034) parses option
strings:

| String | Result |
|---|---|
| `""` or `"nullptr"` | No filter |
| `"bloomfilter:10"` or `"rocksdb.BloomFilter:10"` (a trailing `:false` / `:true` is accepted and ignored) | `NewBloomFilterPolicy(10)` |
| `"ribbonfilter:10"`, `"ribbonfilter:10:2"` | `NewRibbonFilterPolicy(10[, bloom_before_level])` |
| `"rocksdb.BuiltinBloomFilter"` | `ReadOnlyBuiltinFilterPolicy` (reads filters, builds none) |
| `"rocksdb.internal.LegacyBloomFilter:10"`, `...FastLocalBloomFilter:10`, `...Standard128RibbonFilter:10` | Test-only policies fixed to one implementation |

`RegisterBuiltinFilterPolicies` registers the factories into
`ObjectLibrary::Default()` once, using `std::call_once`.

### 5.2 `FilterBuildingContext` (filter_policy.h:48)

A stack-allocated struct passed to `GetBuilderWithContext` so a policy can make
a different choice for each file.

| Field | Meaning |
|---|---|
| `table_options` | `BlockBasedTableOptions` of the table being built |
| `compaction_style`, `num_levels`, `info_log` | From the DB/CF options |
| `column_family_name`, `db_name` | Identification |
| `level_at_creation` | Output level, or -1 if unknown (for example `SstFileWriter`) |
| `is_bottommost` | True if the file goes into the bottommost sorted run for its key range |
| `reason` | `kFlush`, `kCompaction`, `kRecovery`, `kSstFileWriter`, `kMisc` |
| `is_remote_compaction` | Built by a remote compaction worker |

It is filled in by the `BlockBasedTableBuilder::Rep` constructor
(block_based_table_builder.cc:1398). The LSM-related fields are set only for
flush, compaction and recovery.

### 5.3 `NewBloomFilterPolicy(double bits_per_key, bool ignored)`

filter_policy.h:177, implemented in filter_policy.cc. It returns
`new BloomFilterPolicy(bits_per_key)`. The boolean used to select the old
block-based filter, which has been removed, so it is ignored. Typical use:
`table_options.filter_policy.reset(NewBloomFilterPolicy(10));`

### 5.4 `NewRibbonFilterPolicy(double bloom_equivalent_bits_per_key, int bloom_before_level = 0)`

filter_policy.h:220. Ribbon uses about 30% less space than Bloom for the same
FP rate, at roughly 3-4x the CPU cost to build. `bloom_before_level` uses Bloom
for upper levels and Ribbon below them, with a flush counted as level -1:

- `0` (default): Bloom for flushes only.
- `INT_MAX`: always Bloom.
- `-1`: always Ribbon.

It can be changed at runtime through `SetOptions` with
`table_factory.filter_policy.bloom_before_level`.

---

## 6. Filter policies

File: `table/block_based/filter_policy_internal.h`, implemented in
`table/block_based/filter_policy.cc`.

```
FilterPolicy (public, abstract)
 +-- BuiltinFilterPolicy                       can read every built-in format
      +-- ReadOnlyBuiltinFilterPolicy          builds nothing
      +-- BloomLikeFilterPolicy                bits/key settings + shared builder factories
           +-- BloomFilterPolicy               NewBloomFilterPolicy()
           +-- RibbonFilterPolicy              NewRibbonFilterPolicy()
           +-- test::LegacyBloomFilterPolicy
           +-- test::FastLocalBloomFilterPolicy
           +-- test::Standard128RibbonFilterPolicy
```

### 6.1 `BuiltinFilterPolicy` (filter_policy_internal.h:136)

- `GetFilterBitsReader(contents)` calls the static
  `GetBuiltinFilterBitsReader(contents)` (filter_policy.cc:1649). It reads the
  5-byte metadata trailer and returns the right reader (decision table in
  [9.1](#91-filter-bytes-and-the-5-byte-trailer)).
- `CompatibilityName()` returns `"rocksdb.BuiltinBloomFilter"`.
- `static GetBuilderFromContext(context)` returns
  `table_options.filter_policy->GetBuilderWithContext(context)`, or `nullptr`
  if there is no policy. `CreateFilterBlockBuilder` calls it.

### 6.2 `ReadOnlyBuiltinFilterPolicy` (filter_policy_internal.h:174)

For old OPTIONS files that say only `"rocksdb.BuiltinBloomFilter"`. It reads
existing filters, but `GetBuilderWithContext` returns `nullptr`, so new SSTs
get no filter.

### 6.3 `BloomLikeFilterPolicy` (filter_policy_internal.h:190)

The constructor (filter_policy.cc:1424) first sanitizes `bits_per_key`:

| Input | Value used |
|---|---|
| `< 0.5` | 0 (no filter) |
| `[0.5, 1.0)` | 1.0 |
| `>= 100` or NaN | 100 |

It then derives these members:

| Member | Purpose |
|---|---|
| `millibits_per_key_` | `round(bits * 1000)`. Used by FastLocalBloom, which supports fractional bits per key. |
| `whole_bits_per_key_` | `round(millibits / 1000)`. Used by LegacyBloom. |
| `desired_one_in_fp_rate_` | `1 / CacheLocalFpRate(bits, probes, 512)`. Ribbon targets the same FP rate as Bloom would. |
| `aggregate_rounding_balance_` | Atomic running FP-rate surplus or deficit for `optimize_filters_for_memory` (see [7.3](#73-xxph3filterbitsbuilder-base-of-fastlocalbloom-and-ribbon-builders)) |
| `warned_` | Makes sure the legacy high-bits-per-key warning is logged only once |

Shared builder factories:

- `GetFastLocalBloomBuilderWithContext` (filter_policy.cc:1502). It creates a
  `CacheReservationManagerImpl<kFilterConstruction>` if a block cache exists
  and that role is charged. It passes `&aggregate_rounding_balance_` only when
  `optimize_filters_for_memory` is set, and it passes
  `detect_filter_construct_corruption`.
- `GetLegacyBloomBuilderWithContext` (:1527). It logs a warning when whole
  bits per key is 14 or more, because `format_version >= 5` is much better
  there.
- `GetStandard128RibbonBuilderWithContext` (:1550). Same as FastLocal, plus
  the FP target and a logger.
- `GetId()` / `GetBitsPerKeySuffix()` serialize the policy, for example
  `"bloomfilter:9.9:false"`.

### 6.4 `BloomFilterPolicy::GetBuilderWithContext` (filter_policy.cc:1481)

```
millibits_per_key == 0    -> nullptr (no filter)
format_version < 5        -> LegacyBloomBitsBuilder
otherwise                 -> FastLocalBloomBitsBuilder
```

The default `format_version` is 7 (table.h:739), so the default is FastLocalBloom.

### 6.5 `RibbonFilterPolicy::GetBuilderWithContext` (filter_policy.cc:1840)

It computes a "levelish" value:

- flush: -1
- known level (Level or Universal compaction): that level
- unknown level, FIFO, `kCompactionStyleNone`, or `SstFileWriter`: `INT_MAX - 1`, which is treated as bottommost

If `levelish < bloom_before_level` it builds FastLocalBloom, otherwise Ribbon.

---

## 7. Bits builders and bits readers

All of these are in `table/block_based/filter_policy.cc`. Except for the
interfaces, they live in an anonymous namespace.

### 7.1 `FilterBitsBuilder` interface (filter_policy_internal.h:22)

| Method | Contract |
|---|---|
| `AddKey(key)` | Add one entry. Keys arrive in sorted order and can repeat, so builders drop a hash that equals the previous one. |
| `AddKeyAndAlt(key, alt)` | Add a key and its prefix, removing duplicates across both streams (see 7.3) |
| `EstimateEntriesAdded()` | Unique entries so far. Becomes `num_filter_entries`. Returns 0 only if nothing was added. Thread-safe in the XXH3 builders, because parallel compression reads it. |
| `Finish(buf[, status])` | Build the filter, return its bytes, and hand ownership to `*buf`. Sets `Status::Corruption` if a construction check fails. |
| `MaybePostVerify(filter)` | Optional re-check of every hash against the finished filter |
| `ApproximateNumEntries(bytes)` | How many keys fit in `bytes`. Used to size partitions. |
| `CalculateSpace(n)` | Bytes needed for `n` keys, including metadata |
| `EstimatedFpRate(n, bytes)` | Predicted FP rate |

### 7.2 `FilterBitsReader` / `BuiltinFilterBitsReader` (filter_policy_internal.h:110, :127)

- `MayMatch(key) -> bool`.
- `MayMatch(num_keys, keys, may_match)`: the batch version. The default just
  loops. The built-in readers override it with two passes: first compute every
  hash and prefetch its cache line, then probe. This overlaps the memory
  latency of the keys.
- `HashMayMatch(uint64_t h)`: built-in readers only. Queries by a
  precomputed 64-bit hash. Post-verification uses it.

### 7.3 `XXPH3FilterBitsBuilder`: base of FastLocalBloom and Ribbon builders

filter_policy.cc:63. Its state is `HashEntriesInfo`:
`deque<uint64_t> entries`, `entries_count` (a relaxed atomic),
`cache_res_bucket_handles`, `xor_checksum` and `prev_alt_hash`.

- **`AddKey`**: `h = GetSliceHash64(key)`. Appended unless it equals the last
  hash.
- **`AddKeyAndAlt`**: the alt (prefix) is added unless it equals the previous
  alt, this key's hash, or the previous key. The key is added unless it equals
  the previous key or the previous alt. The alt is pushed first, so
  `entries.back()` is always the most recent key. This handles keys that are
  their own prefix, and reverse comparators.
- **`AddHash`**: updates the optional xor checksum, then `push_back` and
  `entries_count++`. Each time another
  `kUint64tHashEntryCacheResBucketSize` entries (one dummy cache entry divided
  by 8 bytes) start, it reserves one more bucket in the block cache under
  `kFilterConstruction`. The reservation is made halfway through the bucket.
- **`AllocateMaybeRounding`** implements `optimize_filters_for_memory` and
  needs `ROCKSDB_MALLOC_USABLE_SIZE`:
  1. If the running balance is negative (recent filters were more accurate
     than the target), try smaller sizes of 3/4, 13/16, 7/8 and 15/16 of the
     target. Pick the first whose FP rate still fits inside the balance.
  2. Allocate that size plus the block trailer. If `malloc_usable_size`
     reports slack (up to 1/3 extra), grow the filter into it.
  3. Add `(actual_fp - target_fp) * 2^32` to the balance.

  Net effect: filters fill the allocator's size classes, and the average FP
  rate stays on target.
- **`MaybeVerifyHashEntriesChecksum`**: if `detect_filter_construct_corruption`
  is set, recompute the xor of all hashes before using them. On mismatch it
  returns `Corruption` and `Finish` emits an always-true filter.
- **`MaybePostVerify`**: builds a reader over the finished filter and checks
  `HashMayMatch` for every saved hash. Any miss returns
  `Corruption("Corrupted filter content")`. Then it frees the hash entries.

### 7.4 `FastLocalBloomBitsBuilder`: the "format_version=5" Bloom (filter_policy.cc:365)

- **`CalculateSpace(n)`**: `ceil(n * millibits / 8000)` bytes, rounded up to a
  multiple of 64 and capped at `0xffffffc0`, plus 5 metadata bytes.
  Example: 1,000,000 keys at 10 bits/key -> 1,250,048 + 5 = 1,250,053 bytes.
- **`Finish`**:
  1. With 0 entries, return an empty `Slice`, which a reader treats as
     AlwaysFalse.
  2. Size = `CalculateSpace`, possibly adjusted by `AllocateMaybeRounding`.
     Reserve cache space for the final buffer.
  3. `num_probes = FastLocalBloomImpl::ChooseNumProbes(millibits)`. With
     `optimize_filters_for_memory`, the millibits are the actual value after
     rounding.
  4. `AddAllEntries` inserts with software pipelining over an 8-entry ring:
     while entry `i` is inserted, `PrepareHash` prefetches the cache line for
     entry `i + 8`.
  5. Verify the checksum, then write the metadata trailer
     `[0xFF][0x00][num_probes][0x00][0x00]`.
  6. Keep the hashes only if post-verification is enabled.
- **`ApproximateNumEntries(bytes)`**:
  `(RoundDownUsableSpace(bytes) - 5) * 8000 / millibits`.
- **`EstimatedFpRate`**: `FastLocalBloomImpl::EstimatedFpRate` with a 64-bit hash.

### 7.5 `FastLocalBloomBitsReader` (filter_policy.cc:588)

It holds `data_`, `num_probes_` and `len_bytes_`. `MayMatch` computes
`h = GetSliceHash64(key)`, picks the cache line with
`FastRange32(lower32(h), len / 64)`, then probes using `upper32(h)`. The batch
`MayMatch` hashes and prefetches every key in a first loop and probes in a
second loop.

### 7.6 `LegacyBloomBitsBuilder` / `LegacyBloomBitsReader` (filter_policy.cc:1079, :1309)

Used when `format_version < 5`.

- **Hash**: `BloomHash`, which is 32-bit with seed `0xbc9f1d34`, kept in a
  `vector<uint32_t>`.
- **num_probes**: `floor(bits_per_key * 0.69)`, clamped to [1, 30].
- **Size**: `total_bits = n * bits_per_key`, capped at `0xffff0000`, rounded up
  to whole cache lines. The number of lines is forced to be odd, so more hash
  bits influence which line is chosen.
- **Probing**: `LegacyLocalityBloomImpl<false>`. Line = `h % num_lines`,
  bit position = `h & 511`, then `h += rotr(h, 17)`.
- **Trailer**: `[num_probes][num_lines as fixed32]`.
- With more than 3M keys it logs a warning if the 32-bit hash pushes the FP
  rate to 1.5x or more of normal.
- The reader handles a cache line size that differs from the local
  `CACHE_LINE_SIZE`, derived from `len / num_lines`, so files built on another
  platform still work.
- `HashMayMatch` returns false because there is no 64-bit hash. This is why
  post-verification does not apply to legacy filters.

### 7.7 `Standard128RibbonBitsBuilder` / `Standard128RibbonBitsReader` (filter_policy.cc:658, :1025)

Ribbon is not a Bloom filter (it solves a linear system), but it is in the same
family and contains a FastLocalBloom builder (`bloom_fallback_`). It falls back
to Bloom when:

- there are too many keys: `Finish` checks `entries.size() > kMaxRibbonEntries`
  (950,000,000), and `CalculateSpaceAndSlots` checks the same bound;
- the filter is small enough that Bloom would be no larger:
  `CalculateSpaceAndSlots` sets `num_slots = 0` when `num_slots < 1024` and
  `bloom_fallback_.CalculateSpace(num_entries)` is less than the Ribbon target;
- reserving cache space for the banding memory fails with `MemoryLimit`;
- 256 seeds all fail to solve (`ResetAndFindSeedToSolve` with seed mask 255).

Zero entries is handled before any of this, by returning `FinishAlwaysFalse`.

Trailer: `[0xFE][seed][num_blocks as 3 bytes]`.

### 7.8 `AlwaysTrueFilter` / `AlwaysFalseFilter` (filter_policy.cc:1363, :1371)

Trivial readers.

- **AlwaysFalse**: a filter with no keys, meaning any filter of 5 bytes or
  fewer.
- **AlwaysTrue**: an unknown, reserved or invalid format, or a construction
  that detected corruption. "Maybe" is the safe answer.

`FinishAlwaysTrue` emits 6 zero bytes. The probe-count byte is 0, so the bytes
decode as AlwaysTrue.

---

## 8. Hashing and probing algorithms

### 8.1 Hash functions (`util/hash.h`)

| Function | Bits | Used by |
|---|---|---|
| `GetSliceHash64(key)` = `Hash64` (XXH3 preview) | 64 | FastLocalBloom, Ribbon |
| `BloomHash(key)` = `Hash(data, n, 0xbc9f1d34)` | 32 | LegacyBloom, DynamicBloom (memtable) |
| `GetSliceHash(key)` | 32 | PlainTable bloom |

Why 64 bits matters: with a 32-bit hash, two different keys share a hash with
probability 2^-32. With tens of millions of keys in one filter, that collision
FP rate dominates (`BloomMath::FingerprintFpRate`). At 10 bits/key the
crossover is around 40M keys per filter.

### 8.2 `BloomMath` (util/bloom_impl.h:26)

| Function | Formula |
|---|---|
| `StandardFpRate(b, k)` | `(1 - e^(-k/b))^k` |
| `CacheLocalFpRate(b, k, line_bits)` | Average of `StandardFpRate` at the mean number of keys per line plus and minus one standard deviation (Poisson) |
| `FingerprintFpRate(n, bits)` | Chance that a new key's hash equals one of `n` stored hashes |
| `IndependentProbabilitySum(a, b)` | `a + b - a*b` |

These are used for estimates, warnings, the Ribbon FP target and
`optimize_filters_for_memory`. They never affect correctness.

### 8.3 `FastLocalBloomImpl` (util/bloom_impl.h:144)

```
  64-bit hash h = XXH3(key)
    +--------------------+--------------------+
    |   upper 32 = h2    |   lower 32 = h1    |
    +--------------------+--------------------+
              |                    |
              |                    v
              |     line = FastRange32(h1, num_lines)   (= (h1 * num_lines) >> 32)
              |                    |
              v                    v
   probe i (i = 0..k-1):  bitpos = top 9 bits of (h2 * 0x9e3779b9^i)
                          set / test bit `bitpos` inside that 512-bit line

   filter bytes: [line 0: 64 B][line 1: 64 B] ... [line N-1: 64 B][5-byte trailer]
```

| Function | Role |
|---|---|
| `AddHash` / `AddHashPrepared` | Set `k` bits in the chosen line |
| `PrepareHash` | Compute the line offset and `PREFETCH` both ends of the line (for pipelining) |
| `HashMayMatch` / `HashMayMatchPrepared` | Scalar loop. With AVX2 it tests 8 probes at once: multiply 8 copies of `h` by powers of the golden ratio, permute and blend the two 256-bit halves of the line, then `testc` against a mask. Past 8 probes it loops again with `h *= 0xab25f4c1` (golden ratio to the 8th power). |
| `EstimatedFpRate` | `CacheLocalFpRate(512)` combined with `FingerprintFpRate(64)` |
| `ChooseNumProbes(millibits)` | Empirically tuned table, below |

The thresholds are compared in millibits, so they are exact integers; the
bits/key column is just that value divided by 1000. Note 14001, 22001 and 25501,
which are deliberately a hair above the round number.

| `millibits_per_key` (up to) | Bits per key | Probes |
|---|---|---|
| 2080 | 2.080 | 1 |
| 3580 | 3.580 | 2 |
| 5100 | 5.100 | 3 |
| 6640 | 6.640 | 4 |
| 8300 | 8.300 | 5 |
| 10070 | 10.070 | 6 |
| 11720 | 11.720 | 7 |
| 14001 | 14.001 | 8 |
| 16050 | 16.050 | 9 |
| 18300 | 18.300 | 10 |
| 22001 | 22.001 | 11 |
| 25501 | 25.501 | 12 |
| 50000 | 50.000 | `(millibits - 1) / 2000 - 1` |
| above 50000 | above 50 | 24 |

At 10 bits/key this gives 6 probes and about a 0.957% FP rate. The theoretical
best for a 512-bit cache-local Bloom is 0.9535%; LegacyBloom gets 1.138%.

### 8.4 `LegacyNoLocalityBloomImpl` (util/bloom_impl.h:354)

Classic double hashing over the whole bit array (`h % total_bits`,
`h += rotr(h, 17)`). It is slow, with one cache miss per probe. It is used only
by the PlainTable bloom when `bloom_locality == 0`, and its `ChooseNumProbes`
is reused by LegacyBloom.

### 8.5 `LegacyLocalityBloomImpl<ExtraRotates>` (util/bloom_impl.h:404)

Double hashing confined to one cache line.

- `<false>` is the legacy SST filter format. It has a known accuracy flaw: a
  1/512 chance that the increment is zero.
- `<true>` is used by PlainTable and adds an extra rotation.

The cache line size is a parameter so files stay portable across platforms.

### 8.6 DynamicBloom probing

See [section 16](#16-memtable-bloom-filter-dynamicbloom).

---

## 9. On-disk format

### 9.1 Filter bytes and the 5-byte trailer

Every built-in filter is raw bits followed by a 5-byte metadata trailer. The
reader chooses an implementation from the signed byte at offset `len` (the
first trailer byte).

```
 LegacyBloom (fv < 5)          FastLocalBloom (fv >= 5)        Standard128Ribbon
 +---------------------+       +----------------------+        +----------------------+
 | bit array           |       | bit array (N * 64 B) |        | ribbon solution      |
 +---------------------+       +----------------------+        +----------------------+
 | num_probes (1..127) |       | 0xFF  (-1)           |        | 0xFE  (-2)           |
 | num_lines (fixed32) |       | 0x00  sub-impl       |        | seed (1 byte)        |
 |                     |       | block+probes byte    |        | num_blocks (3 bytes) |
 |                     |       | 0x00 0x00 reserved   |        |                      |
 +---------------------+       +----------------------+        +----------------------+
```

In the FastLocalBloom block+probes byte, the top 3 bits hold
`log2(block bytes) - 6`, and only 0 (64-byte blocks) is supported. The bottom
5 bits hold `num_probes` (1..30).

How `GetBuiltinFilterBitsReader` picks a reader (filter_policy.cc:1649):

| Condition | Reader |
|---|---|
| size <= 5 | `AlwaysFalseFilter` (no keys) |
| `byte[len] == 0` | `AlwaysTrueFilter` |
| `byte[len] == -1` | `GetBloomBitsReader`: `FastLocalBloomBitsReader` if sub-impl is 0, the block is 64 B, probes are in 1..30 and the reserved bytes are 0. Otherwise `AlwaysTrue`. |
| `byte[len] == -2` | `GetRibbonBitsReader`: `Standard128RibbonBitsReader` if `num_blocks >= 2`, otherwise `AlwaysTrue` |
| other negative values | `AlwaysTrue` (reserved) |
| 1..127 | `LegacyBloomBitsReader` if `num_lines` is consistent with `len` (power-of-two line size), otherwise `AlwaysTrue` |

Design principle: an unrecognized, reserved or inconsistent filter degrades to
"always maybe" rather than producing a wrong answer. The single branch that
departs from this is the `size <= 5` row above, which yields `AlwaysFalse`; see
[21.1](#21-correctness-rules-and-gotchas) for why that is unreachable from a
well-formed file.

### 9.2 Where filter blocks sit in the SST

`WriteFilterBlock` writes the first meta block in `Finish`
(block_based_table_builder.cc:3448):

```
 [data block 0] ... [data block N]
 [filter partition 0] ... [filter partition P-1]   <- partitioned filters only
 [filter block]  or  [filter partition index]
 [index block(s)]
 [compression dict] [range-del block] [properties block]
 [metaindex block]   <- e.g. "fullfilter.rocksdb.BuiltinBloomFilter" -> BlockHandle
 [footer]
```

- Filter blocks are written uncompressed (`kNoCompression`), with the normal
  block trailer (type byte plus checksum).
- The metaindex key is a prefix followed by `CompatibilityName()`:

| Prefix | Reader filter type | Block type |
|---|---|---|
| `fullfilter.` | `Rep::FilterType::kFullFilter` | `BlockType::kFilter` |
| `partitionedfilter.` | `kPartitionedFilter` | The handle points to the partition index (`kFilterPartitionIndex`). Partitions are `kFilter`. |
| `filter.` | Obsolete block-based filter, removed in 7.0 | Recognized only to log "Detected obsolete filter type"; treated as no filter |

### 9.3 Partitioned filter layout

```
  filter partition index (an ordinary index Block, binary-searched)
  +---------------------------------------+-------------------------------+
  | separator key                         | BlockHandle (delta-encoded)   |
  +---------------------------------------+-------------------------------+
  | >= every key in partition 0           | -> partition 0                |
  | >= every key in partition 1           | -> partition 1                |
  | ...                                   |                               |
  +---------------------------------------+-------------------------------+
  Separators are user keys, unless the index uses key+seqno separators.
  Each partition is a complete filter (FastLocalBloom / Legacy / Ribbon)
  with its own 5-byte trailer.
```

### 9.4 Table properties written

| Property | Meaning |
|---|---|
| `filter_policy_name` | `Name()` of the policy (for example `bloomfilter`), or empty |
| `filter_size` | Total bytes of filter blocks, including partitions and the partition index |
| `num_filter_entries` | `EstimateEntriesAdded()`: unique keys and prefixes |
| `prefix_extractor_name` | The extractor in effect when the file was built |
| `rocksdb.block.based.table.whole.key.filtering` | `"1"` / `"0"` |
| `rocksdb.block.based.table.prefix.filtering` | `"1"` / `"0"` |
| `rocksdb.block.based.table.decoupled.partitioned.filters` | Present when `decouple_partitioned_filters` was used |

---

## 10. Filter blocks: full and partitioned

### 10.1 `FilterBlockBuilder` (filter_block.h:46)

Call order: `(Add | AddWithPrevKey)* [PrevKeyBeforeFinish] Finish+`

| Method | Purpose |
|---|---|
| `Add(key_without_ts)` | Feed one user key with the timestamp stripped |
| `AddWithPrevKey(key, prev)` | Same, but the caller passes the previous key so the partitioned builder does not have to copy it |
| `IsEmpty()`, `EstimateEntriesAdded()` | |
| `CurrentFilterSizeEstimate()` | Cached estimate, used to estimate the SST tail size when deciding where to cut files |
| `OnDataBlockFinalized(n)` | Refresh the size estimate after each data block |
| `PrevKeyBeforeFinish(prev)` | Lets the partitioned builder cut its last partition |
| `Finish(last_handle, &filter, &owner)` | `OK` means done. `Incomplete` means more partitions follow: the caller writes the returned block and passes its handle to the next call. |
| `ResetFilterBitsBuilder()` | Release memory and cache reservations early |
| `MaybePostVerifyFilter(content)` | Forwarded to the bits builder |

### 10.2 `FullFilterBlockBuilder` (full_filter_block.cc)

`Add` decides what enters the filter:

| `prefix_extractor` set and key `InDomain` | `whole_key_filtering` | Entries added |
|---|---|---|
| yes | true | `AddKeyAndAlt(key, prefix)` |
| yes | false | `AddKey(prefix)` |
| no | true | `AddKey(key)` |
| no | false | nothing |

`Finish` calls `bits_builder->Finish`. The size estimate is
`CalculateSpace(entries)` plus headroom of twice the average per data block.

**A full filter is always exactly one SST block.** `Finish` returns `OK` (or
`Corruption`), never `Incomplete`, so `WriteFilterBlock`'s
`while (s.IsIncomplete())` loop runs a single iteration: one
`WriteMaybeCompressedBlock` call, one metaindex entry under
`fullfilter.<CompatibilityName>`, one `BlockHandle`. On the read side it becomes
one `ParsedFullFilterBlock` holding one `FilterBitsReader`. Contrast
`PartitionedFilterBlockBuilder::Finish` below, which returns `Incomplete` once
per partition and so drives that loop round repeatedly.

There is no size cap on that one block. It grows as
`num_entries * bits_per_key / 8`, rounded up to 64 and plus the 5-byte trailer,
until the implementation limit of `0xffffffc0` bytes. A million keys at 10
bits/key is a single ~1.25 MB block: one I/O to fetch, one block-cache entry,
evicted as a unit. That granularity is the reason `partition_filters` exists
([10.3](#103-partitionedfilterblockbuilder-partitioned_filter_blockcc)) -- it
cuts the filter at `metadata_block_size` so a point lookup faults in one small
partition instead of the whole filter. The 64-byte filter blocks inside the
buffer do not help here; see the "block" disambiguation in
[2.4](#24-vocabulary-used-in-the-code).

### 10.3 `PartitionedFilterBlockBuilder` (partitioned_filter_block.cc)

It extends `FullFilterBlockBuilder` and reuses the one bits builder for every
partition.

- **Constructor**: `keys_per_partition_ = ApproximateNumEntries(partition_size)`,
  where `partition_size = metadata_block_size * (100 - block_size_deviation) / 100`
  (block_based_table_builder.cc:169). With the defaults (4096 and 10%) that is
  3687 bytes, or about 2918 keys at 10 bits/key. It is reduced by 1 when a
  prefix extractor is set, to leave room for the extra boundary prefix.
- **`DecideCutAFilterBlock`**:
  - Decoupled (the default): cut when entries reach `keys_per_partition_`.
  - Coupled (legacy): ask the `PartitionedIndexBuilder` for a cut
    (`RequestPartitionCut`) and cut only when `ShouldCutFilterBlock()` agrees.
    Filter partitions then line up with index partitions. Coupled mode forces
    `parallel_threads = 1`.
- **`CutAFilterBlock(next_key, next_prefix, prev_key)`**:
  1. If there is a next prefix, add it to the closing partition. Without this,
     a prefix `Seek` that lands in this partition could miss a key whose
     prefix starts in the next one.
  2. Finish the bits builder (with post-verify) and push
     `{separator ikey, bytes}` onto the `filters_` deque. The separator is
     `prev_key` with a (seq 0, `kTypeDeletion`) footer in decoupled mode
     (min timestamp added if user-defined timestamps are on), or the index
     builder's partition key in coupled mode.
  3. If there is a next key, add `prefix(prev_key)` to the new partition, for
     `SeekForPrev`.
- **`Finish`** works as a state machine:
  - The first call cuts the last partition, then returns `filters_.front()`
    with `Incomplete`.
  - Each later call receives the handle of the partition just written, adds
    `separator -> handle` to the index builders (one on internal keys, one on
    user keys), pops the entry and returns the next partition.
  - When no partitions remain, it returns the finished partition index block
    with `OK`.

  Construction errors are recorded in
  `partitioned_filters_construction_status_` and returned.
- **Size estimate**: bytes of completed partitions (atomic), plus 2 bytes per
  key for the active partition, plus the index estimate, plus headroom.

### 10.4 `FilterBlockReader` (filter_block.h:136)

| Method | Purpose |
|---|---|
| `KeyMayMatch(key, ikey_ptr, get_context, lookup_ctx, ro)` | Whole-key query. The internal key is used by the partitioned reader to find the partition. |
| `KeysMayMatch(range, ...)` | Batch query. Calls `range->SkipKey` for each negative. |
| `PrefixMayMatch` / `PrefixesMayMatch` | Prefix queries |
| `RangeMayExist(upper_bound, user_key, extractor, cmp, ikey, &checked, need_ub_check, ...)` | Check for an iterator `Seek` |
| `CacheDependencies(ro, pin, prefetch_buf)` | Load and optionally pin partitions when the table is opened |
| `EraseFromCacheBeforeDestruction(aggr)` | Remove cached blocks when the file is deleted |
| `ApproximateMemoryUsage()` | |

### 10.5 `FilterBlockReaderCommon<TBlocklike>` (filter_block_reader_common.{h,cc})

`TBlocklike` is `ParsedFullFilterBlock` for full filters, or
`Block_kFilterPartitionIndex` for the top level of a partitioned filter.

| Member | What it does |
|---|---|
| `filter_block_` | A `CachableEntry`: empty, owned by the reader, or a pinned cache handle |
| `GetOrReadFilterBlock` | Uses `filter_block_` if it is set, with no cache lookup. Otherwise calls `ReadFilterBlock` -> `BlockBasedTable::RetrieveBlock(filter_handle, use_cache = cache_index_and_filter_blocks)`. Timed by `read_filter_block_nanos`. |
| `whole_key_filtering()`, `cache_filter_blocks()` | Read from the table rep |
| `table_prefix_extractor()` | The table's prefix extractor, or `nullptr` if the table property says no prefix filtering |
| `RangeMayExist` | If there is no extractor or the key is not in its domain, returns true and marks the filter unchecked. If `need_upper_bound_check` is set and `IsFilterCompatible` fails, also returns true unchecked. Otherwise calls `PrefixMayMatch(prefix)`. |
| `IsFilterCompatible(upper_bound, prefix, cmp)` | True if the upper bound is in domain and either shares the prefix, or is the same-length immediate successor of the prefix under a full-length extractor. Either way every key in `[seek, upper_bound)` has the same prefix. |
| `EraseFromCacheBeforeDestruction` | Erases the filter block when `uncache_aggressiveness > 0` |

### 10.6 `ParsedFullFilterBlock` (parsed_full_filter_block.{h,cc})

The object that sits in the block cache for a filter block: `BlockContents`
plus a `unique_ptr<FilterBitsReader>` created once through
`filter_policy->GetFilterBitsReader(contents)`. The trailer is therefore
decoded once per cache insertion, not on every query. Cache role
`kFilterBlock`, block type `kFilter`. Created at block_cache.cc:59.

### 10.7 `FullFilterBlockReader` (full_filter_block.cc)

- **`Create(table, ro, prefetch_buffer, use_cache, prefetch, pin, ctx)`**: if
  `prefetch` or `!use_cache`, reads the block immediately. If the block is
  cached but not pinned, the handle is released (the block stays in the
  cache).
- **`KeyMayMatch`**: returns true if `!whole_key_filtering`, otherwise calls
  `MayMatch`.
- **`MayMatch(entry)`**: `GetOrReadFilterBlock` (an error returns true), then
  `filter_bits_reader->MayMatch`. Updates the perf counters
  `bloom_sst_hit_count` / `bloom_sst_miss_count`.
- **`MayMatch(range, extractor)`** (batch): gathers up to `MAX_BATCH_SIZE`
  keys or prefixes (keys outside the extractor's domain are left out of the
  filter pass), makes one batched `FilterBitsReader::MayMatch` call, and calls
  `SkipKey` on each negative.

### 10.8 `PartitionedFilterBlockReader` (partitioned_filter_block.cc)

- **`Create`**: same pattern as the full reader, but the top-level block is a
  `Block_kFilterPartitionIndex` (cache role `kFilterMetaBlock`).
- **`KeyMayMatch` / `PrefixMayMatch` -> `MayMatch(slice, ikey, ..., member fn pointer)`**:
  1. Get the top-level index. An error returns true, and so does an empty
     index.
  2. `GetFilterPartitionHandle`: build an `IndexBlockIter` and
     `Seek(ikey)`. If that runs past the end, use `SeekToLast`, because the
     prefix may still be in the last partition.
  3. `GetFilterPartitionBlock`: use `filter_map_` (pinned partitions) if the
     partition is there, otherwise `RetrieveBlock` with `use_cache = true`.
     Partitions always go through the block cache, even when
     `cache_index_and_filter_blocks = false`.
  4. Wrap the partition in a temporary `FullFilterBlockReader` and call the
     same member function on it.
- **Batch `MayMatch`**: keys are sorted, so consecutive keys that fall in the
  same partition form one sub-range. Each group costs one partition fetch and
  one batched probe (`MayMatchPartition`).
- **`CacheDependencies`**: reads the top-level index, prefetches the byte range
  from the first partition to the last in a single I/O, and loads each
  partition into the cache. With `pin`, it keeps the handles in `filter_map_`.
- **`EraseFromCacheBeforeDestruction`**: erases partitions, either from
  `filter_map_` or by walking the index, guided by
  `UncacheAggressivenessAdvisor`.

---

## 11. Write path: building filters

### 11.1 When filters are built

`BlockBasedTableBuilder` builds a filter for the SSTs it writes, whatever the
reason for writing them:

- flush (`FlushJob` -> `BuildTable`)
- compaction (`CompactionJob::OpenCompactionOutputFile`)
- recovery and repair
- `SstFileWriter` (external files)

Three conditions suppress the filter, all checked in the `Rep` constructor
([11.2](#112-sequence)): `optimize_filters_for_hits` with `is_bottommost`, a
null `filter_policy`, and a policy whose `GetBuilderWithContext` returns
`nullptr` (which is what `bits_per_key < 0.5` and
`ReadOnlyBuiltinFilterPolicy` do).

Keys are hashed as they are added. The bits are set during `Finish`.

### 11.2 Sequence

```
CompactionJob / FlushJob / SstFileWriter
  |
  v
BlockBasedTableBuilder::Rep::Rep()                       block_based_table_builder.cc:1398
  - fill FilterBuildingContext (level, reason, is_bottommost, ...)
  - optimize_filters_for_hits && is_bottommost  -> no filter                    :1650
  - filter_policy == nullptr                    -> no filter
  - otherwise CreateFilterBlockBuilder()                                         :148
       BuiltinFilterPolicy::GetBuilderFromContext(ctx)
         -> policy->GetBuilderWithContext(ctx)  -> FilterBitsBuilder*
       partition_filters ? PartitionedFilterBlockBuilder : FullFilterBlockBuilder
  |
  v
BlockBasedTableBuilder::Add(ikey, value)               once per entry           :1939
  - only value types (Put, Delete, Merge, ...) reach the filter; range
    tombstones go to the range-del block and are NOT in the filter
  - filter_builder->AddWithPrevKey(user_key_without_ts, prev_user_key)
  - in buffered mode (building a compression dictionary) keys are added later,
    when the buffered data blocks are replayed                                   :3365
  - after each data block: filter_builder->OnDataBlockFinalized(n)              :2094
  |
  v
BlockBasedTableBuilder::Finish()                                                 :3448
  WriteFilterBlock()                                                             :2891
    PrevKeyBeforeFinish(last key)
    props.num_filter_entries += EstimateEntriesAdded()
    do {
      s = filter_builder->Finish(prev_handle, &content, &owner)
      WriteMaybeCompressedBlock(content, kNoCompression, &handle,
                                kFilter or kFilterPartitionIndex)
        -> for kFilter: MaybePostVerifyFilter()   (corruption check)            :2589
    } while (s.IsIncomplete())
    ResetFilterBitsBuilder()
    metaindex: "fullfilter." or "partitionedfilter." + CompatibilityName() -> handle
  props.filter_policy_name = policy->Name()
```

Notes:

- Multiple versions of one user key hash to the same value and are collapsed
  by the adjacent-duplicate check.
- If filter construction reports `Corruption`, the whole table build fails
  (`rep_->SetStatus`).
- `EstimatedTailSize()` (:3540) includes `CurrentFilterSizeEstimate()`, so
  compaction accounts for filter size when it decides where to cut output
  files.
- Coupled partitioned filters disable parallel compression (:1544).

### 11.3 Where `is_bottommost` comes from

Compaction passes `Compaction::bottommost_level()` (compaction_job.cc:2613),
and flush passes `false` (flush_job.cc:1039). The flag drives
`optimize_filters_for_hits`, and custom policies can read it.

---

## 12. Read path: opening a table

`BlockBasedTable::Open` (block_based_table_reader.cc:795) touches filters at
three points, in this order:

1. `ReadPropertiesBlock`, called at :918 (body at :1150). At :1197 it applies
   `rep->whole_key_filtering &= property` and
   `rep->prefix_filtering &= property`, so the reader follows how the file was
   built. `IsFeatureSupported` treats a missing property as true, for files
   from before the property existed.
2. Prefix extractor setup at :986. This must come after step 1, because it
   reads `rep->table_properties->prefix_extractor_name`.
3. `PrefetchIndexAndFilterBlocks`, called at :1029 (body at :1684).

Details:

- `Rep::filter_policy` is `table_options.filter_policy`, or `nullptr` if the
  table was opened with `skip_filters` (block_based_table_reader.h:810).
  Without a policy the filter is never looked up.
- **Finding the filter**: the metaindex is searched for `fullfilter.<compat>`,
  then `partitionedfilter.<compat>`, then `filter.<compat>`.
- **`table_prefix_extractor`**: if the current CF extractor matches the table
  property `prefix_extractor_name`, it is reused. Otherwise the extractor named
  in the table is recreated with `SliceTransform::CreateFromString`. Either
  way, the reader knows which prefix function built the filter.
- **Pinning and prefetching** (:1727 onward):

```
use_cache       = cache_index_and_filter_blocks && !avoid_shared_metadata_cache
pin_filter      = partitioned ? pin_top_level_index : pin_unpartitioned
prefetch_filter = prefetch_all || pin_filter
filter = CreateFilterBlockReader(use_cache, prefetch_filter, pin_filter)    :2123
if (prefetch_all || pin_partition) filter->CacheDependencies(pin_partition)
```

  - `pin_top_level_index` comes from
    `MetadataCacheOptions::top_level_index_pinning`. Its fallback is
    `pin_top_level_index_and_filter`, which maps to `kAll`.
  - `pin_unpartitioned` and `pin_partition` come from `unpartitioned_pinning`
    and `partition_pinning`. Their fallback is
    `pin_l0_filter_and_index_blocks_in_cache`, which maps to
    `kFlushedAndSimilar` (L0 files no larger than
    `max_file_size_for_l0_meta_pin`).

Where the filter ends up:

| Configuration | Where the filter lives |
|---|---|
| `cache_index_and_filter_blocks = false` | Read at open and owned by the table reader (heap) for its lifetime. Partitions still go through the block cache. |
| `= true`, not pinned | In the block cache, looked up on every query, can be evicted |
| `= true`, pinned | In the block cache with the handle held by the reader, so it is not evicted while the table is open |

---

## 13. Read path: Get

```
DBImpl::GetImpl
 |-- MemTable::Get   (mutable memtable, then immutables)   DynamicBloom check, section 16
 |-- Version::Get                                          db/version_set_sync_and_async.h:14
       FilePicker picks files by key range (L0: every overlapping file, Ln: one file)
       for each candidate file:
         skip = IsFilterSkipped(level, is_last_file_in_level)       version_set.cc:3165
         TableCache::Get(..., skip_filters = skip)                   table_cache_sync_and_async.h:14
           row cache lookup (if row_cache is set)
           FindTable -> open the reader if needed (section 12)
           range tombstones: MaxCoveringTombstoneSeqnum (independent of the filter)
           BlockBasedTable::Get(..., skip_filters)                   ..._reader_sync_and_async.h:516
             filter = skip_filters ? nullptr : rep_->filter
             FullFilterKeyMayMatch(filter, ikey, prefix_extractor)    block_based_table_reader.cc:2784
               if whole_key_filtering:
                 filter->KeyMayMatch(user_key_without_ts)
               else if extractor unchanged and key InDomain:
                 filter->PrefixMayMatch(prefix)
               else:
                 true
               record BLOOM_FILTER_USEFUL / FULL_POSITIVE / PREFIX_* stats
             if !may_match: return  (no index block, no data block)
             else: index seek and data block reads; if the key is found
                   and a filter was used: FULL_TRUE_POSITIVE / PREFIX_TRUE_POSITIVE  (:725)
```

**`IsFilterSkipped`** (version_set.cc:3165):

```
optimize_filters_for_hits
  && (level > 0 || is_file_last_in_level)
  && level == num_non_empty_levels - 1
```

The reasoning: a lookup that reaches the last non-empty level has already
missed in every level above. In a workload where most lookups hit, the key is
probably in this level, so checking the filter is wasted work. This pairs with
the write-side rule that bottommost files get no filter at all
([11.2](#112-sequence)).

**`DB::KeyMayExist`** runs `GetImpl` with `read_tier = kBlockCacheTier`. A
filter is consulted only if it is already in memory or in the block cache.
When loading it would need I/O, `RetrieveBlock` returns `Incomplete`,
`GetOrReadFilterBlock` fails, and `MayMatch` returns true ("may exist").

---

## 14. Read path: MultiGet

**Synchronous path** (`Version::MultiGet` -> `MultiGetFromSST` ->
`TableCache::MultiGet` -> `BlockBasedTable::MultiGet`):

- `BlockBasedTable::MultiGet` first runs `FullFilterKeysMayMatch` on the
  file's key range (..._reader_sync_and_async.h:1049). Negatives are removed
  from the `MultiGetRange` before any index or data I/O.
- Full filter: a single batched `FilterBitsReader::MayMatch` (hash and
  prefetch everything, then probe).
- Partitioned filter: keys are grouped by partition
  ([10.8](#108-partitionedfilterblockreader-partitioned_filter_blockcc)).

**Asynchronous path** (`Version::MultiGetAsync` -> `ProcessBatch`,
version_set.cc:2934):

- For each file where `IsFilterSkipped` is false, `TableCache::MultiGetFilter`
  runs first (table_cache.cc:620). It opens the table, calls
  `BlockBasedTable::MultiGetFilter` (block_based_table_reader.cc:2948), and
  updates the range-tombstone sequence numbers. If every key is filtered out,
  no read coroutine is scheduled for that file. The later `MultiGet` call
  passes `skip_filters = true` so the filter is not checked twice.
- This pass is skipped when the row cache is enabled, because filtering has to
  happen after the row cache lookup.

**Memtable `MultiGet`**: a batched `DynamicBloom::MayContain`, disabled when
the memtable contains range tombstones.

---

## 15. Read path: iterators and prefix seek

Prefix filters let a `Seek` skip an entire SST when no key with the seek key's
prefix exists in it.

In `BlockBasedTable::NewIterator` (block_based_table_reader.cc:2696):

```
check_filter = !skip_filters
            && (!total_order_seek || auto_prefix_mode || prefix_same_as_start)
            && prefix_extractor != nullptr

need_upper_bound_check = auto_prefix_mode || PrefixExtractorChanged(prefix_extractor)
```

`Seek(target)` and `SeekForPrev(target)` call `CheckPrefixMayMatch`
(block_based_table_iterator.h:474):

1. A backward seek with `need_upper_bound_check` set skips the filter.
2. `BlockBasedTable::PrefixRangeMayMatch` (block_based_table_reader.cc:2636)
   uses the table's prefix extractor. It falls back to the current one only if
   the table has none and no upper-bound check is needed. If the key is not in
   the extractor's domain, the answer is true. Otherwise it calls
   `filter->RangeMayExist(...)` ([10.5](#105-filterblockreadercommontblocklike-filter_block_reader_commonhcc)).
3. On a negative, `ResetDataIter()` runs and the iterator is invalid for this
   file. Ticker: `LAST_LEVEL_SEEK_FILTERED` or `NON_LAST_LEVEL_SEEK_FILTERED`.
4. On a positive: `*_SEEK_FILTER_MATCH`, and later
   `*_SEEK_DATA_USEFUL_FILTER_MATCH` if a value is read.

`auto_prefix_mode` keeps total-order semantics, but it uses the prefix filter
whenever `[seek_key, iterate_upper_bound)` provably stays inside one prefix
(`IsFilterCompatible`).

The **memtable iterator** uses `DynamicBloom` only for prefix seek, and only
when the memtable's prefix extractor is the same object as the CF's and either
`prefix_same_as_start` is set or both `total_order_seek` and
`auto_prefix_mode` are off (memtable.cc:521, filter assigned at :530). On `Seek` / `SeekForPrev`, if
`!MayContain(prefix)` the iterator becomes invalid.

The partition-boundary prefixes ([10.3](#103-partitionedfilterblockbuilder-partitioned_filter_blockcc))
make sure a prefix `Seek` that lands in the partition just before the first
key with that prefix still gets a positive.

Compaction input iterators opened with ephemeral readers skip filters
(`GetCompactionTableCacheOpenOptions`, version_set.cc:1139).

---

## 16. Memtable Bloom filter (DynamicBloom)

**Creation** (memtable.cc:244): when
`(prefix_extractor || memtable_whole_key_filtering) && memtable_prefix_bloom_bits > 0`,

```
bloom_filter_.reset(new DynamicBloom(&arena_, bits, /*num_probes*/ 6,
                                     memtable_huge_page_size, logger));
```

Here `bits = write_buffer_size * memtable_prefix_bloom_size_ratio * 8`
(memtable.cc:117), and the ratio is clamped to [0, 0.25]
(column_family.cc:299). Example: a 64 MB memtable with ratio 0.1 gets a 6.4 MB
filter.

**Insertion** (`MemTable::Add`, memtable.cc:1199): adds the prefix (if there is
an extractor and the key is in its domain) and the whole key (if
`memtable_whole_key_filtering`), both into the same filter. Concurrent
memtable writes use `AddConcurrently`, which does a relaxed `fetch_or` only
when some of the bits are missing.

**Queries**:

| Operation | What is checked |
|---|---|
| `Get` (memtable.cc:1607) | The whole key if `memtable_whole_key_filtering`, otherwise the prefix if the key is in domain. Range tombstones are evaluated before the bloom check, so results stay correct. |
| `MultiGet` (:1720) | Batched. Whole keys if there is no extractor or `memtable_whole_key_filtering` is set, otherwise prefixes. Skipped entirely if the memtable has range deletions (unless `ignore_range_deletions`). |
| Iterator `Seek` / `SeekForPrev` (:573) | Prefix only, in prefix mode only |

**Internals** (util/dynamic_bloom.h:34):

```
  h32 = BloomHash(key)
  a   = FastRange32(h32, kLen)                    starting 64-bit word
  h   = 0x9e3779b97f4a7c13 * h32                  64-bit remix
  for i in 0 .. num_probes/2 - 1:
      word = data[a ^ i]                          neighbouring words in an aligned block
      mask = bit(h & 63) | bit((h >> 6) & 63)     two probes per word
      h    = rotr(h, 12)
```

- `num_probes` must be even and at most 10. The memtable uses 6, which means
  3 words inside an aligned 4-word (32-byte) block.
- The data is `RelaxedAtomic<uint64_t>`, so readers take no locks. Readers see
  inserts because sequence numbers are published after the insert.
- The header comment puts it at about 1.1x the FP rate of a cache-local Bloom
  filter for about 0.9x the latency.
- It is allocated in the memtable arena and freed with the memtable. Because
  its size is fixed, `UpdateWriteBufferSize` cannot grow a memtable that has a
  bloom (memtable.h:774). Option changes apply to new memtables.
- It is never serialized.

---

## 17. PlainTable Bloom filter

- `PlainTableBloomV1` (table/plain/plain_table_bloom.h:24) uses
  `LegacyLocalityBloomImpl<true>` when `bloom_locality > 0`, otherwise
  `LegacyNoLocalityBloomImpl`. It defaults to 6 probes and uses the 32-bit
  `GetSliceHash`.
- `PlainTableOptions::bloom_bits_per_key` defaults to 10. In prefix mode the
  filter holds prefixes; in total-order mode it holds whole keys.
- With `store_index_in_file`, `BloomBlockBuilder` writes the filter as the
  meta block `kBloomBlock` and records the property `kNumBloomBlocks`.
  Otherwise the filter is built in memory when the table is opened
  (`PopulateIndex`, `FillBloom`).
- `PlainTableReader::MatchBloom` (plain_table_reader.cc:513) is called from
  `Get` and `Seek` and updates `bloom_sst_hit_count` / `bloom_sst_miss_count`.
- It is unrelated to `BlockBasedTableOptions::filter_policy`.

---

## 18. Caching and memory accounting

| What | Cache role | Notes |
|---|---|---|
| Full filter block, filter partition | `kFilterBlock` | `ParsedFullFilterBlock` |
| Top-level index of a partitioned filter | `kFilterMetaBlock` | `Block_kFilterPartitionIndex` |
| Construction memory (hash entries, final buffer, Ribbon banding) | `kFilterConstruction` | Only if `cache_usage_options` charges this role. Reserved through `CacheReservationManager`. |
| Old block-based filter | `kDeprecatedFilterBlock` | Obsolete |

- **Priority**: `HIGH` if `cache_index_and_filter_blocks_with_high_priority`
  (default true), otherwise `LOW` (`GetCachePriority`,
  block_based_table_reader.cc:1994).
- **Compressed secondary cache**: `do_not_compress_roles` defaults to
  `{kFilterBlock}`, because filter bits are high-entropy. Filter blocks are not
  compressed on disk either.
- **Memory reporting**: `BlockBasedTable::ApproximateMemoryUsage` (:1947)
  includes `filter->ApproximateMemoryUsage()`. That counts only bytes the
  reader owns, not cached blocks, and it feeds
  `rocksdb.estimate-table-readers-mem`.
- **Uncache on file deletion**: `EraseFromCacheBeforeDestruction`, controlled
  by `uncache_aggressiveness`.
- **Tracing**: the block cache tracer records filter accesses as
  `TraceType::kBlockTraceFilterBlock` (:2420). Per-lookup `GetContext` stats
  include `num_cache_filter_hit`, `num_cache_filter_miss` and
  `num_cache_filter_add`.

---

## 19. Options reference

**`BlockBasedTableOptions`**

| Option | Default | Effect |
|---|---|---|
| `filter_policy` | nullptr | Which filter to build. A policy is also required to read filters. |
| `whole_key_filtering` | true | Add whole keys |
| `format_version` | 7 | `< 5` -> LegacyBloom, `>= 5` -> FastLocalBloom |
| `partition_filters` | false | Use partitioned filters. Requires `index_type = kTwoLevelIndexSearch`, otherwise it is reset to false (block_based_table_factory.cc:495). |
| `decouple_partitioned_filters` | true | Filter partition boundaries independent of index partitions (deprecated setting) |
| `metadata_block_size` | 4096 | Target partition size |
| `optimize_filters_for_memory` | true | Fit filter sizes to allocator size classes (needs `malloc_usable_size` and `format_version >= 5`) |
| `detect_filter_construct_corruption` | false | Checksum plus post-verify; construction is about 30% slower |
| `cache_index_and_filter_blocks` | false | Filters in the block cache rather than owned by the reader |
| `cache_index_and_filter_blocks_with_high_priority` | true | High cache priority for filters |
| `pin_l0_filter_and_index_blocks_in_cache` | false | Older pinning setting |
| `pin_top_level_index_and_filter` | true | Older pinning setting |
| `metadata_cache_options.{top_level_index,partition,unpartitioned}_pinning` | `kFallback` | Current pinning settings |
| `cache_usage_options` (role `kFilterConstruction`) | not charged | Charge construction memory to the block cache |

**`ColumnFamilyOptions`**

| Option | Default | Effect |
|---|---|---|
| `prefix_extractor` | nullptr | Enables prefix filtering in SSTs and in the memtable |
| `optimize_filters_for_hits` | false | No filters for bottommost files, and no filter check at the last level |
| `memtable_prefix_bloom_size_ratio` | 0 | Memtable bloom size as a fraction of `write_buffer_size`. Dynamic. |
| `memtable_whole_key_filtering` | false | Whole keys in the memtable bloom. Dynamic. |
| `memtable_huge_page_size` | 0 | Allocate the memtable bloom from huge pages |
| `bloom_locality` | 0 | PlainTable only |

**`ReadOptions`**

| Option | Default | Effect |
|---|---|---|
| `total_order_seek` | false | Turns off the prefix filter for iterators, unless `auto_prefix_mode` or `prefix_same_as_start` is set |
| `auto_prefix_mode` | false | Use the prefix filter whenever the bounds allow it |
| `prefix_same_as_start` | false | Prefix-bounded iteration; enables filters |
| `iterate_upper_bound` | nullptr | Used by `auto_prefix_mode` and `IsFilterCompatible` |
| `read_tier = kBlockCacheTier` | | No I/O to load a filter; a filter that is not in memory means "maybe" |

**`RibbonFilterPolicy`**: `bloom_before_level`, mutable via
`"table_factory.filter_policy.bloom_before_level"`.

---

## 20. Statistics, perf counters, properties

**Statistics tickers** (`include/rocksdb/statistics.h`)

| Ticker | String | Counted when |
|---|---|---|
| `BLOOM_FILTER_USEFUL` | `rocksdb.bloom.filter.useful` | Whole-key filter said no (Get/MultiGet) |
| `BLOOM_FILTER_FULL_POSITIVE` | `rocksdb.bloom.filter.full.positive` | Whole-key filter said maybe |
| `BLOOM_FILTER_FULL_TRUE_POSITIVE` | `rocksdb.bloom.filter.full.true.positive` | Filter said maybe and the key was found in that file |
| `BLOOM_FILTER_PREFIX_CHECKED` | `rocksdb.bloom.filter.prefix.checked` | Prefix filter queried for a point lookup |
| `BLOOM_FILTER_PREFIX_USEFUL` | `rocksdb.bloom.filter.prefix.useful` | Prefix filter said no |
| `BLOOM_FILTER_PREFIX_TRUE_POSITIVE` | `rocksdb.bloom.filter.prefix.true.positive` | Prefix filter said maybe and the key was found |
| `[NON_]LAST_LEVEL_SEEK_FILTERED` | `rocksdb.[non.]last.level.seek.filtered` | An iterator seek skipped the file |
| `[NON_]LAST_LEVEL_SEEK_FILTER_MATCH` | `rocksdb.[non.]last.level.seek.filter.match` | Filter checked and did not skip |
| `[NON_]LAST_LEVEL_SEEK_DATA_USEFUL_FILTER_MATCH` | | A value was read after a filter match |

Observed whole-key FP rate:

```
FP = FULL_POSITIVE - FULL_TRUE_POSITIVE
FP rate ~= FP / (FP + USEFUL)
```

**Perf context** (`include/rocksdb/perf_context.h`)

| Counter | Meaning |
|---|---|
| `bloom_memtable_hit_count` / `bloom_memtable_miss_count` | Memtable bloom |
| `bloom_sst_hit_count` / `bloom_sst_miss_count` | SST filter (block-based and PlainTable) |
| Per level: `bloom_filter_useful`, `bloom_filter_full_positive`, `bloom_filter_full_true_positive` | Point-lookup filter results by level. **These merge whole-key and prefix results**, unlike the tickers, which keep them in separate `*_PREFIX_*` counters: the prefix branch of `FullFilterKeyMayMatch` bumps the `_full_` and `_useful` per-level counters under the comment "Includes prefix stats". So a per-level counter is not the per-level version of the same-named ticker. |
| `block_cache_filter_hit_count`, `filter_block_read_count`, `block_cache_filter_read_byte`, `filter_block_read_byte`, `read_filter_block_nanos` | Cost of loading filter blocks |

**Table properties**: `filter_policy_name`, `filter_size`,
`num_filter_entries`, `prefix_extractor_name`, plus the whole-key and prefix
filtering flags ([9.4](#94-table-properties-written)).

**Log messages to look for**: "Detected obsolete filter type", "Using legacy
Bloom filter with high (N) bits/key", "Using legacy SST/BBT Bloom filter with
excessive key count", "Too many keys for Ribbon filter", "Cache charging for
Ribbon filter banding failed".

---

## 21. Correctness rules and gotchas

1. **Failures degrade to "may match", with one exception.** An I/O or cache
   error reading the filter, an unknown sub-implementation, non-zero reserved
   bits, an inconsistent `num_lines`, `num_blocks < 2`, a construction that
   detected corruption, and a filter not in memory under `kBlockCacheTier` all
   yield "maybe" (`AlwaysTrueFilter`, or an early `return true`).

   The exception: `GetBuiltinFilterBitsReader` maps a filter of 5 bytes or
   fewer to `AlwaysFalseFilter`, under the comment "filter is empty or broken."
   A filter truncated to that length would therefore filter out every key -- a
   false negative. What rules this out in practice is that it is not reachable
   from a well-formed file (`WriteFilterBlock` returns early when the builder is
   empty, so a zero-length full filter block is never written) and that
   `BlockFetcher` verifies the block checksum on the read path before the bytes
   reach this function. Note that the explicit `DB::VerifyChecksum` /
   `VerifyChecksumInMetaBlocks` path carries a `FIXME?` asking whether it
   reaches filter *partitions* (block_based_table_reader.cc:3197), so do not
   assume that API covers them.
2. **Keys are user keys without timestamps.** `ExtractUserKeyAndStripTimestamp`
   is applied before `Add`. The sequence number and type are not in the
   filter, so one entry covers every version of a key.
3. **Range tombstones are not in the filter.** For SSTs, `TableCache::Get`
   evaluates range tombstones for each file regardless of the filter. Memtable
   `Get` evaluates them before the bloom check. Memtable `MultiGet` turns the
   bloom off when range deletions exist.
4. **Custom comparators** that treat different byte strings as equal need a
   custom `FilterPolicy`, because the built-in filters hash raw bytes.
5. **Prefix extractor contract** (options.h:264): all keys with one prefix must
   be contiguous in comparator order. If not, prefix-filtered iteration can
   miss keys.
6. **Changing `prefix_extractor`.** Iterators use the extractor recorded in
   the table, with the upper-bound check. For `Get` / `MultiGet` with
   `whole_key_filtering = false`, the prefix filter is used only when
   `PrefixExtractorChanged` is false, meaning the passed extractor is either
   pointer-equal to `rep_->table_prefix_extractor` or its `AsString()` matches
   the table property `prefix_extractor_name`. Both call sites carry a FIXME
   saying point lookups should not consult the current extractor at all. With
   `whole_key_filtering = true`, `Get` does not depend on the prefix extractor.
7. **`whole_key_filtering = false`** makes `Get` rely on prefix filters only:
   `FullFilterBlockReader::KeyMayMatch` returns true. The table property
   records how each file was built, and the reader follows it.
8. **Legacy filters (`format_version < 5`) use a 32-bit hash**, so the FP rate
   rises with many keys per filter (a warning is logged above 3M keys).
   Partitioned filters or `format_version >= 5` avoid this.
9. **All built-in policies read each other's filters** because they share
   `CompatibilityName`. A custom policy with a different `CompatibilityName`
   cannot see built-in filters, so those files behave as unfiltered.
10. **`optimize_filters_for_hits`**: bottommost files have no filter at all, so
    a lookup for an absent key pays index and data reads there.
11. **Filter partitions always go through the block cache**, even with
    `cache_index_and_filter_blocks = false`.
12. **The memtable bloom size is fixed** when the memtable is created, so
    option changes apply to the next memtable.

---

## 22. Call-chain cheat sheet

**Build**

```
BlockBasedTableBuilder::Rep::Rep
  -> CreateFilterBlockBuilder
  -> BloomFilterPolicy::GetBuilderWithContext -> FastLocalBloomBitsBuilder
BlockBasedTableBuilder::Add
  -> FilterBlockBuilder::AddWithPrevKey
       FullFilterBlockBuilder:        -> Add
       PartitionedFilterBlockBuilder: -> AddImpl (may CutAFilterBlock first)
  -> XXPH3FilterBitsBuilder::AddKey[AndAlt] -> deque<uint64_t>
BlockBasedTableBuilder::Finish -> WriteFilterBlock
  -> FilterBlockBuilder::Finish -> FastLocalBloomBitsBuilder::Finish
  -> FastLocalBloomImpl::AddHashPrepared -> write block + metaindex entry
```

**Open**

```
BlockBasedTable::Open -> PrefetchIndexAndFilterBlocks -> CreateFilterBlockReader
  -> FullFilterBlockReader::Create -> ReadFilterBlock -> BlockBasedTable::RetrieveBlock
  -> (block_cache.cc) new ParsedFullFilterBlock
  -> BuiltinFilterPolicy::GetBuiltinFilterBitsReader
```

**Get**

```
Version::Get -> TableCache::Get -> BlockBasedTable::Get -> FullFilterKeyMayMatch
  -> FullFilterBlockReader::KeyMayMatch -> GetOrReadFilterBlock
  -> FastLocalBloomBitsReader::MayMatch -> FastLocalBloomImpl::HashMayMatchPrepared
```

**MultiGet**

```
Version::MultiGet -> ... -> BlockBasedTable::MultiGet -> FullFilterKeysMayMatch
  -> FullFilterBlockReader::KeysMayMatch -> FilterBitsReader::MayMatch(batch)
Version::MultiGetAsync -> ProcessBatch -> TableCache::MultiGetFilter
  -> BlockBasedTable::MultiGetFilter -> FullFilterKeysMayMatch
```

**Seek**

```
BlockBasedTableIterator::Seek -> CheckPrefixMayMatch
  -> BlockBasedTable::PrefixRangeMayMatch
  -> FilterBlockReaderCommon::RangeMayExist -> PrefixMayMatch -> MayMatch
```

**Memtable**

```
MemTable::Add        -> DynamicBloom::Add / AddConcurrently
MemTable::Get        -> DynamicBloom::MayContain
MemTable::MultiGet   -> DynamicBloom::MayContain(batch)
MemTableIterator::Seek -> DynamicBloom::MayContain(prefix)
```

---

## 23. Implementing a new filter policy

This section is a practical guide. It assumes you have read sections
[6](#6-filter-policies), [7](#7-bits-builders-and-bits-readers),
[9](#9-on-disk-format) and [10](#10-filter-blocks-full-and-partitioned).

### 23.1 Pick the layer you are replacing

Four layers are involved in an SST filter. Only two of them are pluggable.

| Layer | What it decides | Interface | Pluggable? |
| --- | --- | --- | --- |
| Policy | Which encoding to use for this particular file | `FilterPolicy` (filter_policy.h:100) | **Yes**, public, via `BlockBasedTableOptions::filter_policy` |
| Bits builder / reader | The filter encoding itself: bits, hashing, probing, trailer | `FilterBitsBuilder` / `FilterBitsReader` (filter_policy_internal.h:22, :110) | **Yes**, but the header is internal |
| Filter block | Full vs. partitioned; whole keys vs. prefixes; the partition index | `FilterBlockBuilder` / `FilterBlockReader` (filter_block.h:46, :136) | **No.** Selected by `partition_filters`; not reachable from options |
| Hash and probe math | `FastRange`, probe loop, SIMD | Free functions in `util/bloom_impl.h` | Not an interface; copy the pattern |

So "implement a new filter policy" means in practice: **one `FilterPolicy`, one
`FilterBitsBuilder`, one `FilterBitsReader`.** Everything in the third row you
inherit -- partitioning, whole-key and prefix entries, timestamp stripping,
block-cache residency, pinning, and the statistics of
[section 20](#20-statistics-perf-counters-properties) all live above your code
and work unchanged.

One practical constraint: `FilterBitsBuilder` and `FilterBitsReader` are only
forward-declared in the public header ("As of RocksDB 7.0, the details of these
classes are internal", filter_policy.h:40-42). Defining your own therefore means
including `table/block_based/filter_policy_internal.h`, which means building
inside the RocksDB tree or vendoring that header. A policy that only *selects
among built-in policies* needs no internal header; see
[23.7](#237-a-policy-that-only-selects-among-built-in-policies).

### 23.2 The decision that governs everything: `CompatibilityName()`

A filter block is found by an **exact string match on a metaindex key**, and
that string contains `CompatibilityName()`.

On the write path, `BlockBasedTableBuilder::WriteFilterBlock` builds the key:

```cpp
std::string key;
key = is_partitioned_filter ? BlockBasedTable::kPartitionedFilterBlockPrefix
                            : BlockBasedTable::kFullFilterBlockPrefix;
key.append(rep_->table_options.filter_policy->CompatibilityName());
meta_index_builder->Add(key, filter_block_handle);
```

(block_based_table_builder.cc:2941-2947. The prefixes are `"fullfilter."`,
`"partitionedfilter."` and the obsolete `"filter."`, defined at :3605-3608.)

On the read path, `PrefetchIndexAndFilterBlocks` rebuilds the same three
candidate keys from the *currently configured* policy and takes the first that
exists (block_based_table_reader.cc:1692-1704):

```cpp
auto name = rep_->filter_policy->CompatibilityName();
for (const auto& [filter_type, prefix] : { ...kFullFilterBlockPrefix... }) {
  std::string filter_block_key = prefix + name;
  if (FindMetaBlock(meta_iter, filter_block_key, &rep_->filter_handle).ok()) {
    rep_->filter_type = filter_type;
    break;
  }
}
```

`rep_->filter_type` is initialized to `kNoFilter`
(block_based_table_reader.h:812), so **a name mismatch is silent**: the file is
simply treated as having no filter. There is no warning and no error. The only
symptom is lost read performance.

That gives you exactly two sensible choices.

| `CompatibilityName()` returns | Consequence |
| --- | --- |
| **Your own name** (the usual choice) | Your files carry `"fullfilter.<YourName>"`. Built-in policies cannot find your filters, and your policy cannot find built-in filters. Switching a live DB in either direction disables filtering for every pre-existing file until compaction rewrites it. |
| **A built-in name** (`"rocksdb.BuiltinBloomFilter"`, returned by `BuiltinFilterPolicy::CompatibilityName`, filter_policy.cc:1420) | Your filters occupy the same metaindex slot as built-in ones. Only valid if your bytes are parseable by every built-in policy *and* your reader parses every built-in format. See [23.8](#238-joining-the-built-in-family). |

A wrapper that delegates building to a built-in policy must return the *wrapped*
policy's `CompatibilityName()`, not its own. This is why the base class declares
the method pure virtual rather than defaulting it (filter_policy.h:105-116), and
`LevelAndStyleCustomFilterPolicy` (db/db_bloom_filter_test.cc:1756) is the
in-tree model:

```cpp
// OK to use built-in policy name because we are deferring to a
// built-in builder. We aren't changing the serialized format.
const char* CompatibilityName() const override {
  return policy_fifo_->CompatibilityName();
}
```

`Name()` is a different thing and does **not** affect reading. It is the
`Customizable` id, and it is recorded in
`TableProperties::filter_policy_name` (block_based_table_builder.cc:3037),
which is consumed only for reporting: `sst_dump`, `TableProperties::ToString`
(table_properties.cc:133), the `OnTableFileCreated` listener payload
(db/event_helpers.cc:125) and the C and Java bindings. Nothing branches on it.

### 23.3 A complete minimal policy

This is a working skeleton with every required method, modeled on
`TestHashFilter` (table/block_based/full_filter_block_test.cc:24-107). It
stores one 64-bit hash per key -- useless as a space-efficient filter, but it
shows the whole contract surface and nothing else.

```cpp
#include "rocksdb/filter_policy.h"
#include "table/block_based/filter_policy_internal.h"
#include "util/coding.h"
#include "util/hash.h"

namespace ROCKSDB_NAMESPACE {

class MyBitsBuilder : public FilterBitsBuilder {
 public:
  void AddKey(const Slice& key) override {
    uint64_t h = GetSliceHash64(key);
    // Keys arrive in sorted order with possible duplicates; de-dup cheaply.
    if (hashes_.empty() || hashes_.back() != h) {
      hashes_.push_back(h);
    }
  }

  void AddKeyAndAlt(const Slice& key, const Slice& alt) override {
    AddKey(key);
    AddKey(alt);
  }

  size_t EstimateEntriesAdded() override { return hashes_.size(); }

  // Silence -Woverloaded-virtual for the two-argument Finish.
  using FilterBitsBuilder::Finish;

  Slice Finish(std::unique_ptr<const char[]>* buf) override {
    size_t len = hashes_.size() * 8;
    char* data = new char[len];
    for (size_t i = 0; i < hashes_.size(); ++i) {
      EncodeFixed64(data + i * 8, hashes_[i]);
    }
    buf->reset(data);
    hashes_.clear();  // Required: the builder is reused for each partition.
    return Slice(data, len);
  }

  size_t ApproximateNumEntries(size_t bytes) override { return bytes / 8; }
  size_t CalculateSpace(size_t num_entries) override {
    return num_entries * 8;
  }
  double EstimatedFpRate(size_t, size_t) override { return 0.0; }

 private:
  std::vector<uint64_t> hashes_;
};

class MyBitsReader : public FilterBitsReader {
 public:
  explicit MyBitsReader(const Slice& contents)
      : data_(contents.data()), len_(contents.size()) {}

  using FilterBitsReader::MayMatch;  // keep the batch overload visible

  bool MayMatch(const Slice& entry) override {
    uint64_t h = GetSliceHash64(entry);
    for (size_t i = 0; i + 8 <= len_; i += 8) {
      if (h == DecodeFixed64(data_ + i)) {
        return true;
      }
    }
    return false;
  }

 private:
  const char* data_;
  size_t len_;
};

class MyFilterPolicy : public FilterPolicy {
 public:
  static const char* kClassName() { return "MyFilterPolicy"; }
  const char* Name() const override { return kClassName(); }
  // Own family: cannot read built-in filters, and they cannot read ours.
  const char* CompatibilityName() const override { return kClassName(); }

  FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext& /*context*/) const override {
    return new MyBitsBuilder();
  }

  FilterBitsReader* GetFilterBitsReader(const Slice& contents) const override {
    return new MyBitsReader(contents);
  }
};

}  // namespace ROCKSDB_NAMESPACE
```

Install it with:

```cpp
BlockBasedTableOptions bbto;
bbto.filter_policy = std::make_shared<MyFilterPolicy>();
cf_options.table_factory.reset(NewBlockBasedTableFactory(bbto));
```

Ownership in both factory methods is **transfer to the caller via raw
pointer**. `GetBuilderWithContext`'s result is adopted by
`FullFilterBlockBuilder`'s constructor into a `unique_ptr`
(full_filter_block.cc:25-27); `GetFilterBitsReader`'s result is adopted by
`ParsedFullFilterBlock` (parsed_full_filter_block.cc:14-19). Do not cache or
reuse either object across calls: the builder is per-file and holds mutable
state, and the reader is bound to one specific byte buffer.

### 23.4 `FilterBitsBuilder` method by method

Nine methods, declared at filter_policy_internal.h:22-105. The contracts that
are easy to get wrong are marked.

| Method | Contract | Called from |
| --- | --- | --- |
| `AddKey(key)` | Keys arrive **in comparator-sorted order and may repeat**, so de-duplicating against only the previous entry is sufficient and is what the built-ins do. The key is a **user key with the timestamp already stripped** -- no sequence number, no value type (see [rule 2](#21-correctness-rules-and-gotchas)). | `FullFilterBlockBuilder::Add` / `AddWithPrevKey` |
| `AddKeyAndAlt(key, alt)` | Adds a key and its prefix together. **The de-duplication rules are specific** and documented at filter_policy_internal.h:35-44: de-dup `key` against the previous `key`, `alt` against the previous `alt`, and `key` against `alt` in both directions across adjacent calls -- because a whole key can be its own prefix. Getting this merely approximate costs space, not correctness. | `FullFilterBlockBuilder` when both `whole_key_filtering` and a `prefix_extractor` are active |
| `EstimateEntriesAdded()` | **MUST return 0 if and only if nothing has been added.** `FullFilterBlockBuilder::IsEmpty()` is `EstimateEntriesAdded() == 0` (full_filter_block.h:57), and `WriteFilterBlock` skips writing the block entirely when it is empty. A nonzero floor would emit junk filter blocks; a wrong zero would discard real ones. Otherwise it may be an estimate. It populates `TableProperties::num_filter_entries`. | `BlockBasedTableBuilder` (:2908), `PartitionedFilterBlockBuilder` (:86, :128, :211) |
| `Finish(buf)` | Returns the exact bytes to be written. Ownership of the backing allocation goes to `*buf`, and the returned `Slice` must point into it. **Must reset internal state**: for partitioned filters the same builder is called once per partition (partitioned_filter_block.cc:131-132), so a builder that does not clear its entries after `Finish` produces partitions that each contain all earlier keys. | `FullFilterBlockBuilder::Finish`, `PartitionedFilterBlockBuilder::CutAFilterBlock` |
| `Finish(buf, status)` | Optional override. Default forwards to the one-argument form. Set `*status` to `Corruption` if construction itself went wrong; `MaybePostVerify` is then skipped. | same |
| `MaybePostVerify(filter_content)` | Optional. Called on every successful `Finish` (full_filter_block.h:68-70, partitioned_filter_block.cc:134). Implementations are expected to consult `detect_filter_construct_corruption` themselves and return `OK` when it is off -- the default implementation returns `OK` unconditionally, which is a valid choice. | `BlockBasedTableBuilder` (:2591) via `FilterBlockBuilder::MaybePostVerifyFilter` |
| `ApproximateNumEntries(bytes)` | Keys that fit in `bytes`. **This drives partitioning.** `PartitionedFilterBlockBuilder`'s constructor derives `keys_per_partition_` from it (partitioned_filter_block.cc:52-53), and if the answer is `< 1` it retries with geometrically larger sizes, giving up at 100000 bytes with the comment "might be a broken implementation. substitute something reasonable: 1 key / byte." Returning 0 for every input therefore silently degrades partition sizing rather than failing. | `PartitionedFilterBlockBuilder` ctor |
| `CalculateSpace(num_entries)` | Bytes needed including your metadata. Feeds `FullFilterBlockBuilder::UpdateFilterSizeEstimate` (full_filter_block.cc:50), which is how `CurrentFilterSizeEstimate` reports a running filter size for file-cutting decisions. Should be roughly the inverse of `ApproximateNumEntries`. | `FullFilterBlockBuilder` |
| `EstimatedFpRate(num_entries, bytes)` | Diagnostic and used by `optimize_filters_for_memory` accounting in the built-ins. A constant is acceptable if you do not implement that option. | built-in policies, `filter_bench` |

### 23.5 `FilterBitsReader`

Only one method is required (filter_policy_internal.h:107-124):

```cpp
virtual bool MayMatch(const Slice& entry) = 0;
virtual void MayMatch(int num_keys, Slice** keys, bool* may_match) {
  for (int i = 0; i < num_keys; ++i) {
    may_match[i] = MayMatch(*keys[i]);
  }
}
```

Three things to know:

1. **The reader receives exactly the bytes your `Finish` returned**, with no
   block trailer. `ParsedFullFilterBlock` passes `block_contents_.data`
   straight through, and only when it is non-empty; an empty block yields a
   null reader, and `FullFilterBlockReader` then answers "may match"
   (parsed_full_filter_block.cc:14-19).
2. **Returning `true` is always safe; returning `false` must be certain.** A
   false negative loses data silently. Everything in RocksDB's own error
   handling degrades toward `true` -- see
   [section 21](#21-correctness-rules-and-gotchas), rule 1. Your reader is
   handed potentially corrupt bytes if a checksum was disabled, so bounds-check
   before trusting any length or offset you decode.
3. **Override the batch form if you can do better than the loop.** This is
   where `MultiGet` benefits most: the built-in
   `FastLocalBloomBitsReader::MayMatch(int, Slice**, bool*)` hashes all keys,
   then prefetches all cache lines, then probes
   ([section 7.5](#75-fastlocalbloombitsreader-filter_policycc588)). A custom
   reader that does the same wins the same way.

`BuiltinFilterBitsReader::HashMayMatch(uint64_t)` (filter_policy_internal.h:127-131)
is **not** part of this contract. It exists so that
`XXPH3FilterBitsBuilder::MaybePostVerify` can re-query a just-built filter using
the 64-bit hashes it already has (filter_policy.cc:1386-1399). You need it only
if you build on `BuiltinFilterPolicy`.

### 23.6 The `FilterPolicy` object

Four methods, all pure virtual (`Name()` comes from `Customizable`):

- **`Name()`** -- the `Customizable` id. Must be stable across releases if you
  persist OPTIONS files. Convention in this tree is a static
  `kClassName()` returning it, so registration and the override share one
  literal.
- **`CompatibilityName()`** -- see [23.2](#232-the-decision-that-governs-everything-compatibilityname).
- **`GetBuilderWithContext(context)`** -- **returning `nullptr` means "write no
  filter for this file"**, and is a supported, first-class answer. Both
  built-ins use it for the `bits_per_key == 0` case (filter_policy.cc:1483-1485),
  and `AlwaysTrueFilterPolicy` in db_bloom_filter_test.cc:725 uses it to model
  "skip". This is the hook for per-file policy decisions.
- **`GetFilterBitsReader(contents)`** -- must handle every format your builder
  has ever emitted, including from older versions of your code. The caller
  retains ownership of the buffer behind the `Slice`; your reader must not
  outlive it, which it does not, since `ParsedFullFilterBlock` owns both.

`FilterBuildingContext` (filter_policy.h:48-92) is what makes
`GetBuilderWithContext` interesting. It is stack-allocated and must not be
retained. Fields worth branching on:

| Field | Use |
| --- | --- |
| `table_options` | `format_version`, `partition_filters`, `optimize_filters_for_memory`, `detect_filter_construct_corruption`, `block_cache`, `cache_usage_options` |
| `level_at_creation` | `-1` for flush and `SstFileWriter`; this is how `NewRibbonFilterPolicy`'s `bloom_before_level` works |
| `is_bottommost` | Known-last-sorted-run for the key range; see [section 11.3](#113-where-is_bottommost-comes-from) |
| `compaction_style`, `num_levels` | Shape of the LSM tree |
| `reason` | `kFlush`, `kCompaction`, `kRecovery`, `kSstFileWriter`, `kMisc` |
| `column_family_name`, `db_name` | Per-CF or per-DB tuning. `db_name` is non-owning |
| `is_remote_compaction` | File built by a `CompactionService` worker |
| `info_log` | Where to log a configuration warning. Log once per policy instance, as `GetLegacyBloomBuilderWithContext` does with its `warned_` flag |

### 23.7 A policy that only selects among built-in policies

If your goal is "Bloom here, Ribbon there, nothing in the bottommost level",
you do not need a new encoding at all, and you do not need any internal header.
Wrap the built-ins and delegate. This is the case the public header explicitly
endorses (filter_policy.h:95-99), and
`LevelAndStyleCustomFilterPolicy` (db/db_bloom_filter_test.cc:1756-1795) is the
reference implementation -- it is even flagged in the source as "referenced by
HISTORY.md as a model for a wrapper FilterPolicy".

The three rules for a wrapper:

1. `CompatibilityName()` returns the **wrapped** policy's name.
2. `GetFilterBitsReader` may defer to **any** of the wrapped built-in policies,
   because every built-in reads every built-in format ("OK to defer to any of
   them; they all can parse built-in filters from any settings").
3. `GetBuilderWithContext` returns `nullptr` from the branches where you want
   no filter.

```cpp
FilterBitsBuilder* GetBuilderWithContext(
    const FilterBuildingContext& context) const override {
  if (context.is_bottommost) {
    return nullptr;  // no filter in the bottommost run
  } else if (context.level_at_creation <= 0) {
    return bloom_->GetBuilderWithContext(context);
  } else {
    return ribbon_->GetBuilderWithContext(context);
  }
}
```

A wrapper is also the cheap way to get per-CF bits-per-key, which
`BlockBasedTableOptions` cannot express directly, by switching on
`context.column_family_name`.

### 23.8 Joining the built-in family

Only do this if you want existing built-in-filter files to remain readable
under your policy and vice versa. It means accepting the built-in on-disk
contract described in [section 9.1](#91-filter-bytes-and-the-5-byte-trailer):
a 5-byte trailer (`kMetadataLen = 5`, filter_policy.cc:50) whose first byte is
the implementation marker, read by `GetBuiltinFilterBitsReader`
(filter_policy.cc:1649):

| First trailer byte | Meaning |
| --- | --- |
| `>= 1` (1..127) | LegacyBloom; the value is `num_probes` |
| `0` | `AlwaysTrueFilter` |
| `-1` | Newer Bloom; sub-dispatch in `GetBloomBitsReader` |
| `-2` | Ribbon; sub-dispatch in `GetRibbonBitsReader` |
| any other negative | **Reserved**, currently decoded as `AlwaysTrueFilter` |

The practical path is therefore:

1. Claim an unused negative marker and add a `case` to the `switch` in
   `GetBuiltinFilterBitsReader`. The `default` branch already degrades unknown
   markers to `AlwaysTrueFilter`, so **older RocksDB builds will read your
   files as unfiltered rather than failing** -- that is the forward
   compatibility this scheme buys, and it is the reason to use it.
2. Derive your policy from `BuiltinFilterPolicy` so you inherit
   `GetFilterBitsReader` (which routes to `GetBuiltinFilterBitsReader`) and
   `CompatibilityName`. You then only override `Name()` and
   `GetBuilderWithContext`.
3. Keep the remaining 4 trailer bytes meaningful to your decoder. The built-ins
   use them differently per implementation; there is no shared meaning beyond
   byte 0.

Note the asymmetry this creates with marker `0`: `GetBuiltinFilterBitsReader`
also maps any filter of 5 bytes or fewer to `AlwaysFalseFilter`, which is the
single false-negative path in the whole system. See
[section 21](#21-correctness-rules-and-gotchas), rule 1, for why it is
unreachable from a well-formed file. If you add a marker, do not add a new way
to reach a false-negative reader.

### 23.9 What you inherit, and what you must do yourself

**Inherited, no work required:**

| Feature | Where it lives |
| --- | --- |
| Full vs. partitioned filters, and the partition index | `FullFilterBlockBuilder` / `PartitionedFilterBlockBuilder` ([10.2](#102-fullfilterblockbuilder-full_filter_blockcc), [10.3](#103-partitionedfilterblockbuilder-partitioned_filter_blockcc)) |
| `whole_key_filtering`, prefix entries, `prefix_extractor` plumbing | `FullFilterBlockBuilder`; reaches you as `AddKey` / `AddKeyAndAlt` |
| Timestamp stripping from keys | `ExtractUserKeyAndStripTimestamp` before `Add` |
| Block cache residency, `cache_index_and_filter_blocks`, pinning tiers | `FilterBlockReaderCommon` ([10.5](#105-filterblockreadercommontblocklike-filter_block_reader_commonhcc)) |
| `optimize_filters_for_hits`, `Version::IsFilterSkipped` | `db/version_set.cc`, above the table layer |
| All the tickers and perf counters of [section 20](#20-statistics-perf-counters-properties) | `BlockBasedTable::FullFilterKeyMayMatch` and friends |
| `TableProperties::num_filter_entries`, `filter_size`, `filter_policy_name` | `BlockBasedTableBuilder::WritePropertiesBlock` |

**Not inherited -- your builder must opt in explicitly:**

| Feature | What is required |
| --- | --- |
| `optimize_filters_for_memory` | Entirely inside the built-in builder. `GetFastLocalBloomBuilderWithContext` passes `&aggregate_rounding_balance_` only when the option is on (filter_policy.cc:1523), and the builder's `AllocateMaybeRounding` rounds allocations to `malloc_usable_size` size classes. Ignoring the option is a legitimate choice; the option simply has no effect on your filters. |
| `detect_filter_construct_corruption` | Read it off `context.table_options` and implement `MaybePostVerify` (and optionally the two-argument `Finish`). Not reading it means the option silently does nothing. |
| Charging construction memory to the block cache | Build the `CacheReservationManager` yourself. The pattern is `GetFastLocalBloomBuilderWithContext` (filter_policy.cc:1504-1521): check `cache_usage_options.options_overrides` for `CacheEntryRole::kFilterConstruction`, fall back to `cache_usage_options.options.charged`, and only construct a manager when `block_cache` is set and the decision is `kEnabled`. |
| Adapting to `format_version` | `BloomFilterPolicy::GetBuilderWithContext` branches on `format_version < 5` to pick LegacyBloom. If your encoding has versions, do the same. |
| An efficient batch `MayMatch` | See [23.5](#235-filterbitsreader). |

### 23.10 Registering for OPTIONS files and `CreateFromString`

`filter_policy` is persisted as a `Customizable` id
(`OptionTypeInfo::AsCustomSharedPtr<const FilterPolicy>` with
`kByNameAllowFromNull`, block_based_table_factory.cc:330-333). Without
registration your policy still works when set programmatically, but
`DB::Open` from a saved OPTIONS file, `GetOptionsFromString`, and
`db_bench --options_file` will all fail to reconstruct it.

Register a factory with `ObjectLibrary::Default()`. Note the template argument
is **`const FilterPolicy`**, matching the `shared_ptr<const FilterPolicy>`
option type:

```cpp
static void RegisterMyFilterPolicy() {
  static std::once_flag loaded;
  std::call_once(loaded, []() {
    ObjectLibrary::Default()->AddFactory<const FilterPolicy>(
        MyFilterPolicy::kClassName(),
        [](const std::string& /*uri*/,
           std::unique_ptr<const FilterPolicy>* guard,
           std::string* /*errmsg*/) {
          guard->reset(new MyFilterPolicy());
          return guard->get();
        });
  });
}
```

`RegisterBuiltinFilterPolicies` (filter_policy.cc:1937-2031) is the in-tree
model, and `ParseCompressionDisplayNameManager::Register`
(db/db_table_properties_test.cc:80-90) is the same `call_once` pattern for a
sibling interface.

For a parameterized policy -- `"myfilter:10"` -- use `ObjectLibrary::PatternEntry`
the way `FilterPatternEntryWithBits` does, and parse the `":"`-delimited tail in
the factory. `FilterPolicy::CreateFromString` (filter_policy.cc:2034) registers
the built-ins lazily under its own `call_once`, then calls
`options.registry->NewSharedObject(id, policy)`, so your registration must have
happened before the first `CreateFromString` that needs it. Calling your
`Register()` from `main`, or from a static initializer in your translation unit,
both work.

To round-trip configuration (not just the bare id), override the `Configurable`
hooks and register option fields; `BloomFilterPolicy::GetId`
(filter_policy.cc:1496-1500) shows how the built-ins encode `bits_per_key` into
the id string instead, which is simpler and is enough for most policies.

### 23.11 Compile and link gotchas

- **Overloaded virtual hiding.** `FilterBitsBuilder::Finish` and
  `FilterBitsReader::MayMatch` are each overloaded. Overriding one hides the
  other and trips `-Woverloaded-virtual`. Add `using FilterBitsBuilder::Finish;`
  and `using FilterBitsReader::MayMatch;` -- both in-tree examples do, with the
  comment "Silence compiler warning about overloaded virtual".
- **No RTTI in release builds.** Production code and `db_stress` build with
  `-fno-rtti`. Use `static_cast_with_check` from `util/cast_util.h` rather than
  `dynamic_cast` if you need to downcast a policy.
- **Pointer-returning factories.** Both `FilterPolicy` factory methods return
  raw pointers that the caller adopts. Returning a pointer you also keep is a
  double-free.
- **`const`-correctness.** `BlockBasedTableOptions::filter_policy` is
  `shared_ptr<const FilterPolicy>`, and `GetBuilderWithContext` /
  `GetFilterBitsReader` are both `const`. Any per-policy accumulator, such as
  the built-ins' `aggregate_rounding_balance_` and `warned_`, must be `mutable`
  and thread-safe -- these methods are called concurrently from flush and
  compaction threads.
- **Portability.** Filter code is a natural place for intrinsics. Guard SIMD on
  the macros `util/bloom_impl.h` uses, and remember that the CI matrix includes
  MSVC, MinGW, ARM, libc++ and TSAN/ASAN/UBSAN builds.

### 23.12 Testing

| What to test | Where | How |
| --- | --- | --- |
| Encoding round-trip, FP rate, edge counts (0, 1, huge) | `util/bloom_test.cc` | `build_tools/rocksptest.sh bloom_test` |
| Policy plugged into the filter-block layer | `table/block_based/full_filter_block_test.cc` | The `PluginFullFilterBlockTest` fixture already exists for exactly this: it constructs `MockBlockBasedTableTester(new TestHashFilter)` and drives `FullFilterBlockBuilder` / `FullFilterBlockReader` directly |
| End-to-end DB behavior, options, properties, stats | `db/db_bloom_filter_test.cc` | `build_tools/rocksptest.sh db_bloom_filter_test` |
| Partitioned filters | `table/block_based/partitioned_filter_block_test.cc` | Verify your `ApproximateNumEntries` yields sane `keys_per_partition_` |
| OPTIONS round-trip | `options/customizable_test.cc` (`MockFilterPolicy` at :1375 is the minimal stub) | `CreateFromString` then compare `Name()` |
| Performance | `util/filter_bench.cc` | Built-in impls are reachable through `-impl` via `BloomLikeFilterPolicy::GetAllFixedImpls` (filter_policy.cc:2069); add yours there if it joins the built-in family |

Two assertions worth writing explicitly, because they catch the two contract
violations that are silent in normal use:

```cpp
// 1. No false negatives: every added key must match.
for (const auto& k : added) {
  ASSERT_TRUE(reader->MayMatch(k));
}

// 2. Finish() leaves the builder empty and reusable (partitioned filters).
builder->AddKey("a");
builder->Finish(&buf1);
ASSERT_EQ(0, builder->EstimateEntriesAdded());
```

Then stress for flakiness as the project requires:

```bash
COERCE_CONTEXT_SWITCH=1 build_tools/rockstest.sh db_bloom_filter_test -r100 \
    --gtest_filter="*MyFilterPolicy*"
```

### 23.13 Build system and source checks

A new `.cc` file must be added to **`Makefile`, `CMakeLists.txt`, `src.mk`, and
`BUCK`**. Do not hand-edit `BUCK`; update `src.mk` and run
`/usr/local/bin/python3 buckifier/buckify_rocksdb.py`. Every new source file
needs the dual-licensed Meta header. Then:

```bash
make check-sources     # rejects non-ASCII characters and other source issues
make format-auto
AUTO_CLEAN=1 make -j$(nproc) check
AUTO_CLEAN=1 ASSERT_STATUS_CHECKED=1 make -j$(nproc) check
```

If the policy is user-facing, also support it in `db_bench` and in
`db_stress_tool` (which exposes filter configuration through `--bloom_bits`,
declared at db_stress_tool/db_stress_common.h:178),
and add a one-line `HISTORY.md` entry.

### 23.14 Checklist

- [ ] `CompatibilityName()` decided deliberately: own family, or built-in with
      an allocated marker byte. A wrapper returns the wrapped policy's name.
- [ ] `Name()` stable and matching the `ObjectLibrary` registration id.
- [ ] `EstimateEntriesAdded()` returns 0 exactly when nothing was added.
- [ ] `Finish()` clears builder state so the builder is reusable per partition.
- [ ] `Finish()`'s returned `Slice` points into the buffer handed to `*buf`.
- [ ] `ApproximateNumEntries()` returns `>= 1` for realistic partition sizes.
- [ ] Reader bounds-checks every decoded length and offset; never returns
      `false` for a key that was added.
- [ ] `using FilterBitsBuilder::Finish;` and `using FilterBitsReader::MayMatch;`
      present.
- [ ] `GetBuilderWithContext` returns `nullptr` where no filter is wanted.
- [ ] Mutable policy state is `mutable` and thread-safe.
- [ ] `detect_filter_construct_corruption` and `optimize_filters_for_memory`
      either honored or knowingly ignored.
- [ ] Registered with `ObjectLibrary::Default()` as `const FilterPolicy` if
      OPTIONS files matter.
- [ ] Build files updated; `make check-sources` clean.

---

## 24. Per-file access history for filter block cache admission

This section covers a specific design: a filter made of **many blocks**, where
each block's **block-cache admission** is decided from runtime history
accumulated **per SST file**. It is a RocksDB-internals change, not a
`FilterPolicy` implementation, so it is separate from
[section 23](#23-implementing-a-new-filter-policy).

The history consists of three per-file counters:

| Counter | Meaning |
| --- | --- |
| **Accesses** | Lookups that reached this file |
| **Found** | Lookups this file answered |
| **Not found** | Lookups this file did not answer -- **both** because the filter returned negative **and** because data blocks were read and the key was not in them, not distinguished |

That "not found" definition is already the definition your tree uses.
`KeyLookupOutcome` (trace_replay/key_lookup_tracer.h:29-37) says so in as many
words:

```cpp
enum class KeyLookupOutcome : uint8_t {
  // The filter excluded the file, or data blocks were read and the key was
  // not present in them. These two cases are deliberately not distinguished.
  kNotFound = 0,
  kFoundValue = 1,
  kFoundTombstone = 2,
  kFoundMergeOperand = 3,
  kError = 4,
};
```

So the classification does not need to be designed. It needs to be *routed*,
and that is the hard part, for the reason in
[24.2](#242-the-outcome-is-classified-above-the-table-reader).

### 24.1 Why this cannot live in the policy layer

`FilterPolicy`, `FilterBitsBuilder` and `FilterBitsReader` never see a
`BlockHandle`, a `Cache`, or a `ReadOptions`. They see bytes. Cache admission
is decided one layer up, in the filter *block* reader, which
[section 23.1](#231-pick-the-layer-you-are-replacing) notes is not pluggable
from options.

The decision itself is a single bool. `BlockBasedTable::RetrieveBlock`
(block_based_table_reader_sync_and_async.h:248-264) branches on it:

```cpp
if (use_cache) {
  Status s = CO_AWAIT(MaybeReadBlockAndLoadToCache, ...);
  if (!s.ok()) CO_RETURN s;
  if (out_parsed_block->GetValue() != nullptr ||
      out_parsed_block->GetCacheHandle() != nullptr) {
    CO_RETURN s;
  }
}
// Falls through to a direct file read ending in SetOwnedValue.
```

With `use_cache = false` the block is read, parsed, used, and freed with its
`CachableEntry`. Nothing enters the block cache and nothing is charged to it.

The existing filter call sites hardcode the argument:

| Call site | `use_cache` today |
| --- | --- |
| `FilterBlockReaderCommon::ReadFilterBlock` (filter_block_reader_common.cc:34) | Plumbed from `cache_index_and_filter_blocks` via `CreateFilterBlockReader`. **Already controllable** |
| `PartitionedFilterBlockReader::GetFilterPartitionBlock` (partitioned_filter_block.cc:468-472) | Hardcoded `true`. This is why table.h:517 says filter partitions "use block cache even when `cache_index_and_filter_blocks=false`" |

So the top-level block of a two-level filter is already yours to control; the
leaf blocks are not. Owning that call means owning the filter block reader --
Route B in [section 23.1](#231-pick-the-layer-you-are-replacing).

### 24.2 The outcome is classified above the table reader

This is the structural constraint that shapes everything else.

```
Version::Get                     <-- outcome known HERE, after the call returns
  -> TableCache::Get
       -> BlockBasedTable::Get
            -> FullFilterKeyMayMatch
                 -> your filter block reader
                      -> RetrieveBlock(..., use_cache, ...)   <-- decision made HERE
            -> data block reads
```

An access count can be incremented at the consult site, because "this file was
consulted" is known there. **Found and not-found cannot.** Whether the file
answered the query is only determined after `table_cache_->Get` returns, in
`Version::Get` (db/version_set_sync_and_async.h:127-136):

```cpp
if (UNLIKELY(lookup_id != KeyLookupTracer::kReservedLookupId)) {
  probes.push_back(
      {fp.GetHitFileLevel(), f->fd.GetNumber(),
       ClassifyProbeOutcome(*status, get_context.State(), operands_before,
                            merge_context->GetNumOperands()),
       static_cast<uint32_t>(blocks_before),
       static_cast<uint32_t>(blocks.size() - blocks_before)});
}
```

So the two sites are in different layers:

| | Site | What it has |
| --- | --- | --- |
| **Write** the counters | `Version::Get`, after `table_cache_->Get` | `f->file_metadata`, `fp.GetHitFileLevel()`, the outcome. **No table reader** |
| **Read** the counters | Your filter block reader, inside `BlockBasedTable::Get` | `rep_`, the `BlockHandle`. **No `FileMetaData`, no outcome** |

This supersedes the obvious choice of holding the counters in your
`FilterBlockReader` subclass. That works for an access count and cannot work
for found / not-found, because the reader never learns the outcome. The state
has to live somewhere both layers reach.

### 24.3 Where the per-file state lives

Given [24.2](#242-the-outcome-is-classified-above-the-table-reader), two homes
are viable.

| Home | Reachable from `Version::Get`? | Reachable from the filter reader? | Lifetime |
| --- | --- | --- | --- |
| **A CFD-owned registry keyed by file number** | Yes, via `vset_` / `cfd_` | Yes, via a raw pointer on `Rep` | Yours to choose |
| `FileMetaData::stats` | Yes, directly -- `f->file_metadata` is in hand | Only if a `FileMetaData*` or `FileSampledStats*` is plumbed into `Rep` at open time | Version |

**The registry is the better fit, and your tree already contains the template
for it.** `KeyLookupTracer` is exactly this shape: owned by the column family
(db/column_family.h:928), referenced by `VersionSet` (db/version_set.h:1889),
passed through `TableReaderOptions::key_lookup_tracer`
(table/table_builder.h:130) by `TableCache` (db/table_cache.cc:196), and landed
on `Rep` in `BlockBasedTable::Open` (block_based_table_reader.cc:891):

```cpp
rep->key_lookup_tracer = key_lookup_tracer;
rep->file_number = cur_file_num;
```

A `FileAccessHistory*` follows the identical path, and `rep->file_number` is
already there to key into it. The registry also decouples history lifetime from
table reader lifetime, which matters for the reasons in
[24.6](#246-what-resets-the-history).

The `FileMetaData::stats` route has real precedent -- `FileSampledStats`
(db/version_edit.h:229-243) is a per-file runtime access counter already:

```cpp
struct FileSampledStats {
  // number of user reads to this file.
  mutable std::atomic<uint64_t> num_reads_sampled;
  mutable std::atomic<uint64_t> num_collapsible_entry_reads_sampled;
};
```

and upstream even keeps a crude not-found counter there:
`sample_collapsible_entry_file_read_inc` fires when the per-file state is
`kNotFound`, `kMerge` or `kDeleted` (db/version_set_sync_and_async.h:318-322).
Note that is **not** your definition -- it counts tombstones and merge operands
as not-found, where you would count them as found. Do not reuse that counter;
it answers a different question.

The cost of the `FileMetaData` route is that the table reader cannot see it
without new plumbing, and `FileMetaData` is per-`Version` while the reader is
per-`TableCache`-entry, so the pointer needs a lifetime argument. The registry
avoids both.

### 24.4 Classifying found vs not found

Use `ClassifyProbeOutcome` (db/key_lookup_trace_scope.h:31-54). It already
exists, it already encodes your definition, and it already handles the trap
described below:

```cpp
inline KeyLookupOutcome ClassifyProbeOutcome(const Status& status,
                                             GetContext::GetState state,
                                             size_t operands_before,
                                             size_t operands_after) {
  if (!status.ok()) {
    return KeyLookupOutcome::kError;
  }
  switch (state) {
    case GetContext::kFound:   return KeyLookupOutcome::kFoundValue;
    case GetContext::kDeleted: return KeyLookupOutcome::kFoundTombstone;
    case GetContext::kMerge:
      return operands_after > operands_before
                 ? KeyLookupOutcome::kFoundMergeOperand
                 : KeyLookupOutcome::kNotFound;
    case GetContext::kNotFound: return KeyLookupOutcome::kNotFound;
    ...
  }
}
```

**The trap: `GetContext::State()` is cumulative across files.** Your own
comment at db/key_lookup_trace_scope.h:26-30 states it: "It is *cumulative*
across files: once it reaches kMerge it stays kMerge whether or not this file
contributed anything." A per-file counter that reads `get_context.State()`
directly would therefore attribute a merge to the file that produced it **and
to every file searched afterwards**. The `operands_before` / `operands_after`
delta is what makes the merge case per-file. Do not simplify it away.

Mapping the five outcomes onto your two counters is a decision, not a
derivation:

| `KeyLookupOutcome` | The file answered the query? |
| --- | --- |
| `kFoundValue` | Yes, unambiguously |
| `kFoundTombstone` | Yes -- a tombstone is an answer, and reading it cost data block I/O |
| `kFoundMergeOperand` | Partially -- the lookup continues to older files, so this file is both a hit and not the end of the search |
| `kNotFound` | No. Filter negative and fruitless data block reads both land here |
| `kError` | Neither. Counting these as not-found makes an I/O-error-prone file look cold |

The merge case is the one that genuinely needs a call. If your workload has no
merge operator, `kMerge` never appears and the question is moot; check that
before spending design effort on it.

### 24.5 Update hooks

Two hooks are needed, because the two read paths classify differently.

**Point lookups** -- `Version::Get` (db/version_set_sync_and_async.h:14). Put
the update next to the existing probe recording at :127-136, where
`f->file_metadata`, the level and the outcome are all in scope. Reuse the
`blocks_before` / `operands_before` capture that is already there rather than
adding a second one.

**MultiGet** -- `Version::MultiGetFromSST` (db/version_set_sync_and_async.h:269)
**does not classify outcomes at all.** It only does the sampled counters:

```cpp
if (get_context.sample()) {
  sample_file_read_inc(f->file_metadata);
  if (get_context.State() == GetContext::kNotFound ||
      get_context.State() == GetContext::kMerge ||
      get_context.State() == GetContext::kDeleted) {
    sample_collapsible_entry_file_read_inc(f->file_metadata);
  }
}
```

Counters updated only from `Version::Get` are therefore **blind to all MultiGet
traffic**, which for many workloads is most of it. The per-key loop at :302-324
is the place to add the parallel hook: it iterates the batch with a per-key
`GetContext` and `Status`, so each key can be classified independently. Note
that one file yields a *mix* of outcomes for one batch, so the counters take
several increments per MultiGet per file, not one.

Two more paths worth a decision:

- **Range tombstone short-circuit.** `Version::Get` breaks out of the file loop
  when `*max_covering_tombstone_seq > 0` (:91-96), so files that would have been
  searched below the covering tombstone are never probed and get no update.
  That is correct -- they genuinely were not accessed -- but it means a
  delete-heavy workload leaves lower files with sparse history.
- **Iterators.** `Seek` consults filters through `PrefixMayMatch` (see
  [section 15](#15-read-path-iterators-and-prefix-seek)) and has no "found"
  notion at all. If scans matter, either exclude them from the history or
  define an outcome for them explicitly.

### 24.6 What resets the history

If the counters live in the registry rather than in the table reader, the first
item below stops applying -- which is a good reason to prefer the registry.

1. **Table cache eviction.** `TableReader` objects live in `TableCache`, sized
   by `max_open_files`. The default `-1` (options.h:776) opens every file at
   `DB::Open` and never evicts. A finite value evicts readers at arbitrary
   times, taking any reader-held state with them. Registry-held state survives.
2. **Compaction.** Output files are new files with empty histories, and there
   is no inheritance path. Building one means deciding what "the history of a
   file assembled from six others" means.
3. **Trivial moves.** The file survives but `rep_->level` goes stale: "the
   level when the table is opened, could potentially change when trivial move
   is involved" (block_based_table_reader.h:873). Your tracer already encodes
   this with `KeyLookupFileOp::kMove` and the note that a move must not be read
   as delete-plus-create. If the rule is keyed on level, take the level from
   `fp.GetHitFileLevel()` per lookup instead.
4. **Reopen.** Nothing persists across restarts unless written out.
5. **Registry eviction.** A registry keyed by file number grows with the number
   of files ever opened, so it needs its own reclamation, driven by file
   deletion. Your tracer's `KeyLookupFileOp::kDelete` is the signal.

None of these are correctness problems; a cold history just yields a different
decision. They are measurement problems, so decide which ones the experiment
intends to include.

### 24.7 Thread safety

One registry entry serves every concurrent lookup against the file, so
**counters must be atomic**, with `std::memory_order_relaxed`: they are
statistics driving a heuristic and nothing is ordered against them.
`sample_file_read_inc` uses exactly that (monitoring/file_read_sample.h:31-34).
Do not take a lock; the path runs once per SST per lookup, per key.

If the counters instead live on an object reached through a const path, they
also need `mutable`. The constness is not uniform:

| Method | Const? |
| --- | --- |
| `FilterBlockReader::KeyMayMatch`, `KeysMayMatch` (filter_block.h:150, :155) | **Non-const** |
| `PartitionedFilterBlockReader::MayMatch` (partitioned_filter_block.h:195, :203) | `const` |
| `PartitionedFilterBlockReader::GetFilterPartitionBlock` (partitioned_filter_block.h:185-189) | `const` |

Reading is fine from any of them, since `std::atomic::load` is const.
`FileSampledStats` declares its counters `mutable` (db/version_edit.h:241)
because `FileMetaData` is passed by const reference.

One subtlety specific to three counters: they are read **non-atomically with
respect to each other**. A rule like "not_found / accesses > 0.9" can observe
a torn pair where `accesses` has been incremented but the outcome has not,
because the two updates happen at different layers and different times. For a
heuristic this is harmless; just do not assert the invariant
`found + not_found + errors == accesses`, because it is transiently false by
construction.

### 24.8 Sample rather than count

A shared atomic incremented on every lookup against a hot file is a contention
point, which is why RocksDB's own per-file read counting is sampled
(monitoring/file_read_sample.h):

```cpp
static const uint32_t kFileReadSampleRate = 1024;

inline bool should_sample_file_read() {
  bool result = (Random::GetTLSInstance()->Next() % kFileReadSampleRate == 307);
  TEST_SYNC_POINT_CALLBACK("should_sample_file_read:override", &result);
  return result;
}

inline void sample_file_read_inc(const FileMetaData* meta) {
  meta->stats.num_reads_sampled.fetch_add(kFileReadSampleRate,
                                          std::memory_order_relaxed);
}
```

The increment adds the **sample rate**, not 1, so the counter estimates the
true count rather than the sample count, and the TLS random source avoids both
a shared counter and a syscall.

Sampling interacts with found / not-found specifically:

- A **ratio** (`not_found / accesses`) survives sampling, because both
  numerator and denominator are scaled by the same factor -- *provided the same
  sampling decision gates both*. Sample once per lookup and update all three
  counters under that one decision; sampling them independently decorrelates
  numerator and denominator and the ratio becomes meaningless.
- **Absolute counts** do not survive usefully at 1-in-1024 for cold files. A
  file accessed 50 times has an expected sample count of 0.
- A ratio from few samples is noisy. If the rule is "admit when the not-found
  ratio exceeds X", require a minimum sample count before trusting the ratio,
  or a newly-opened file will flip decisions on its first one or two samples.

Since the counters are per file rather than global, contention is already far
lower than for a DB-wide counter. Exact counts are plausible here; measure
before assuming sampling is required.

### 24.9 Reading it: the admission point

The decision is made where the filter block is fetched:

```cpp
Status MyFilterBlockReader::GetFilterBlock(
    const BlockHandle& handle, GetContext* get_context,
    BlockCacheLookupContext* lookup_context, const ReadOptions& read_options,
    CachableEntry<ParsedFullFilterBlock>* filter_block) const {
  // Per-file history, uniform across this file's filter blocks.
  const bool admit = history_ != nullptr && history_->ShouldAdmit();

  return table()->RetrieveBlock(
      /* prefetch_buffer */ nullptr, read_options, handle,
      /* decomp */ nullptr, filter_block, get_context, lookup_context,
      /* for_compaction */ false, /* use_cache */ admit,
      /* async_read */ false, /* use_block_cache_for_lookup */ admit);
}
```

where `history_` was resolved once at reader construction from
`rep_->key_lookup_tracer`'s sibling registry, keyed by `rep_->file_number`.
Resolve it once per reader, not per lookup: a hash lookup per filter probe
would cost more than the filter probe itself.

Three things to get right.

**Keep `use_block_cache_for_lookup` consistent with `use_cache`.** Passing
`use_cache = false` with `use_block_cache_for_lookup = true` asks for a lookup
in a cache you are not populating. Coherent only if something else populates
it, such as `prepopulate_block_cache` or an earlier admitting period.

**A per-file rule still produces mixed per-block outcomes.** If the threshold
is crossed mid-file, early blocks were refused and later ones admitted, so the
file's blocks are not uniformly resident. If the simulator assumes
per-file uniformity, re-evaluate on a fixed schedule or record the decision per
block access.

**Decide what a zero-history file does.** Every file starts with no history,
and the bootstrap choice is load-bearing: admit-by-default makes the rule an
eviction policy, refuse-by-default makes it an admission policy, and the two
give different cache contents for the same workload.

### 24.10 The `kBlockCacheTier` interaction

Immediately after the `use_cache` branch, `RetrieveBlock` does
(block_based_table_reader_sync_and_async.h:265-270):

```cpp
assert(out_parsed_block->IsEmpty());

const bool no_io = ro.read_tier == kBlockCacheTier;
if (no_io) {
  CO_RETURN Status::Incomplete("no blocking io");
}
```

So under `ReadOptions::read_tier == kBlockCacheTier`, a block you refused to
admit cannot be consulted at all: the fetch returns `Incomplete` and the caller
degrades to "may match" (see
[section 21](#21-correctness-rules-and-gotchas), rule 1). Correctness holds,
but a non-admitting file loses **all** filtering under memory-only reads, not
just the caching of its filter.

There is a feedback loop here worth noticing: degrading to "may match" turns a
would-be filter-negative into a data block read, which changes the outcome your
counters record from not-found-via-filter to not-found-via-data-blocks. Both
count as not-found under your definition, so the ratio is stable, but the I/O
cost is not. Either force `use_cache = true` when
`read_options.read_tier == kBlockCacheTier`, or record the degradation.

### 24.11 Interaction with existing options

| Option | Interaction |
| --- | --- |
| `cache_index_and_filter_blocks` | Controls the top-level filter block only ([24.1](#241-why-this-cannot-live-in-the-policy-layer)). Leave it `true` while refusing leaves |
| `metadata_cache_options` pinning tiers | A pinned block is held in `filter_map_` and never re-consults the cache, so pinning silently overrides the rule for those blocks |
| `prepopulate_block_cache` | Inserts at write time, before any history exists, so blocks can be cached despite a refusing rule |
| `ReadOptions::fill_cache` | Per-operation; already suppresses insertion |
| `optimize_filters_for_hits` | Bottommost files get no filter, so they accumulate accesses but never a filter-negative not-found |
| `uncache_aggressiveness` | `EraseFromCacheBeforeDestruction` (partitioned_filter_block.cc:687) evicts on reader teardown, interleaving with admissions |
| `max_open_files` | See [24.6](#246-what-resets-the-history) |

### 24.12 Observability

- **Tickers.** A refused block produces **no** ticker activity -- not a miss,
  not an add -- because `MaybeReadBlockAndLoadToCache` is never entered. So
  `BLOCK_CACHE_FILTER_HIT` / `_MISS` will make the filter look cheaper than it
  is. Count refusals separately or the numbers mislead.
- **Your trace.** `KeyLookupTraceWriter::WriteIteratorAccess` already carries a
  `no_insert` flag (trace_replay/key_lookup_tracer.h:114-119), and RocksDB
  computes the same notion for the block cache trace as
  `no_insert = no_io || !ro.fill_cache` (block_based_table_reader.cc:2438).
  Extending that to include "the admission rule refused" keeps one concept
  rather than two.
- **The probe record already carries the outcome.** `KeyLookupProbe` holds
  `level`, `file_number` and `outcome`, so a trace already contains everything
  needed to reconstruct the per-file counters offline. Worth checking a
  candidate rule against an existing trace before implementing it online.

Also emit the history value at decision time, not just the decision. Without
it, a run where the rule never fired is indistinguishable from one where it
fired uniformly.

### 24.13 Checklist

- [ ] Counters live somewhere both `Version::Get` and the filter block reader
      can reach -- a CFD-owned registry keyed by file number, following the
      `KeyLookupTracer` plumbing path.
- [ ] Found / not-found derived from `ClassifyProbeOutcome`, **not** from
      `get_context.State()` directly, because that state is cumulative across
      files.
- [ ] Decided which outcomes count as "found": tombstones and merge operands
      are the ambiguous ones.
- [ ] `kError` handled as its own case, not folded into not-found.
- [ ] A hook added in `Version::MultiGetFromSST` too, or the history is blind
      to MultiGet traffic.
- [ ] Counters atomic, `memory_order_relaxed`, no lock on the consult path; no
      assertion that `found + not_found + errors == accesses`.
- [ ] If sampling, one sampling decision gates all three counters, and a
      minimum sample count is required before trusting a ratio.
- [ ] Bootstrap behavior for a zero-history file chosen deliberately.
- [ ] `use_cache` and `use_block_cache_for_lookup` passed consistently.
- [ ] `read_tier == kBlockCacheTier` handled explicitly, or its filter
      degradation accepted and recorded.
- [ ] Rule not silently overridden by pinning or `prepopulate_block_cache`.
- [ ] Registry entries reclaimed on file deletion.
- [ ] Refusals counted and traced, since they are invisible to the block cache
      tickers.
