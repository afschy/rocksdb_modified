//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>

#include "rocksdb/rocksdb_namespace.h"
#include "util/autovector.h"

// The block access record shared between the key lookup tracer and the read
// paths that feed it. Kept separate from trace_replay/key_lookup_tracer.h so
// that table/get_context.h, which is included very widely, does not pull in
// the tracer and its mutex dependency.

namespace ROCKSDB_NAMESPACE {

// One data block access.
struct KeyLookupBlockAccess {
  // Ordinal or raw offset, per KeyLookupTraceOptions::block_id_mode.
  // UINT64_MAX when it could not be resolved.
  uint64_t block_id = 0;
  // Position of this access in the trace-wide sequence.
  uint64_t seq = 0;
  // Bytes read from the file, including the block trailer.
  uint32_t read_bytes = 0;
  // Size of the materialized block, i.e. its footprint in the block cache.
  // 0 when the block did not materialize.
  uint32_t uncomp_bytes = 0;
};

// Block accesses of one point lookup, across every file it searched.
//
// The inline capacity here is deliberately small. KeyLookupBlockAccess is 24
// bytes, and this buffer sits on the stack of every Version::Get whether that
// lookup is traced or not, so a large inline capacity would be paid for by
// untraced lookups too. A point lookup reads one to three data blocks in
// total, so spilling is rare.
using KeyLookupBlockAccesses = autovector<KeyLookupBlockAccess, 8>;

// Buffer for one table iterator's block accesses. Owned by the iterator and
// flushed both when it fills and when the iterator is destroyed, so its size
// bounds how much is lost if the process dies.
using KeyLookupIterBlocks = autovector<KeyLookupBlockAccess, 16>;

}  // namespace ROCKSDB_NAMESPACE
