//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>

#include "rocksdb/compression_type.h"
#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

// How a data block is identified in a key lookup trace.
enum class KeyLookupBlockIdMode : uint8_t {
  // Zero-based index of the block among the file's data blocks, in offset
  // order. This identifier stays meaningful after the SST file is gone, but it
  // requires building a per-file map of data block offsets on the first traced
  // access to each file. Building that map walks the whole index; with a
  // partitioned index (kTwoLevelIndexSearch) that reads every index partition,
  // which is real extra I/O against the workload being measured. Prefer
  // kOffset for partitioned-index workloads.
  kOrdinal,
  // Raw byte offset of the block within the file. No setup cost, but mapping
  // an offset back to a block index later requires the SST file to still
  // exist.
  kOffset,
};

struct KeyLookupTraceOptions {
  // Capture one in every `sampling_frequency` lookups. 0 or 1 captures all.
  //
  // Note that this drops whole lookups uniformly, which breaks the per-block
  // access histories a block cache simulator needs. It is useful for cheap
  // workload characterization; a simulation run should leave it at 1 and
  // control trace volume with `iterator_caller_mask` instead.
  uint64_t sampling_frequency = 1;

  // Stop writing once the trace file exceeds this size.
  //
  // When `compression` is enabled this counts compressed bytes actually
  // written, and is checked once per compressed batch rather than per record,
  // so the file may overshoot the limit by at most one batch.
  uint64_t max_trace_file_size = uint64_t{64} * 1024 * 1024 * 1024;

  // Whether to record the data blocks read from each file. Setting this to
  // false records only the file sequence and skips all block-capture work.
  bool record_blocks = true;

  KeyLookupBlockIdMode block_id_mode = KeyLookupBlockIdMode::kOrdinal;

  // Compression applied to the trace file as it is written. Only
  // kNoCompression (the default) and kZSTD are supported; anything else is
  // rejected by StartTrace(). kZSTD produces a standard .zst file: records are
  // batched and each batch becomes one zstd frame, and concatenated frames
  // decode transparently with `zstd -d` or Python's zstandard module.
  //
  // Requesting kZSTD on a build without zstd support returns NotSupported
  // rather than silently falling back to plain text.
  CompressionType compression = kNoCompression;

  // zstd compression level, used only when `compression` is kZSTD.
  // CompressionOptions::kDefaultCompressionLevel means "let zstd choose",
  // which is level 3.
  //
  // Level 1 is a good choice here: trace text is enormously redundant, so most
  // of the achievable ratio is available at a fraction of the CPU cost, and
  // compression runs while holding the tracer's writer lock.
  int compression_level = 1;

  // Whether to record data block accesses made by table iterators, which
  // covers user iterators, compaction, ingestion, and verification. Checked at
  // the collection site, so false means no collection work at all rather than
  // suppressed output.
  bool record_iterator_accesses = true;

  // Record an iterator access only if bit (1 << caller) is set, where `caller`
  // is a TableReaderCaller value. TableReaderCaller has fewer than 16 values,
  // so 16 bits suffice. The default traces every caller.
  //
  // Compaction typically outweighs user iterators by one to two orders of
  // magnitude on a write-heavy workload, so masking out
  // TableReaderCaller::kCompaction is the main lever for trace volume.
  uint16_t iterator_caller_mask = 0xFFFF;
};

}  // namespace ROCKSDB_NAMESPACE
