//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "trace_replay/key_lookup_tracer.h"

#include <cassert>

#include "rocksdb/env.h"
#include "rocksdb/version.h"
#include "util/compression.h"
#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {

namespace {

const char kTruncatedLine[] = "# truncated\n";

// Uncompressed bytes accumulated before a batch is compressed and written.
// One batch becomes one zstd frame. Compressing anything much smaller is
// pointless: ZSTDStreamingCompress ends the frame on every call, so a
// per-record batch would pay a frame header per record and restart the window
// each time.
constexpr size_t kCompressBatchSize = 256 * 1024;

// Safely above ZSTD_compressBound() for kCompressBatchSize, so one Compress()
// call consumes a whole batch.
constexpr size_t kCompressOutSize =
    kCompressBatchSize + kCompressBatchSize / 8 + 1024;

// The version RocksDB's streaming compressors use internally.
constexpr uint32_t kCompressFormatVersion = 2;

const char* BlockIdModeName(KeyLookupBlockIdMode mode) {
  switch (mode) {
    case KeyLookupBlockIdMode::kOrdinal:
      return "ordinal";
    case KeyLookupBlockIdMode::kOffset:
      return "offset";
  }
  return "unknown";
}

const char* FileOpName(KeyLookupFileOp op) {
  switch (op) {
    case KeyLookupFileOp::kCreate:
      return "create";
    case KeyLookupFileOp::kDelete:
      return "delete";
    case KeyLookupFileOp::kMove:
      return "move";
  }
  return "unknown";
}

}  // namespace

KeyLookupTraceWriter::~KeyLookupTraceWriter() {
  CloseFile().PermitUncheckedError();
}

Status KeyLookupTraceWriter::CloseFile() {
  if (!file_) {
    return Status::OK();
  }
  // Flush before closing, or the tail of the trace is lost. When compressing
  // this also terminates the final frame.
  Status s = FlushStaging();
  Status flush_status = file_->Flush();
  Status close_status = file_->Close();
  file_.reset();
  if (!s.ok()) {
    flush_status.PermitUncheckedError();
    close_status.PermitUncheckedError();
    return s;
  }
  if (!flush_status.ok()) {
    close_status.PermitUncheckedError();
    return flush_status;
  }
  return close_status;
}

Status KeyLookupTraceWriter::NewWritableFile(
    const std::string& trace_file_path, Env* env,
    const KeyLookupTraceOptions& trace_options) {
  if (trace_file_path.empty()) {
    return Status::InvalidArgument("The provided trace_file_path is empty.");
  }
  if (env == nullptr) {
    return Status::InvalidArgument("The provided Env is null.");
  }
  if (trace_options.compression != kNoCompression &&
      trace_options.compression != kZSTD) {
    return Status::InvalidArgument(
        "Key lookup trace compression must be kNoCompression or kZSTD.");
  }

  if (trace_options.compression == kZSTD) {
    CompressionOptions compression_opts;
    compression_opts.level = trace_options.compression_level;
    compress_ = StreamingCompress::Create(kZSTD, compression_opts,
                                          kCompressFormatVersion,
                                          kCompressBatchSize);
    if (compress_ == nullptr) {
      // Do not silently write plain text under a .zst name.
      return Status::NotSupported(
          "This build does not support zstd stream compression.");
    }
    compress_out_size_ = kCompressOutSize;
    compress_out_.reset(new char[compress_out_size_]);
    staging_.reserve(kCompressBatchSize + 1024);
  }

  Status s = env->NewWritableFile(trace_file_path, &file_, EnvOptions());
  if (!s.ok()) {
    return s;
  }

  line_buffer_.clear();
  line_buffer_.append("# rocksdb_key_lookup_trace v3 block_id_mode=");
  line_buffer_.append(BlockIdModeName(trace_options.block_id_mode));
  line_buffer_.append(" size_unit=bytes rocksdb=");
  AppendNumberTo(&line_buffer_, ROCKSDB_MAJOR);
  line_buffer_.push_back('.');
  AppendNumberTo(&line_buffer_, ROCKSDB_MINOR);
  line_buffer_.push_back('\n');

  // The header is always written, whatever the size limit is. It goes through
  // the same path as every other line so that a compressed trace is a pure
  // .zst file rather than a plaintext header followed by compressed bytes.
  if (compress_) {
    staging_.append(line_buffer_);
    return Status::OK();
  }
  s = file_->Append(line_buffer_);
  if (s.ok()) {
    bytes_written_ += line_buffer_.size();
  }
  return s;
}

Status KeyLookupTraceWriter::FlushStaging() {
  if (!compress_ || staging_.empty() || !file_) {
    return Status::OK();
  }
  // Compress() must be called repeatedly with the same input pointer until it
  // reports nothing remaining. staging_ must not be modified while that loop
  // runs, since the compressor holds a pointer into it.
  const char* input = staging_.data();
  const size_t input_size = staging_.size();
  int remaining = 0;
  do {
    size_t output_pos = 0;
    remaining =
        compress_->Compress(input, input_size, compress_out_.get(), &output_pos);
    if (remaining < 0) {
      return Status::Corruption(
          "zstd stream compression failed while writing key lookup trace.");
    }
    if (output_pos > 0) {
      Status s = file_->Append(Slice(compress_out_.get(), output_pos));
      if (!s.ok()) {
        return s;
      }
      bytes_written_ += output_pos;
    }
  } while (remaining > 0);
  // The frame is complete, so reset the compressor before staging_ is reused.
  // Compress() decides whether input is new by comparing the pointer, and
  // staging_.clear() keeps the same allocation, so the next batch would come
  // back with an identical data() and be mistaken for the batch just finished.
  // Without this, every batch after the first is silently dropped.
  compress_->Reset();
  staging_.clear();
  return Status::OK();
}

Status KeyLookupTraceWriter::AppendLine(uint64_t max_trace_file_size) {
  if (compress_) {
    // Compressed output cannot be measured before it is produced, so the limit
    // is enforced against bytes already written and checked once per batch.
    // The file can therefore overshoot by at most one batch.
    if (bytes_written_ > max_trace_file_size) {
      truncated_ = true;
      staging_.append(kTruncatedLine);
      return FlushStaging();
    }
    staging_.append(line_buffer_);
    if (staging_.size() >= kCompressBatchSize) {
      return FlushStaging();
    }
    return Status::OK();
  }

  if (bytes_written_ + line_buffer_.size() > max_trace_file_size) {
    // Mark the end of the trace so that a reader can tell truncation apart
    // from a clean end, then stop writing for good.
    truncated_ = true;
    Status s = file_->Append(Slice(kTruncatedLine, sizeof(kTruncatedLine) - 1));
    if (s.ok()) {
      bytes_written_ += sizeof(kTruncatedLine) - 1;
    }
    return s;
  }
  Status s = file_->Append(line_buffer_);
  if (s.ok()) {
    bytes_written_ += line_buffer_.size();
  }
  return s;
}

void KeyLookupTraceWriter::AppendBlock(const KeyLookupBlockAccess& block) {
  AppendNumberTo(&line_buffer_, block.block_id);
  line_buffer_.push_back('/');
  AppendNumberTo(&line_buffer_, block.seq);
  line_buffer_.push_back('/');
  AppendNumberTo(&line_buffer_, block.read_bytes);
  line_buffer_.push_back('/');
  AppendNumberTo(&line_buffer_, block.uncomp_bytes);
}

Status KeyLookupTraceWriter::WriteLookup(uint64_t seq, uint64_t timestamp_us,
                                         uint64_t lookup_id, uint32_t cf_id,
                                         KeyLookupResult final_result,
                                         const KeyLookupProbes& probes,
                                         const KeyLookupBlockAccesses& blocks,
                                         uint64_t max_trace_file_size) {
  if (!file_ || truncated_) {
    return Status::OK();
  }
  const size_t num_probes = probes.size();
  const size_t num_blocks = blocks.size();

  line_buffer_.clear();
  line_buffer_.append("G,");
  AppendNumberTo(&line_buffer_, seq);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, timestamp_us);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, lookup_id);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, cf_id);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, static_cast<uint64_t>(final_result));
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, num_probes);
  line_buffer_.push_back(',');
  for (size_t i = 0; i < num_probes; i++) {
    const KeyLookupProbe& probe = probes[i];
    if (i != 0) {
      line_buffer_.push_back('|');
    }
    AppendNumberTo(&line_buffer_, probe.level);
    line_buffer_.push_back(':');
    AppendNumberTo(&line_buffer_, probe.file_number);
    line_buffer_.push_back(':');
    AppendNumberTo(&line_buffer_, static_cast<uint64_t>(probe.outcome));
    const size_t first = probe.first_block_idx;
    const size_t last = first + probe.num_blocks;
    assert(last <= num_blocks);
    if (last > num_blocks) {
      // Defensive: never read past the caller's buffer.
      continue;
    }
    for (size_t b = first; b < last; b++) {
      line_buffer_.push_back(':');
      AppendBlock(blocks[b]);
    }
  }
  line_buffer_.push_back('\n');

  return AppendLine(max_trace_file_size);
}

Status KeyLookupTraceWriter::WriteFileLifecycle(
    uint64_t seq, uint64_t timestamp_us, KeyLookupFileOp op, uint32_t cf_id,
    uint64_t file_number, uint64_t num_entries, uint64_t file_size,
    uint32_t level, uint32_t to_level, uint64_t max_trace_file_size) {
  if (!file_ || truncated_) {
    return Status::OK();
  }
  line_buffer_.clear();
  line_buffer_.append("F,");
  AppendNumberTo(&line_buffer_, seq);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, timestamp_us);
  line_buffer_.push_back(',');
  line_buffer_.append(FileOpName(op));
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, cf_id);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, file_number);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, num_entries);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, file_size);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, level);
  if (op == KeyLookupFileOp::kMove) {
    line_buffer_.push_back(',');
    AppendNumberTo(&line_buffer_, to_level);
  }
  line_buffer_.push_back('\n');

  return AppendLine(max_trace_file_size);
}

Status KeyLookupTraceWriter::WriteIteratorAccess(
    uint64_t seq, uint64_t timestamp_us, uint32_t cf_id, uint32_t caller,
    uint64_t iter_id, uint32_t level, uint64_t file_number, bool no_insert,
    const KeyLookupIterBlocks& blocks, uint64_t max_trace_file_size) {
  if (!file_ || truncated_) {
    return Status::OK();
  }
  const size_t num_blocks = blocks.size();

  line_buffer_.clear();
  line_buffer_.append("A,");
  AppendNumberTo(&line_buffer_, seq);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, timestamp_us);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, cf_id);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, caller);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, iter_id);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, level);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, file_number);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, no_insert ? 1 : 0);
  line_buffer_.push_back(',');
  AppendNumberTo(&line_buffer_, num_blocks);
  line_buffer_.push_back(',');
  for (size_t b = 0; b < num_blocks; b++) {
    if (b != 0) {
      line_buffer_.push_back(':');
    }
    AppendBlock(blocks[b]);
  }
  line_buffer_.push_back('\n');

  return AppendLine(max_trace_file_size);
}

KeyLookupTracer::KeyLookupTracer() { writer_.store(nullptr); }

KeyLookupTracer::~KeyLookupTracer() { EndTrace(); }

Status KeyLookupTracer::StartTrace(const KeyLookupTraceOptions& trace_options,
                                   const std::string& trace_file_path,
                                   Env* env) {
  InstrumentedMutexLock lock_guard(&trace_writer_mutex_);
  if (writer_.load()) {
    return Status::Busy();
  }
  std::unique_ptr<KeyLookupTraceWriter> writer(new KeyLookupTraceWriter());
  Status s = writer->NewWritableFile(trace_file_path, env, trace_options);
  if (!s.ok()) {
    return s;
  }
  lookup_id_counter_.store(1);
  seq_counter_.store(1);
  iter_id_counter_.store(1);
  session_id_.fetch_add(1);
  // Set before publishing the writer: readers only consult trace_options_
  // once they have observed a non-null writer_.
  trace_options_ = trace_options;
  writer_.store(writer.release());
  return Status::OK();
}

void KeyLookupTracer::EndTrace() {
  InstrumentedMutexLock lock_guard(&trace_writer_mutex_);
  if (!writer_.load()) {
    return;
  }
  delete writer_.load();
  writer_.store(nullptr);
}

uint64_t KeyLookupTracer::NextLookupId() {
  if (!writer_.load(std::memory_order_relaxed)) {
    return kReservedLookupId;
  }
  uint64_t id = lookup_id_counter_.fetch_add(1);
  if (id == kReservedLookupId) {
    // Wrapped around; 0 is reserved, so fetch and add again.
    id = lookup_id_counter_.fetch_add(1);
  }
  const uint64_t sampling_frequency = trace_options_.sampling_frequency;
  if (sampling_frequency > 1 && (id % sampling_frequency) != 0) {
    return kReservedLookupId;
  }
  return id;
}

Status KeyLookupTracer::WriteLookup(uint64_t seq, uint64_t timestamp_us,
                                    uint64_t lookup_id, uint32_t cf_id,
                                    KeyLookupResult final_result,
                                    const KeyLookupProbes& probes,
                                    const KeyLookupBlockAccesses& blocks) {
  if (!writer_.load()) {
    return Status::OK();
  }
  InstrumentedMutexLock lock_guard(&trace_writer_mutex_);
  KeyLookupTraceWriter* writer = writer_.load();
  if (!writer) {
    return Status::OK();
  }
  return writer->WriteLookup(seq, timestamp_us, lookup_id, cf_id, final_result,
                             probes, blocks,
                             trace_options_.max_trace_file_size);
}

Status KeyLookupTracer::WriteFileLifecycle(uint64_t timestamp_us,
                                           KeyLookupFileOp op, uint32_t cf_id,
                                           uint64_t file_number,
                                           uint64_t num_entries,
                                           uint64_t file_size, uint32_t level,
                                           uint32_t to_level) {
  if (!writer_.load()) {
    return Status::OK();
  }
  InstrumentedMutexLock lock_guard(&trace_writer_mutex_);
  KeyLookupTraceWriter* writer = writer_.load();
  if (!writer) {
    return Status::OK();
  }
  // Allocated under the lock so that the sequence numbers of lifecycle records
  // match the order they appear in the file.
  const uint64_t seq = NextSeq();
  return writer->WriteFileLifecycle(seq, timestamp_us, op, cf_id, file_number,
                                    num_entries, file_size, level, to_level,
                                    trace_options_.max_trace_file_size);
}

Status KeyLookupTracer::WriteIteratorAccess(uint64_t seq, uint64_t timestamp_us,
                                            uint32_t cf_id, uint32_t caller,
                                            uint64_t iter_id, uint32_t level,
                                            uint64_t file_number,
                                            bool no_insert,
                                            const KeyLookupIterBlocks& blocks) {
  if (!writer_.load()) {
    return Status::OK();
  }
  InstrumentedMutexLock lock_guard(&trace_writer_mutex_);
  KeyLookupTraceWriter* writer = writer_.load();
  if (!writer) {
    return Status::OK();
  }
  return writer->WriteIteratorAccess(seq, timestamp_us, cf_id, caller, iter_id,
                                     level, file_number, no_insert, blocks,
                                     trace_options_.max_trace_file_size);
}

}  // namespace ROCKSDB_NAMESPACE
