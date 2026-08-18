//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstddef>
#include <cstdint>

#include "rocksdb/status.h"
#include "rocksdb/system_clock.h"
#include "rocksdb/types.h"
#include "table/get_context.h"
#include "trace_replay/key_lookup_tracer.h"
#include "util/autovector.h"

// Helper types for key lookup tracing in Version::Get. They live here rather
// than in db/version_set_sync_and_async.h because that file is included twice
// in the same translation unit (once per coroutine variant), so it must not
// define any type or non-inline function.

namespace ROCKSDB_NAMESPACE {

// Classifies what one SST file contributed to a point lookup.
//
// `state` is GetContext's state after searching the file. It is *cumulative*
// across files: once it reaches kMerge it stays kMerge whether or not this
// file contributed anything. So the merge case is decided by whether the file
// actually added operands, not by the state alone.
inline KeyLookupOutcome ClassifyProbeOutcome(const Status& status,
                                             GetContext::GetState state,
                                             size_t operands_before,
                                             size_t operands_after) {
  if (!status.ok()) {
    return KeyLookupOutcome::kError;
  }
  switch (state) {
    case GetContext::kFound:
      return KeyLookupOutcome::kFoundValue;
    case GetContext::kDeleted:
      return KeyLookupOutcome::kFoundTombstone;
    case GetContext::kMerge:
      return operands_after > operands_before
                 ? KeyLookupOutcome::kFoundMergeOperand
                 : KeyLookupOutcome::kNotFound;
    case GetContext::kNotFound:
      return KeyLookupOutcome::kNotFound;
    case GetContext::kCorrupt:
    case GetContext::kUnexpectedBlobIndex:
    case GetContext::kMergeOperatorFailed:
      return KeyLookupOutcome::kError;
  }
  return KeyLookupOutcome::kError;
}

// Emits one key lookup trace record when it goes out of scope, covering every
// return path out of Version::Get.
class KeyLookupTraceScope {
 public:
  // `tracer` is null when this lookup is not being traced, which makes the
  // destructor a no-op. All pointers must outlive this object.
  //
  // `seq` is allocated at lookup start rather than at emit time, so that it
  // precedes the sequence numbers of the blocks this lookup reads and a sort
  // by sequence number yields causal order. It also gives a lookup that reads
  // no blocks at all, because every candidate file was filtered out, a
  // position in the trace-wide order.
  KeyLookupTraceScope(KeyLookupTracer* tracer, uint64_t seq,
                      uint64_t lookup_id, uint32_t cf_id, SystemClock* clock,
                      const Status* status, const GetContext* get_context,
                      const SequenceNumber* max_covering_tombstone_seq,
                      const KeyLookupProbes* probes,
                      const KeyLookupBlockAccesses* blocks)
      : tracer_(tracer),
        seq_(seq),
        lookup_id_(lookup_id),
        cf_id_(cf_id),
        clock_(clock),
        status_(status),
        get_context_(get_context),
        max_covering_tombstone_seq_(max_covering_tombstone_seq),
        probes_(probes),
        blocks_(blocks) {}

  // No copy and move.
  KeyLookupTraceScope(const KeyLookupTraceScope&) = delete;
  KeyLookupTraceScope& operator=(const KeyLookupTraceScope&) = delete;
  KeyLookupTraceScope(KeyLookupTraceScope&&) = delete;
  KeyLookupTraceScope& operator=(KeyLookupTraceScope&&) = delete;

  ~KeyLookupTraceScope() {
    if (tracer_ == nullptr) {
      return;
    }
    tracer_
        ->WriteLookup(seq_, clock_->NowMicros(), lookup_id_, cf_id_,
                      ComputeFinalResult(), *probes_, *blocks_)
        .PermitUncheckedError();
  }

 private:
  KeyLookupResult ComputeFinalResult() const {
    if (!status_->ok() && !status_->IsNotFound()) {
      return KeyLookupResult::kError;
    }
    switch (get_context_->State()) {
      case GetContext::kFound:
        return KeyLookupResult::kFound;
      case GetContext::kDeleted:
        return KeyLookupResult::kDeleted;
      default:
        break;
    }
    if (*max_covering_tombstone_seq_ > 0) {
      return KeyLookupResult::kRangeDeleted;
    }
    return KeyLookupResult::kNotFound;
  }

  KeyLookupTracer* const tracer_;
  const uint64_t seq_;
  const uint64_t lookup_id_;
  const uint32_t cf_id_;
  SystemClock* const clock_;
  const Status* const status_;
  const GetContext* const get_context_;
  const SequenceNumber* const max_covering_tombstone_seq_;
  const KeyLookupProbes* const probes_;
  const KeyLookupBlockAccesses* const blocks_;
};

}  // namespace ROCKSDB_NAMESPACE
