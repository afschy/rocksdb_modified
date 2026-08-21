//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "monitoring/instrumented_mutex.h"
#include "rocksdb/key_lookup_trace_options.h"
#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/status.h"
#include "trace_replay/key_lookup_block_access.h"
#include "util/autovector.h"

namespace ROCKSDB_NAMESPACE {

class Env;
class StreamingCompress;
class WritableFile;

// What a single SST file contributed to a point lookup.
enum class KeyLookupOutcome : uint8_t {
  // The filter excluded the file, or data blocks were read and the key was
  // not present in them. These two cases are deliberately not distinguished.
  kNotFound = 0,
  kFoundValue = 1,
  kFoundTombstone = 2,
  kFoundMergeOperand = 3,
  kError = 4,
};

// How a point lookup ended overall.
enum class KeyLookupResult : uint8_t {
  kNotFound = 0,
  kFound = 1,
  kDeleted = 2,
  kError = 3,
  // The search stopped early because a range tombstone covered the key.
  kRangeDeleted = 4,
};

// What happened to an SST file in the live file set.
enum class KeyLookupFileOp : uint8_t {
  kCreate = 0,
  kDelete = 1,
  // A trivial move: the file was relabeled to a different level, not
  // rewritten. Its blocks stay byte identical, so a cache simulator must not
  // treat this as a delete plus a create.
  kMove = 2,
};

// One SST file searched by a point lookup. The data blocks read from the file
// live in a separate flat buffer owned by the caller; this records the range
// [first_block_idx, first_block_idx + num_blocks) within it.
struct KeyLookupProbe {
  uint32_t level = 0;
  uint64_t file_number = 0;
  KeyLookupOutcome outcome = KeyLookupOutcome::kNotFound;
  uint32_t first_block_idx = 0;
  uint32_t num_blocks = 0;
};

// Per-request probe buffer, sized so that a typical lookup needs no heap
// allocation. The matching block buffers are KeyLookupBlockAccesses and
// KeyLookupIterBlocks from trace_replay/key_lookup_block_access.h.
using KeyLookupProbes = autovector<KeyLookupProbe, 8>;

// Writes key lookup records as plain text, optionally zstd compressed. Not
// thread safe; callers serialize through KeyLookupTracer.
class KeyLookupTraceWriter {
 public:
  KeyLookupTraceWriter() = default;
  ~KeyLookupTraceWriter();
  // No copy and move.
  KeyLookupTraceWriter(const KeyLookupTraceWriter&) = delete;
  KeyLookupTraceWriter& operator=(const KeyLookupTraceWriter&) = delete;
  KeyLookupTraceWriter(KeyLookupTraceWriter&&) = delete;
  KeyLookupTraceWriter& operator=(KeyLookupTraceWriter&&) = delete;

  // Creates the trace file and writes the header line. Returns NotSupported
  // if compression was requested but is unavailable in this build.
  Status NewWritableFile(const std::string& trace_file_path, Env* env,
                         const KeyLookupTraceOptions& trace_options);

  // Appends one point lookup record.
  Status WriteLookup(uint64_t seq, uint64_t timestamp_us, uint64_t lookup_id,
                     uint32_t cf_id, KeyLookupResult final_result,
                     const KeyLookupProbes& probes,
                     const KeyLookupBlockAccesses& blocks,
                     uint64_t max_trace_file_size);

  // Appends one file lifecycle record. `num_entries` and `file_size` describe
  // the file itself; 0 means the caller could not recover that number, which
  // only really happens for `num_entries`. `to_level` is written only for
  // kMove.
  Status WriteFileLifecycle(uint64_t seq, uint64_t timestamp_us,
                            KeyLookupFileOp op, uint32_t cf_id,
                            uint64_t file_number, uint64_t num_entries,
                            uint64_t file_size, uint32_t level,
                            uint32_t to_level, uint64_t max_trace_file_size);

  // Appends one iterator access record, covering the blocks one table iterator
  // read from one file. A long scan emits several of these sharing an
  // `iter_id`.
  Status WriteIteratorAccess(uint64_t seq, uint64_t timestamp_us,
                             uint32_t cf_id, uint32_t caller, uint64_t iter_id,
                             uint32_t level, uint64_t file_number,
                             bool no_insert,
                             const KeyLookupIterBlocks& blocks,
                             uint64_t max_trace_file_size);

 private:
  // Appends one block tuple: <block_id>/<seq>/<read_bytes>/<uncomp_bytes>.
  void AppendBlock(const KeyLookupBlockAccess& block);
  // Routes line_buffer_ to the file, honoring the size limit and compressing
  // when enabled. Once the limit is reached a final "# truncated" line is
  // appended and all later records are dropped.
  Status AppendLine(uint64_t max_trace_file_size);
  // Compresses and writes whatever has accumulated in staging_.
  Status FlushStaging();
  // Flushes pending output and closes the file. Safe to call more than once.
  Status CloseFile();

  std::unique_ptr<WritableFile> file_;
  // Reused across records so that writing one costs no allocation.
  std::string line_buffer_;
  // Uncompressed bytes awaiting compression. Unused when not compressing.
  std::string staging_;
  std::unique_ptr<StreamingCompress> compress_;
  std::unique_ptr<char[]> compress_out_;
  size_t compress_out_size_ = 0;
  // Bytes actually written to the file, i.e. compressed bytes when
  // compressing.
  uint64_t bytes_written_ = 0;
  bool truncated_ = false;
};

// Traces point lookups, iterator block accesses, and file lifecycle events for
// offline block cache simulation. Thread safe.
class KeyLookupTracer {
 public:
  KeyLookupTracer();
  ~KeyLookupTracer();
  // No copy and move.
  KeyLookupTracer(const KeyLookupTracer&) = delete;
  KeyLookupTracer& operator=(const KeyLookupTracer&) = delete;
  KeyLookupTracer(KeyLookupTracer&&) = delete;
  KeyLookupTracer& operator=(KeyLookupTracer&&) = delete;

  // Starts writing key lookup records to a new file at `trace_file_path`.
  Status StartTrace(const KeyLookupTraceOptions& trace_options,
                    const std::string& trace_file_path, Env* env);

  // Stops tracing and closes the trace file.
  void EndTrace();

  bool is_tracing_enabled() const {
    return writer_.load(std::memory_order_relaxed) != nullptr;
  }

  // Valid only while tracing is enabled; set before the writer is published
  // and not modified until EndTrace().
  const KeyLookupTraceOptions& options() const { return trace_options_; }

  // Returns 0 if this lookup should not be traced (tracing off, or the lookup
  // was not sampled), otherwise a nonzero lookup id. Cycles from 1 to
  // uint64_t max; 0 is reserved.
  uint64_t NextLookupId();

  // Position of the next record or block access in the trace-wide order.
  // Timestamps cannot substitute: a lookup's record is emitted when the lookup
  // ends, so under concurrency an iterator access made during that lookup
  // carries an earlier timestamp than the lookup that logically precedes it.
  uint64_t NextSeq() {
    return seq_counter_.fetch_add(1, std::memory_order_relaxed);
  }

  // Identifies one table iterator, so that continuation records can be
  // stitched back together and two concurrent scans over the same file stay
  // distinguishable.
  uint64_t NextIterId() {
    return iter_id_counter_.fetch_add(1, std::memory_order_relaxed);
  }

  // Identifies one StartTrace()..EndTrace() session. A table iterator can
  // outlive the session it was created in, and StartTrace() resets the
  // sequence and iterator id counters, so a stale iterator flushing into a
  // later session would inject records with colliding ids and out-of-range
  // sequence numbers. Holders compare this against the value they captured and
  // discard their buffer when it differs.
  uint64_t session_id() const {
    return session_id_.load(std::memory_order_relaxed);
  }

  // Whether iterator accesses from `caller` (a TableReaderCaller) should be
  // traced. Callers outside the mask are never collected.
  bool ShouldTraceIterator(int caller) const {
    return trace_options_.record_iterator_accesses && caller >= 0 &&
           caller < 16 &&
           (trace_options_.iterator_caller_mask &
            static_cast<uint16_t>(uint16_t{1} << caller)) != 0;
  }

  Status WriteLookup(uint64_t seq, uint64_t timestamp_us, uint64_t lookup_id,
                     uint32_t cf_id, KeyLookupResult final_result,
                     const KeyLookupProbes& probes,
                     const KeyLookupBlockAccesses& blocks);

  Status WriteFileLifecycle(uint64_t timestamp_us, KeyLookupFileOp op,
                            uint32_t cf_id, uint64_t file_number,
                            uint64_t num_entries, uint64_t file_size,
                            uint32_t level, uint32_t to_level);

  Status WriteIteratorAccess(uint64_t seq, uint64_t timestamp_us,
                             uint32_t cf_id, uint32_t caller, uint64_t iter_id,
                             uint32_t level, uint64_t file_number,
                             bool no_insert,
                             const KeyLookupIterBlocks& blocks);

  // Reserved lookup id, also the "do not trace this lookup" sentinel.
  static constexpr uint64_t kReservedLookupId = 0;

  // Records buffered per table iterator before a flush. Caps both the memory a
  // live iterator holds (24 bytes per entry) and how much is lost on a crash.
  static constexpr size_t kIterFlushThreshold = 1024;

 private:
  KeyLookupTraceOptions trace_options_;
  // Protects writer_ for writes; is_tracing_enabled() reads it lock-free.
  InstrumentedMutex trace_writer_mutex_;
  std::atomic<KeyLookupTraceWriter*> writer_{nullptr};
  std::atomic<uint64_t> lookup_id_counter_{1};
  std::atomic<uint64_t> seq_counter_{1};
  std::atomic<uint64_t> iter_id_counter_{1};
  // Incremented by every StartTrace(), so it never repeats within a process.
  std::atomic<uint64_t> session_id_{0};
};

}  // namespace ROCKSDB_NAMESPACE
