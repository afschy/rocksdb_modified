//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "trace_replay/key_lookup_tracer.h"

#include <algorithm>
#include <string>
#include <vector>

#include "db/db_test_util.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/key_lookup_trace_options.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "table/block_based/block_based_table_reader.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "util/compression.h"
#include "util/string_util.h"
#include "utilities/merge_operators.h"

namespace ROCKSDB_NAMESPACE {

namespace {

// One data block access as parsed back out of a trace line.
struct ParsedBlock {
  uint64_t block_id = 0;
  uint64_t seq = 0;
  uint32_t read_bytes = 0;
  uint32_t uncomp_bytes = 0;
};

// One probe as parsed back out of a trace line.
struct ParsedProbe {
  uint32_t level = 0;
  uint64_t file_number = 0;
  int outcome = 0;
  std::vector<ParsedBlock> blocks;
};

// One "G" record as parsed back out of a trace line.
struct ParsedRecord {
  uint64_t seq = 0;
  uint64_t timestamp_us = 0;
  uint64_t lookup_id = 0;
  uint32_t cf_id = 0;
  int final_result = 0;
  size_t declared_num_probes = 0;
  std::vector<ParsedProbe> probes;
};

// One "F" file lifecycle record.
struct ParsedFileRecord {
  uint64_t seq = 0;
  uint64_t timestamp_us = 0;
  std::string op;
  uint32_t cf_id = 0;
  uint64_t file_number = 0;
  uint64_t num_entries = 0;
  uint64_t file_size = 0;
  uint32_t level = 0;
  uint32_t to_level = 0;
  bool has_to_level = false;
};

// One "A" iterator access record.
struct ParsedIterRecord {
  uint64_t seq = 0;
  uint64_t timestamp_us = 0;
  uint32_t cf_id = 0;
  uint32_t caller = 0;
  uint64_t iter_id = 0;
  uint32_t level = 0;
  uint64_t file_number = 0;
  bool no_insert = false;
  size_t declared_num_blocks = 0;
  std::vector<ParsedBlock> blocks;
};

// Every record of one trace, split by type but with the file-order sequence
// numbers kept together so ordering can be asserted across types.
struct ParsedTrace {
  std::vector<ParsedRecord> gets;
  std::vector<ParsedFileRecord> files;
  std::vector<ParsedIterRecord> iters;
  std::vector<uint64_t> seqs_in_file_order;
  bool truncated = false;
};

std::vector<std::string> SplitOn(const std::string& s, char sep) {
  std::vector<std::string> out;
  if (s.empty()) {
    return out;
  }
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
}

// Reads every line of a trace file, dropping the trailing empty piece.
std::vector<std::string> ReadTraceLines(Env* env, const std::string& path) {
  std::string contents;
  EXPECT_OK(ReadFileToString(env, path, &contents));
  std::vector<std::string> lines = SplitOn(contents, '\n');
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  return lines;
}

// Parses "<block_id>/<seq>/<read_bytes>/<uncomp_bytes>".
ParsedBlock ParseBlock(const std::string& s) {
  std::vector<std::string> parts = SplitOn(s, '/');
  EXPECT_EQ(4u, parts.size());
  ParsedBlock block;
  if (parts.size() != 4) {
    return block;
  }
  block.block_id = ParseUint64(parts[0]);
  block.seq = ParseUint64(parts[1]);
  block.read_bytes = static_cast<uint32_t>(ParseUint64(parts[2]));
  block.uncomp_bytes = static_cast<uint32_t>(ParseUint64(parts[3]));
  return block;
}

ParsedRecord ParseGetRecord(const std::vector<std::string>& fields) {
  ParsedRecord record;
  // The probe list is the last field and may be empty, in which case the line
  // ends with a comma and SplitOn yields only seven pieces.
  EXPECT_GE(fields.size(), 7u);
  EXPECT_LE(fields.size(), 8u);
  record.seq = ParseUint64(fields[1]);
  record.timestamp_us = ParseUint64(fields[2]);
  record.lookup_id = ParseUint64(fields[3]);
  record.cf_id = static_cast<uint32_t>(ParseUint64(fields[4]));
  record.final_result = atoi(fields[5].c_str());
  record.declared_num_probes = static_cast<size_t>(ParseUint64(fields[6]));
  if (fields.size() < 8 || fields[7].empty()) {
    return record;
  }
  for (const std::string& probe_str : SplitOn(fields[7], '|')) {
    std::vector<std::string> parts = SplitOn(probe_str, ':');
    EXPECT_GE(parts.size(), 3u);
    ParsedProbe probe;
    probe.level = static_cast<uint32_t>(ParseUint64(parts[0]));
    probe.file_number = ParseUint64(parts[1]);
    probe.outcome = atoi(parts[2].c_str());
    for (size_t i = 3; i < parts.size(); i++) {
      probe.blocks.push_back(ParseBlock(parts[i]));
    }
    record.probes.push_back(probe);
  }
  return record;
}

ParsedFileRecord ParseFileRecord(const std::vector<std::string>& fields) {
  ParsedFileRecord record;
  EXPECT_GE(fields.size(), 9u);
  EXPECT_LE(fields.size(), 10u);
  record.seq = ParseUint64(fields[1]);
  record.timestamp_us = ParseUint64(fields[2]);
  record.op = fields[3];
  record.cf_id = static_cast<uint32_t>(ParseUint64(fields[4]));
  record.file_number = ParseUint64(fields[5]);
  record.num_entries = ParseUint64(fields[6]);
  record.file_size = ParseUint64(fields[7]);
  record.level = static_cast<uint32_t>(ParseUint64(fields[8]));
  if (fields.size() == 10) {
    record.to_level = static_cast<uint32_t>(ParseUint64(fields[9]));
    record.has_to_level = true;
  }
  return record;
}

ParsedIterRecord ParseIterRecord(const std::vector<std::string>& fields) {
  ParsedIterRecord record;
  EXPECT_GE(fields.size(), 10u);
  EXPECT_LE(fields.size(), 11u);
  record.seq = ParseUint64(fields[1]);
  record.timestamp_us = ParseUint64(fields[2]);
  record.cf_id = static_cast<uint32_t>(ParseUint64(fields[3]));
  record.caller = static_cast<uint32_t>(ParseUint64(fields[4]));
  record.iter_id = ParseUint64(fields[5]);
  record.level = static_cast<uint32_t>(ParseUint64(fields[6]));
  record.file_number = ParseUint64(fields[7]);
  record.no_insert = fields[8] == "1";
  record.declared_num_blocks = static_cast<size_t>(ParseUint64(fields[9]));
  if (fields.size() < 11 || fields[10].empty()) {
    return record;
  }
  for (const std::string& block_str : SplitOn(fields[10], ':')) {
    record.blocks.push_back(ParseBlock(block_str));
  }
  return record;
}

// Parses every record line, skipping "#"-prefixed metadata lines, and checks
// that each record's declared element count matches what it carries.
ParsedTrace ParseTrace(const std::vector<std::string>& lines) {
  ParsedTrace trace;
  for (const std::string& line : lines) {
    if (line == "# truncated") {
      trace.truncated = true;
      continue;
    }
    if (!line.empty() && line[0] == '#') {
      continue;
    }
    std::vector<std::string> fields = SplitOn(line, ',');
    EXPECT_FALSE(fields.empty());
    if (fields.empty()) {
      continue;
    }
    if (fields[0] == "G") {
      ParsedRecord record = ParseGetRecord(fields);
      EXPECT_EQ(record.declared_num_probes, record.probes.size());
      trace.seqs_in_file_order.push_back(record.seq);
      trace.gets.push_back(record);
    } else if (fields[0] == "F") {
      ParsedFileRecord record = ParseFileRecord(fields);
      trace.seqs_in_file_order.push_back(record.seq);
      trace.files.push_back(record);
    } else if (fields[0] == "A") {
      ParsedIterRecord record = ParseIterRecord(fields);
      EXPECT_EQ(record.declared_num_blocks, record.blocks.size());
      trace.seqs_in_file_order.push_back(record.seq);
      trace.iters.push_back(record);
    } else {
      ADD_FAILURE() << "unknown record type: " << line;
    }
  }
  return trace;
}

std::vector<ParsedRecord> ParseRecords(const std::vector<std::string>& lines) {
  return ParseTrace(lines).gets;
}

// Decompresses a trace file written as concatenated zstd frames.
std::string DecompressTraceFile(Env* env, const std::string& path) {
  std::string compressed;
  EXPECT_OK(ReadFileToString(env, path, &compressed));
  constexpr uint32_t kCompressionFormatVersion = 2;
  constexpr size_t kChunk = 64 * 1024;
  auto uncompress =
      StreamingUncompress::Create(kZSTD, kCompressionFormatVersion, kChunk);
  EXPECT_NE(nullptr, uncompress);
  if (uncompress == nullptr) {
    return "";
  }
  std::string out;
  std::unique_ptr<char[]> buf(new char[kChunk]);
  const char* input = compressed.data();
  int ret = 0;
  size_t output_pos = 0;
  do {
    ret = uncompress->Uncompress(input, compressed.size(), buf.get(),
                                 &output_pos);
    // Continuing with the same input buffer requires passing nullptr.
    input = nullptr;
    EXPECT_GE(ret, 0);
    if (ret < 0) {
      break;
    }
    if (output_pos > 0) {
      out.append(buf.get(), output_pos);
    }
  } while (ret > 0 || output_pos == kChunk);
  return out;
}

// Splits already-decompressed trace text into lines.
std::vector<std::string> SplitTraceText(const std::string& contents) {
  std::vector<std::string> lines = SplitOn(contents, '\n');
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  return lines;
}

int Outcome(KeyLookupOutcome outcome) { return static_cast<int>(outcome); }
int Result(KeyLookupResult result) { return static_cast<int>(result); }

}  // namespace

// Format-level tests that exercise the tracer without a DB.
class KeyLookupTracerTest : public testing::Test {
 public:
  KeyLookupTracerTest() {
    test_path_ = test::PerThreadDBPath("key_lookup_tracer_test");
    env_ = Env::Default();
    EXPECT_OK(env_->CreateDirIfMissing(test_path_));
    trace_file_path_ = test_path_ + "/key_lookup_trace";
  }

  ~KeyLookupTracerTest() override {
    EXPECT_OK(DestroyDir(env_, test_path_));
  }

  // Starts a trace, hands the tracer to `body`, then stops it and returns the
  // resulting file's lines.
  template <typename Body>
  std::vector<std::string> Trace(const KeyLookupTraceOptions& options,
                                 Body body) {
    KeyLookupTracer tracer;
    EXPECT_OK(tracer.StartTrace(options, trace_file_path_, env_));
    body(&tracer);
    tracer.EndTrace();
    return ReadTraceLines(env_, trace_file_path_);
  }

  // Writes one record with the given probes and block accesses. The sequence
  // number is fixed rather than drawn from the tracer so that format
  // assertions stay exact.
  static void WriteOne(KeyLookupTracer* tracer, KeyLookupResult final_result,
                       const KeyLookupProbes& probes,
                       const KeyLookupBlockAccesses& blocks) {
    ASSERT_OK(tracer->WriteLookup(/*seq=*/9, /*timestamp_us=*/12345,
                                  /*lookup_id=*/tracer->NextLookupId(),
                                  /*cf_id=*/7, final_result, probes, blocks));
  }

  std::string test_path_;
  std::string trace_file_path_;
  Env* env_;
};

TEST_F(KeyLookupTracerTest, HeaderLine) {
  KeyLookupTraceOptions options;
  options.block_id_mode = KeyLookupBlockIdMode::kOrdinal;
  std::vector<std::string> lines = Trace(options, [](KeyLookupTracer*) {});
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(0u, lines[0].find(
                    "# rocksdb_key_lookup_trace v3 block_id_mode=ordinal "
                    "size_unit=bytes rocksdb="));

  options.block_id_mode = KeyLookupBlockIdMode::kOffset;
  lines = Trace(options, [](KeyLookupTracer*) {});
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(0u, lines[0].find(
                    "# rocksdb_key_lookup_trace v3 block_id_mode=offset "
                    "size_unit=bytes rocksdb="));
}

TEST_F(KeyLookupTracerTest, RecordWithNoProbes) {
  std::vector<std::string> lines =
      Trace(KeyLookupTraceOptions(), [](KeyLookupTracer* tracer) {
        WriteOne(tracer, KeyLookupResult::kNotFound, KeyLookupProbes(),
                 KeyLookupBlockAccesses());
      });
  ASSERT_EQ(2u, lines.size());
  // The probe list is empty, so the line ends with a comma.
  ASSERT_EQ("G,9,12345,1,7,0,0,", lines[1]);
}

TEST_F(KeyLookupTracerTest, ProbeWithNoBlocks) {
  std::vector<std::string> lines =
      Trace(KeyLookupTraceOptions(), [](KeyLookupTracer* tracer) {
        KeyLookupProbes probes;
        probes.push_back({3, 1201, KeyLookupOutcome::kNotFound, 0, 0});
        WriteOne(tracer, KeyLookupResult::kNotFound, probes,
                 KeyLookupBlockAccesses());
      });
  ASSERT_EQ(2u, lines.size());
  // Exactly three colon-separated fields, no trailing colon.
  ASSERT_EQ("G,9,12345,1,7,0,1,3:1201:0", lines[1]);
}

TEST_F(KeyLookupTracerTest, ProbesWithSeveralBlocks) {
  std::vector<std::string> lines =
      Trace(KeyLookupTraceOptions(), [](KeyLookupTracer* tracer) {
        KeyLookupProbes probes;
        probes.push_back({0, 1201, KeyLookupOutcome::kNotFound, 0, 0});
        probes.push_back({1, 1150, KeyLookupOutcome::kNotFound, 0, 2});
        probes.push_back({2, 1043, KeyLookupOutcome::kFoundValue, 2, 1});
        KeyLookupBlockAccesses blocks;
        blocks.push_back({4, 100, 4101, 8192});
        blocks.push_back({5, 101, 4101, 8192});
        blocks.push_back({12, 102, 2048, 4096});
        WriteOne(tracer, KeyLookupResult::kFound, probes, blocks);
      });
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(
      "G,9,12345,1,7,1,3,"
      "0:1201:0|1:1150:0:4/100/4101/8192:5/101/4101/8192|2:1043:1:12/102/2048/"
      "4096",
      lines[1]);
}

TEST_F(KeyLookupTracerTest, AllOutcomeAndResultValues) {
  const KeyLookupOutcome kOutcomes[] = {
      KeyLookupOutcome::kNotFound, KeyLookupOutcome::kFoundValue,
      KeyLookupOutcome::kFoundTombstone, KeyLookupOutcome::kFoundMergeOperand,
      KeyLookupOutcome::kError};
  const KeyLookupResult kResults[] = {
      KeyLookupResult::kNotFound, KeyLookupResult::kFound,
      KeyLookupResult::kDeleted, KeyLookupResult::kError,
      KeyLookupResult::kRangeDeleted};
  ASSERT_EQ(sizeof(kOutcomes) / sizeof(kOutcomes[0]),
            sizeof(kResults) / sizeof(kResults[0]));

  std::vector<std::string> lines =
      Trace(KeyLookupTraceOptions(), [&](KeyLookupTracer* tracer) {
        for (size_t i = 0; i < 5; i++) {
          KeyLookupProbes probes;
          probes.push_back(
              {static_cast<uint32_t>(i), 100 + i, kOutcomes[i], 0, 0});
          WriteOne(tracer, kResults[i], probes, KeyLookupBlockAccesses());
        }
      });
  std::vector<ParsedRecord> records = ParseRecords(lines);
  ASSERT_EQ(5u, records.size());
  for (size_t i = 0; i < 5; i++) {
    ASSERT_EQ(Result(kResults[i]), records[i].final_result);
    ASSERT_EQ(1u, records[i].probes.size());
    ASSERT_EQ(Outcome(kOutcomes[i]), records[i].probes[0].outcome);
    ASSERT_EQ(static_cast<uint32_t>(i), records[i].probes[0].level);
    ASSERT_EQ(100 + i, records[i].probes[0].file_number);
  }
}

TEST_F(KeyLookupTracerTest, TruncationAppendsMarker) {
  KeyLookupTraceOptions options;
  // Enough for the header and a couple of records, but not for 100 of them.
  options.max_trace_file_size = 100;
  std::vector<std::string> lines = Trace(options, [](KeyLookupTracer* tracer) {
    for (int i = 0; i < 100; i++) {
      WriteOne(tracer, KeyLookupResult::kNotFound, KeyLookupProbes(),
               KeyLookupBlockAccesses());
    }
  });
  ASSERT_GE(lines.size(), 2u);
  ASSERT_EQ("# truncated", lines.back());
  // The marker is written exactly once, and nothing follows it.
  ASSERT_EQ(1, std::count(lines.begin(), lines.end(), std::string("# truncated")));
}

TEST_F(KeyLookupTracerTest, Sampling) {
  KeyLookupTraceOptions options;
  options.sampling_frequency = 4;
  KeyLookupTracer tracer;
  ASSERT_OK(tracer.StartTrace(options, trace_file_path_, env_));
  int sampled = 0;
  for (int i = 0; i < 400; i++) {
    if (tracer.NextLookupId() != KeyLookupTracer::kReservedLookupId) {
      sampled++;
    }
  }
  tracer.EndTrace();
  ASSERT_EQ(100, sampled);
}

TEST_F(KeyLookupTracerTest, NotTracingReturnsReservedId) {
  KeyLookupTracer tracer;
  ASSERT_FALSE(tracer.is_tracing_enabled());
  ASSERT_EQ(KeyLookupTracer::kReservedLookupId, tracer.NextLookupId());
  // Writing while stopped is a no-op rather than an error.
  ASSERT_OK(tracer.WriteLookup(1, 1, 1, 0, KeyLookupResult::kNotFound,
                               KeyLookupProbes(), KeyLookupBlockAccesses()));
}

TEST_F(KeyLookupTracerTest, FileLifecycleFormat) {
  std::vector<std::string> lines =
      Trace(KeyLookupTraceOptions(), [](KeyLookupTracer* tracer) {
        ASSERT_OK(tracer->WriteFileLifecycle(111, KeyLookupFileOp::kCreate, 2,
                                             1201, 5000, 4194304, 0, 0));
        ASSERT_OK(tracer->WriteFileLifecycle(222, KeyLookupFileOp::kDelete, 2,
                                             1150, 4211, 3211264, 3, 0));
        // to_level is written only for a move.
        ASSERT_OK(tracer->WriteFileLifecycle(333, KeyLookupFileOp::kMove, 2,
                                             1043, 90, 65536, 1, 2));
      });
  ASSERT_EQ(4u, lines.size());
  ASSERT_EQ("F,1,111,create,2,1201,5000,4194304,0", lines[1]);
  ASSERT_EQ("F,2,222,delete,2,1150,4211,3211264,3", lines[2]);
  ASSERT_EQ("F,3,333,move,2,1043,90,65536,1,2", lines[3]);
}

TEST_F(KeyLookupTracerTest, IteratorAccessFormat) {
  std::vector<std::string> lines =
      Trace(KeyLookupTraceOptions(), [](KeyLookupTracer* tracer) {
        KeyLookupIterBlocks blocks;
        blocks.push_back({7, 40, 4101, 8192});
        blocks.push_back({8, 41, 4101, 8192});
        ASSERT_OK(tracer->WriteIteratorAccess(
            /*seq=*/5, /*timestamp_us=*/999, /*cf_id=*/0, /*caller=*/10,
            /*iter_id=*/3, /*level=*/2, /*file_number=*/1201,
            /*no_insert=*/true, blocks));
        // An empty block list still ends the line with a comma.
        ASSERT_OK(tracer->WriteIteratorAccess(
            /*seq=*/6, /*timestamp_us=*/999, /*cf_id=*/0, /*caller=*/3,
            /*iter_id=*/4, /*level=*/0, /*file_number=*/1202,
            /*no_insert=*/false, KeyLookupIterBlocks()));
      });
  ASSERT_EQ(3u, lines.size());
  ASSERT_EQ("A,5,999,0,10,3,2,1201,1,2,7/40/4101/8192:8/41/4101/8192", lines[1]);
  ASSERT_EQ("A,6,999,0,3,4,0,1202,0,0,", lines[2]);
}

TEST_F(KeyLookupTracerTest, ShouldTraceIteratorHonorsMask) {
  KeyLookupTraceOptions options;
  options.iterator_caller_mask =
      static_cast<uint16_t>(~(1u << TableReaderCaller::kCompaction));
  KeyLookupTracer tracer;
  ASSERT_OK(tracer.StartTrace(options, trace_file_path_, env_));
  ASSERT_FALSE(tracer.ShouldTraceIterator(TableReaderCaller::kCompaction));
  ASSERT_TRUE(tracer.ShouldTraceIterator(TableReaderCaller::kUserIterator));
  tracer.EndTrace();

  // The master switch overrides the mask.
  options.record_iterator_accesses = false;
  options.iterator_caller_mask = 0xFFFF;
  ASSERT_OK(tracer.StartTrace(options, trace_file_path_, env_));
  ASSERT_FALSE(tracer.ShouldTraceIterator(TableReaderCaller::kUserIterator));
  tracer.EndTrace();
}

TEST_F(KeyLookupTracerTest, ZstdCompressionRoundTrips) {
  if (!StreamingCompressionTypeSupported(kZSTD)) {
    ROCKSDB_GTEST_SKIP("Test requires zstd support");
    return;
  }
  // The same workload traced twice, once plain and once compressed.
  auto write_records = [](KeyLookupTracer* tracer) {
    for (int i = 0; i < 5000; i++) {
      KeyLookupProbes probes;
      probes.push_back({1, 1150, KeyLookupOutcome::kNotFound, 0, 1});
      KeyLookupBlockAccesses blocks;
      blocks.push_back({static_cast<uint64_t>(i), static_cast<uint64_t>(i),
                        4101, 8192});
      WriteOne(tracer, KeyLookupResult::kNotFound, probes, blocks);
    }
  };

  std::vector<std::string> plain_lines =
      Trace(KeyLookupTraceOptions(), write_records);
  uint64_t plain_size = 0;
  ASSERT_OK(env_->GetFileSize(trace_file_path_, &plain_size));

  KeyLookupTraceOptions options;
  options.compression = kZSTD;
  options.compression_level = 1;
  Trace(options, write_records);
  uint64_t compressed_size = 0;
  ASSERT_OK(env_->GetFileSize(trace_file_path_, &compressed_size));

  // A zstd frame starts with the magic number 0xFD2FB528, little endian.
  std::string raw;
  ASSERT_OK(ReadFileToString(env_, trace_file_path_, &raw));
  ASSERT_GE(raw.size(), 4u);
  ASSERT_EQ('\x28', raw[0]);
  ASSERT_EQ('\xb5', raw[1]);
  ASSERT_EQ('\x2f', raw[2]);
  ASSERT_EQ('\xfd', raw[3]);

  // Decompressing reproduces the plain trace exactly, header included.
  std::vector<std::string> decoded =
      SplitTraceText(DecompressTraceFile(env_, trace_file_path_));
  ASSERT_EQ(plain_lines, decoded);

  // This payload is highly redundant, so the ratio should be large. A ratio
  // near 1 means records are being compressed one at a time rather than in
  // batches, which is the mistake that makes compression pointless here.
  ASSERT_GT(plain_size, compressed_size * 3);
}

TEST_F(KeyLookupTracerTest, ZstdCompressionSpansMultipleBatches) {
  if (!StreamingCompressionTypeSupported(kZSTD)) {
    ROCKSDB_GTEST_SKIP("Test requires zstd support");
    return;
  }
  // Records are staged and compressed one batch at a time, so a trace that
  // fits in a single batch exercises only the first flush. This writes enough
  // to span many batches, which is what catches a compressor that mistakes a
  // reused staging buffer for input it has already consumed and silently drops
  // every batch after the first.
  auto write_records = [](KeyLookupTracer* tracer) {
    for (int i = 0; i < 60000; i++) {
      KeyLookupProbes probes;
      probes.push_back({1, 1150, KeyLookupOutcome::kNotFound, 0, 1});
      KeyLookupBlockAccesses blocks;
      blocks.push_back({static_cast<uint64_t>(i), static_cast<uint64_t>(i),
                        4101, 8192});
      WriteOne(tracer, KeyLookupResult::kNotFound, probes, blocks);
    }
  };

  std::vector<std::string> plain_lines =
      Trace(KeyLookupTraceOptions(), write_records);
  uint64_t plain_size = 0;
  ASSERT_OK(env_->GetFileSize(trace_file_path_, &plain_size));
  // Guard the premise: the payload must be several batches, or this test
  // silently degrades into the single batch case above.
  ASSERT_GT(plain_size, 4 * 256 * 1024);

  KeyLookupTraceOptions options;
  options.compression = kZSTD;
  options.compression_level = 1;
  Trace(options, write_records);

  std::vector<std::string> decoded =
      SplitTraceText(DecompressTraceFile(env_, trace_file_path_));
  ASSERT_EQ(plain_lines.size(), decoded.size());
  ASSERT_EQ(plain_lines, decoded);
}

TEST_F(KeyLookupTracerTest, CompressionMustBeNoneOrZstd) {
  KeyLookupTraceOptions options;
  options.compression = kSnappyCompression;
  KeyLookupTracer tracer;
  ASSERT_TRUE(tracer.StartTrace(options, trace_file_path_, env_)
                  .IsInvalidArgument());
}

TEST_F(KeyLookupTracerTest, StartTwiceIsBusy) {
  KeyLookupTracer tracer;
  ASSERT_OK(tracer.StartTrace(KeyLookupTraceOptions(), trace_file_path_, env_));
  ASSERT_TRUE(
      tracer.StartTrace(KeyLookupTraceOptions(), trace_file_path_, env_)
          .IsBusy());
  tracer.EndTrace();
}

// DB-level tests that assert the exact probe sequence of a Get() against a
// deterministically constructed LSM.
class KeyLookupTraceDBTest : public DBTestBase {
 public:
  KeyLookupTraceDBTest()
      : DBTestBase("key_lookup_trace_db_test", /*env_do_fsync=*/false) {
    trace_file_path_ = dbname_ + "_key_lookup_trace";
  }

  // Options that make the LSM shape predictable: no automatic compaction, no
  // dynamic level sizing, and no compression.
  Options TraceOptions() {
    Options options = CurrentOptions();
    options.create_if_missing = true;
    options.disable_auto_compactions = true;
    options.level_compaction_dynamic_level_bytes = false;
    options.compression = kNoCompression;
    return options;
  }

  void StartTrace(const KeyLookupTraceOptions& trace_options) {
    ASSERT_OK(db_->StartKeyLookupTrace(trace_options, trace_file_path_));
  }

  void StartTrace() { StartTrace(KeyLookupTraceOptions()); }

  // Stops tracing and returns the point lookup records written since it
  // started.
  std::vector<ParsedRecord> EndTraceAndParse() {
    return EndTraceAndParseAll().gets;
  }

  // Stops tracing and returns every record written since it started.
  ParsedTrace EndTraceAndParseAll() {
    EXPECT_OK(db_->EndKeyLookupTrace());
    return ParseTrace(ReadTraceLines(env_, trace_file_path_));
  }

  // Traces exactly one Get() and returns its record.
  ParsedRecord TraceOneGet(const std::string& key,
                           const KeyLookupTraceOptions& trace_options,
                           std::string* value_out = nullptr) {
    StartTrace(trace_options);
    std::string value = Get(key);
    if (value_out != nullptr) {
      *value_out = value;
    }
    std::vector<ParsedRecord> records = EndTraceAndParse();
    EXPECT_EQ(1u, records.size());
    if (records.empty()) {
      return ParsedRecord();
    }
    return records[0];
  }

  ParsedRecord TraceOneGet(const std::string& key,
                           std::string* value_out = nullptr) {
    return TraceOneGet(key, KeyLookupTraceOptions(), value_out);
  }

  // Flushes the memtable and moves the resulting file down to `level`.
  void FlushToLevel(int level) {
    ASSERT_OK(Flush());
    if (level > 0) {
      MoveFilesToLevel(level);
    }
    ASSERT_EQ(1, NumTableFilesAtLevel(level));
  }

  // Writes one file at `level` whose key range spans "a".."z", so that it is
  // always a candidate for a Get("k"). `middle` writes whatever this
  // particular file should hold for "k", if anything.
  template <typename MiddleFn>
  void FlushSpanningFile(int level, const std::string& tag, MiddleFn middle) {
    ASSERT_OK(Put("a", tag));
    middle();
    ASSERT_OK(Put("z", tag));
    FlushToLevel(level);
  }

  // Same, for a file that spans "k" but holds no entry for it.
  void FlushSpanningFile(int level, const std::string& tag) {
    FlushSpanningFile(level, tag, [] {});
  }

  // File numbers of the SST files currently at `level`.
  std::vector<uint64_t> FileNumbersAtLevel(int level) {
    ColumnFamilyMetaData meta;
    db_->GetColumnFamilyMetaData(&meta);
    std::vector<uint64_t> numbers;
    for (const auto& level_meta : meta.levels) {
      if (level_meta.level != level) {
        continue;
      }
      for (const auto& file : level_meta.files) {
        numbers.push_back(file.file_number);
      }
    }
    return numbers;
  }

  std::string trace_file_path_;
};

TEST_F(KeyLookupTraceDBTest, FoundInBottomLevel) {
  Options options = TraceOptions();
  DestroyAndReopen(options);

  FlushSpanningFile(2, "1", [&] { ASSERT_OK(Put("k", "target")); });
  FlushSpanningFile(1, "2");
  FlushSpanningFile(0, "3");

  std::string value;
  ParsedRecord record = TraceOneGet("k", &value);
  ASSERT_EQ("target", value);
  ASSERT_EQ(Result(KeyLookupResult::kFound), record.final_result);
  ASSERT_EQ(3u, record.probes.size());
  ASSERT_EQ(0u, record.probes[0].level);
  ASSERT_EQ(1u, record.probes[1].level);
  ASSERT_EQ(2u, record.probes[2].level);
  ASSERT_EQ(Outcome(KeyLookupOutcome::kNotFound), record.probes[0].outcome);
  ASSERT_EQ(Outcome(KeyLookupOutcome::kNotFound), record.probes[1].outcome);
  ASSERT_EQ(Outcome(KeyLookupOutcome::kFoundValue), record.probes[2].outcome);
  // Without a filter policy every probed file reads a data block.
  for (const ParsedProbe& probe : record.probes) {
    ASSERT_EQ(1u, probe.blocks.size());
  }
  // The recorded file numbers are the files actually in those levels.
  for (const ParsedProbe& probe : record.probes) {
    std::vector<uint64_t> numbers =
        FileNumbersAtLevel(static_cast<int>(probe.level));
    ASSERT_EQ(1u, numbers.size());
    ASSERT_EQ(numbers[0], probe.file_number);
  }
}

TEST_F(KeyLookupTraceDBTest, TombstoneStopsTheSearch) {
  Options options = TraceOptions();
  DestroyAndReopen(options);

  FlushSpanningFile(2, "1", [&] { ASSERT_OK(Put("k", "target")); });
  FlushSpanningFile(0, "2", [&] { ASSERT_OK(Delete("k")); });

  std::string value;
  ParsedRecord record = TraceOneGet("k", &value);
  ASSERT_EQ("NOT_FOUND", value);
  ASSERT_EQ(Result(KeyLookupResult::kDeleted), record.final_result);
  // The search stops at the tombstone, so the L2 file is never probed.
  ASSERT_EQ(1u, record.probes.size());
  ASSERT_EQ(0u, record.probes[0].level);
  ASSERT_EQ(Outcome(KeyLookupOutcome::kFoundTombstone),
            record.probes[0].outcome);
}

// Regression test for the cumulative nature of GetContext::State(): once a
// merge operand is found the state stays kMerge, so a later file that
// contributes nothing must still be reported as not_found.
TEST_F(KeyLookupTraceDBTest, MergeOperandsAcrossFiles) {
  Options options = TraceOptions();
  options.merge_operator = MergeOperators::CreateStringAppendOperator();
  DestroyAndReopen(options);

  FlushSpanningFile(3, "1", [&] { ASSERT_OK(Put("k", "base")); });
  FlushSpanningFile(2, "2", [&] { ASSERT_OK(Merge("k", "m1")); });
  // Spans "k" but holds no entry for it.
  FlushSpanningFile(1, "3");
  FlushSpanningFile(0, "4", [&] { ASSERT_OK(Merge("k", "m2")); });

  std::string value;
  ParsedRecord record = TraceOneGet("k", &value);
  ASSERT_EQ("base,m1,m2", value);
  ASSERT_EQ(Result(KeyLookupResult::kFound), record.final_result);
  ASSERT_EQ(4u, record.probes.size());
  ASSERT_EQ(Outcome(KeyLookupOutcome::kFoundMergeOperand),
            record.probes[0].outcome);
  // The file that contributed nothing, probed after a merge operand was seen.
  ASSERT_EQ(Outcome(KeyLookupOutcome::kNotFound), record.probes[1].outcome);
  ASSERT_EQ(Outcome(KeyLookupOutcome::kFoundMergeOperand),
            record.probes[2].outcome);
  ASSERT_EQ(Outcome(KeyLookupOutcome::kFoundValue), record.probes[3].outcome);
}

TEST_F(KeyLookupTraceDBTest, FilterNegativeProbesReadNoBlocks) {
  Options options = TraceOptions();
  BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
  table_options.whole_key_filtering = true;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  DestroyAndReopen(options);

  // "absent" falls inside every file's key range but is in none of them.
  FlushSpanningFile(2, "1");
  FlushSpanningFile(1, "2");
  FlushSpanningFile(0, "3");

  std::string value;
  ParsedRecord record = TraceOneGet("absent", &value);
  ASSERT_EQ("NOT_FOUND", value);
  ASSERT_EQ(Result(KeyLookupResult::kNotFound), record.final_result);
  ASSERT_EQ(3u, record.probes.size());
  for (const ParsedProbe& probe : record.probes) {
    ASSERT_EQ(Outcome(KeyLookupOutcome::kNotFound), probe.outcome);
    // The filter excluded the file, so no data block was read.
    ASSERT_EQ(0u, probe.blocks.size());
  }
}

TEST_F(KeyLookupTraceDBTest, MemtableHitIsNotTraced) {
  Options options = TraceOptions();
  DestroyAndReopen(options);

  FlushSpanningFile(0, "1");
  // Served by the memtable, so Version::Get() is never reached.
  ASSERT_OK(Put("k", "in_memtable"));

  StartTrace();
  ASSERT_EQ("in_memtable", Get("k"));
  ASSERT_TRUE(EndTraceAndParse().empty());
}

TEST_F(KeyLookupTraceDBTest, NoCandidateFilesGivesZeroProbes) {
  Options options = TraceOptions();
  DestroyAndReopen(options);

  // More than one non-empty level, so that FilePicker applies its key range
  // check rather than taking the "few files, just search them" shortcut.
  ASSERT_OK(Put("b", "1"));
  ASSERT_OK(Put("c", "1"));
  FlushToLevel(2);
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(Put("c", "2"));
  FlushToLevel(0);

  // Outside every file's key range, so the file picker returns nothing.
  std::string value;
  ParsedRecord record = TraceOneGet("zzz", &value);
  ASSERT_EQ("NOT_FOUND", value);
  ASSERT_EQ(Result(KeyLookupResult::kNotFound), record.final_result);
  ASSERT_EQ(0u, record.probes.size());
  ASSERT_EQ(0u, record.declared_num_probes);
}

TEST_F(KeyLookupTraceDBTest, RangeTombstoneStopsTheSearch) {
  Options options = TraceOptions();
  DestroyAndReopen(options);

  FlushSpanningFile(2, "1", [&] { ASSERT_OK(Put("k", "target")); });
  FlushSpanningFile(0, "2", [&] {
    ASSERT_OK(db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(), "k",
                               "l"));
  });

  std::string value;
  ParsedRecord record = TraceOneGet("k", &value);
  ASSERT_EQ("NOT_FOUND", value);
  ASSERT_EQ(Result(KeyLookupResult::kRangeDeleted), record.final_result);
  // Only the L0 file carrying the range tombstone is probed; the loop then
  // stops before searching L2.
  ASSERT_EQ(1u, record.probes.size());
  ASSERT_EQ(0u, record.probes[0].level);
}

// Builds a single L0 file with many small data blocks, so that different keys
// land in different blocks.
class KeyLookupTraceBlockIdTest : public KeyLookupTraceDBTest {
 public:
  static constexpr int kNumKeys = 200;

  static std::string KeyAt(int i) { return "key" + std::to_string(100000 + i); }

  void BuildManyBlockFile() {
    Options options = TraceOptions();
    BlockBasedTableOptions table_options;
    // Small blocks so that 200 keys span many of them.
    table_options.block_size = 128;
    table_options.filter_policy.reset();
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    DestroyAndReopen(options);

    for (int i = 0; i < kNumKeys; i++) {
      ASSERT_OK(Put(KeyAt(i), std::string(64, 'v')));
    }
    FlushToLevel(0);
  }

  // The single block id recorded for a Get() of `key`.
  uint64_t BlockIdFor(const std::string& key, KeyLookupBlockIdMode mode) {
    KeyLookupTraceOptions trace_options;
    trace_options.block_id_mode = mode;
    ParsedRecord record = TraceOneGet(key, trace_options);
    EXPECT_EQ(1u, record.probes.size());
    if (record.probes.empty() || record.probes[0].blocks.size() != 1) {
      EXPECT_EQ(1u, record.probes.empty() ? 0 : record.probes[0].blocks.size());
      return std::numeric_limits<uint64_t>::max();
    }
    return record.probes[0].blocks[0].block_id;
  }

  // The single block access recorded for a Get() of `key`.
  ParsedBlock BlockAccessFor(const std::string& key,
                             KeyLookupBlockIdMode mode) {
    KeyLookupTraceOptions trace_options;
    trace_options.block_id_mode = mode;
    ParsedRecord record = TraceOneGet(key, trace_options);
    EXPECT_EQ(1u, record.probes.size());
    if (record.probes.empty() || record.probes[0].blocks.size() != 1) {
      return ParsedBlock();
    }
    return record.probes[0].blocks[0];
  }
};

TEST_F(KeyLookupTraceBlockIdTest, Ordinals) {
  BuildManyBlockFile();

  // The first key lives in the first data block.
  ASSERT_EQ(0u, BlockIdFor(KeyAt(0), KeyLookupBlockIdMode::kOrdinal));

  // Ordinals increase with key order and stay within the block count.
  uint64_t previous = 0;
  uint64_t distinct = 1;
  for (int i = 1; i < kNumKeys; i++) {
    uint64_t ordinal = BlockIdFor(KeyAt(i), KeyLookupBlockIdMode::kOrdinal);
    ASSERT_NE(std::numeric_limits<uint64_t>::max(), ordinal);
    ASSERT_GE(ordinal, previous);
    if (ordinal != previous) {
      distinct++;
    }
    previous = ordinal;
  }
  // With 128-byte blocks and 200 keys of ~70 bytes there is more than one.
  ASSERT_GT(distinct, 1u);
  // Ordinals are dense: the last one is one less than the number of blocks
  // the keys covered.
  ASSERT_EQ(distinct - 1, previous);
}

TEST_F(KeyLookupTraceBlockIdTest, Offsets) {
  BuildManyBlockFile();

  // In kOffset mode ids are byte offsets: still ascending in key order, and
  // the first data block starts at offset 0.
  ASSERT_EQ(0u, BlockIdFor(KeyAt(0), KeyLookupBlockIdMode::kOffset));

  std::vector<uint64_t> offsets;
  for (int i = 0; i < kNumKeys; i++) {
    uint64_t offset = BlockIdFor(KeyAt(i), KeyLookupBlockIdMode::kOffset);
    ASSERT_NE(std::numeric_limits<uint64_t>::max(), offset);
    if (offsets.empty() || offsets.back() != offset) {
      offsets.push_back(offset);
    }
  }
  ASSERT_GT(offsets.size(), 1u);
  for (size_t i = 1; i < offsets.size(); i++) {
    ASSERT_GT(offsets[i], offsets[i - 1]);
  }
}

TEST_F(KeyLookupTraceBlockIdTest, RecordBlocksDisabled) {
  BuildManyBlockFile();

  KeyLookupTraceOptions trace_options;
  trace_options.record_blocks = false;
  ParsedRecord record = TraceOneGet(KeyAt(10), trace_options);
  ASSERT_EQ(1u, record.probes.size());
  ASSERT_EQ(Outcome(KeyLookupOutcome::kFoundValue), record.probes[0].outcome);
  // The file sequence is still recorded, but no blocks are.
  ASSERT_EQ(0u, record.probes[0].blocks.size());
}

TEST_F(KeyLookupTraceDBTest, SamplingSkipsRequests) {
  Options options = TraceOptions();
  DestroyAndReopen(options);

  FlushSpanningFile(0, "1", [&] { ASSERT_OK(Put("k", "target")); });

  KeyLookupTraceOptions trace_options;
  trace_options.sampling_frequency = 4;
  StartTrace(trace_options);
  for (int i = 0; i < 40; i++) {
    ASSERT_EQ("target", Get("k"));
  }
  ASSERT_EQ(10u, EndTraceAndParse().size());
}

TEST_F(KeyLookupTraceDBTest, BlockSizesRecordedWithoutCompression) {
  Options options = TraceOptions();
  options.compression = kNoCompression;
  DestroyAndReopen(options);
  FlushSpanningFile(1, "v1", [&] { ASSERT_OK(Put("k", "value")); });

  ParsedRecord record = TraceOneGet("k");
  ASSERT_EQ(1u, record.probes.size());
  ASSERT_EQ(1u, record.probes[0].blocks.size());
  const ParsedBlock& block = record.probes[0].blocks[0];
  ASSERT_GT(block.read_bytes, 0u);
  // With compression off the block on disk is the block in memory, and the
  // read additionally covers the 5-byte trailer.
  ASSERT_EQ(block.uncomp_bytes + BlockBasedTable::kBlockTrailerSize,
            block.read_bytes);
}

TEST_F(KeyLookupTraceDBTest, CompressedBlockReportsBothSizes) {
  if (!Snappy_Supported()) {
    ROCKSDB_GTEST_SKIP("Test requires snappy support");
    return;
  }
  Options options = TraceOptions();
  options.compression = kSnappyCompression;
  DestroyAndReopen(options);
  // Highly compressible payload, so the two sizes are clearly different.
  FlushSpanningFile(1, "v1", [&] {
    for (int i = 0; i < 200; i++) {
      ASSERT_OK(Put("k" + std::to_string(i), std::string(512, 'x')));
    }
  });

  ParsedRecord record = TraceOneGet("k100");
  ASSERT_EQ(1u, record.probes.size());
  ASSERT_EQ(1u, record.probes[0].blocks.size());
  const ParsedBlock& block = record.probes[0].blocks[0];
  // The cache footprint is the uncompressed size, which is what a capacity
  // simulation needs; the disk read is the compressed size.
  ASSERT_GT(block.uncomp_bytes, block.read_bytes);
}

TEST_F(KeyLookupTraceDBTest, SequenceNumbersAreDenseAndOrdered) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  FlushSpanningFile(2, "v2", [&] { ASSERT_OK(Put("k", "value")); });
  FlushSpanningFile(1, "v1");

  StartTrace();
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ("value", Get("k"));
  }
  ParsedTrace trace = EndTraceAndParseAll();
  ASSERT_EQ(5u, trace.gets.size());

  // Collect every sequence number the trace hands out, records and blocks
  // alike, and check they form 1..N with no gaps and no repeats.
  std::vector<uint64_t> seqs;
  for (const ParsedRecord& record : trace.gets) {
    seqs.push_back(record.seq);
    for (const ParsedProbe& probe : record.probes) {
      for (const ParsedBlock& block : probe.blocks) {
        seqs.push_back(block.seq);
      }
    }
  }
  std::sort(seqs.begin(), seqs.end());
  ASSERT_EQ(1u, seqs.front());
  for (size_t i = 1; i < seqs.size(); i++) {
    ASSERT_EQ(seqs[i - 1] + 1, seqs[i]) << "gap or repeat at index " << i;
  }

  // Records appear in the file in sequence order.
  for (size_t i = 1; i < trace.gets.size(); i++) {
    ASSERT_GT(trace.gets[i].seq, trace.gets[i - 1].seq);
  }
}

TEST_F(KeyLookupTraceDBTest, RecordSeqPrecedesItsBlockSeqs) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  FlushSpanningFile(2, "v2", [&] { ASSERT_OK(Put("k", "value")); });

  ParsedRecord record = TraceOneGet("k");
  ASSERT_FALSE(record.probes.empty());
  bool saw_block = false;
  for (const ParsedProbe& probe : record.probes) {
    for (const ParsedBlock& block : probe.blocks) {
      // The record's seq is taken at lookup start, so sorting by seq puts the
      // lookup ahead of the blocks it caused.
      ASSERT_LT(record.seq, block.seq);
      saw_block = true;
    }
  }
  ASSERT_TRUE(saw_block);
}

TEST_F(KeyLookupTraceDBTest, LookupReadingNoBlocksStillGetsSeq) {
  Options options = TraceOptions();
  BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
  table_options.cache_index_and_filter_blocks = false;
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  DestroyAndReopen(options);
  // A file spanning "k" whose filter excludes it: probed, but no block read.
  FlushSpanningFile(1, "v1");

  ParsedRecord record = TraceOneGet("k");
  ASSERT_EQ(1u, record.probes.size());
  ASSERT_TRUE(record.probes[0].blocks.empty());
  // The lookup still occupies a position in the trace-wide order, which is
  // what makes filter effectiveness measurable.
  ASSERT_GT(record.seq, 0u);
}

TEST_F(KeyLookupTraceDBTest, FlushEmitsCreateRecord) {
  Options options = TraceOptions();
  DestroyAndReopen(options);

  StartTrace();
  ASSERT_OK(Put("a", "v"));
  ASSERT_OK(Flush());
  ParsedTrace trace = EndTraceAndParseAll();

  ASSERT_EQ(1u, trace.files.size());
  ASSERT_EQ("create", trace.files[0].op);
  ASSERT_EQ(0u, trace.files[0].level);
  ASSERT_FALSE(trace.files[0].has_to_level);
  ASSERT_GT(trace.files[0].seq, 0u);
  // One Put flushed, so one entry, in a file that is not empty.
  ASSERT_EQ(1u, trace.files[0].num_entries);
  ASSERT_GT(trace.files[0].file_size, 0u);
  std::vector<uint64_t> at_l0 = FileNumbersAtLevel(0);
  ASSERT_EQ(1u, at_l0.size());
  ASSERT_EQ(at_l0[0], trace.files[0].file_number);
}

TEST_F(KeyLookupTraceDBTest, TrivialMoveEmitsMoveNotDeleteAndCreate) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  ASSERT_OK(Put("a", "v"));
  ASSERT_OK(Put("z", "v"));
  ASSERT_OK(Flush());
  ASSERT_EQ(1, NumTableFilesAtLevel(0));
  std::vector<uint64_t> before = FileNumbersAtLevel(0);
  ASSERT_EQ(1u, before.size());

  StartTrace();
  MoveFilesToLevel(2);
  ParsedTrace trace = EndTraceAndParseAll();

  // MoveFilesToLevel walks the file down one level at a time, so reaching L2
  // is two trivial moves. Each is relabeling, not rewriting, so the file keeps
  // its number and its blocks stay valid. Recording either as a delete plus a
  // create would make a cache simulator discard still-live blocks.
  ASSERT_EQ(2u, trace.files.size());
  uint32_t expected_from = 0;
  for (const ParsedFileRecord& record : trace.files) {
    ASSERT_EQ("move", record.op);
    ASSERT_EQ(before[0], record.file_number);
    ASSERT_EQ(expected_from, record.level);
    ASSERT_TRUE(record.has_to_level);
    ASSERT_EQ(expected_from + 1, record.to_level);
    expected_from = record.to_level;
    // The file was relabeled, not rewritten, so its stats are those of the
    // two keys it was flushed with and are the same on both records.
    ASSERT_EQ(2u, record.num_entries);
    ASSERT_EQ(trace.files[0].file_size, record.file_size);
    ASSERT_GT(record.file_size, 0u);
  }
  ASSERT_EQ(2u, expected_from);
  // Same file number still live, at the new level.
  ASSERT_EQ(before, FileNumbersAtLevel(2));
}

TEST_F(KeyLookupTraceDBTest, RewritingCompactionEmitsDeleteAndCreate) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  // Two overlapping L0 files, so compacting them must rewrite rather than
  // trivially move.
  ASSERT_OK(Put("a", "v1"));
  ASSERT_OK(Put("m", "v1"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("a", "v2"));
  ASSERT_OK(Put("m", "v2"));
  ASSERT_OK(Flush());
  ASSERT_EQ(2, NumTableFilesAtLevel(0));
  std::vector<uint64_t> inputs = FileNumbersAtLevel(0);
  ASSERT_EQ(2u, inputs.size());

  StartTrace();
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  ParsedTrace trace = EndTraceAndParseAll();

  std::vector<uint64_t> deleted;
  std::vector<uint64_t> created;
  for (const ParsedFileRecord& record : trace.files) {
    ASSERT_NE("move", record.op);
    // A deleted file's stats come from the version it is dropped from, so a
    // delete carries them just as a create does.
    ASSERT_GT(record.num_entries, 0u);
    ASSERT_GT(record.file_size, 0u);
    if (record.op == "delete") {
      // Each input file holds the two keys it was flushed with.
      ASSERT_EQ(2u, record.num_entries);
      deleted.push_back(record.file_number);
    } else {
      created.push_back(record.file_number);
    }
  }
  std::sort(deleted.begin(), deleted.end());
  std::sort(inputs.begin(), inputs.end());
  ASSERT_EQ(inputs, deleted);
  ASSERT_FALSE(created.empty());
}

TEST_F(KeyLookupTraceDBTest, IteratorEmitsGroupedAccesses) {
  Options options = TraceOptions();
  BlockBasedTableOptions table_options;
  // Small blocks so one scan crosses several.
  table_options.block_size = 128;
  table_options.filter_policy.reset();
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  DestroyAndReopen(options);
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put("key" + std::to_string(1000 + i), std::string(64, 'v')));
  }
  FlushToLevel(1);
  std::vector<uint64_t> files = FileNumbersAtLevel(1);
  ASSERT_EQ(1u, files.size());

  StartTrace();
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    int count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      count++;
    }
    ASSERT_OK(it->status());
    ASSERT_EQ(200, count);
  }
  ParsedTrace trace = EndTraceAndParseAll();

  ASSERT_FALSE(trace.iters.empty());
  size_t total_blocks = 0;
  for (const ParsedIterRecord& record : trace.iters) {
    // One BlockBasedTableIterator serves one file, so every record of one
    // scan carries that file and one iter_id.
    ASSERT_EQ(files[0], record.file_number);
    ASSERT_EQ(TableReaderCaller::kUserIterator, record.caller);
    ASSERT_EQ(trace.iters[0].iter_id, record.iter_id);
    // A user iterator fills the cache, unlike compaction.
    ASSERT_FALSE(record.no_insert);
    ASSERT_FALSE(record.blocks.empty());
    for (const ParsedBlock& block : record.blocks) {
      ASSERT_GT(block.read_bytes, 0u);
      ASSERT_GT(block.uncomp_bytes, 0u);
      ASSERT_GT(block.seq, 0u);
    }
    total_blocks += record.blocks.size();
  }
  ASSERT_GT(total_blocks, 1u);
}

TEST_F(KeyLookupTraceDBTest, CompactionAccessesAreMarkedNoInsert) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  ASSERT_OK(Put("a", "v1"));
  ASSERT_OK(Put("m", "v1"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("a", "v2"));
  ASSERT_OK(Put("m", "v2"));
  ASSERT_OK(Flush());

  StartTrace();
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  ParsedTrace trace = EndTraceAndParseAll();

  bool saw_compaction = false;
  for (const ParsedIterRecord& record : trace.iters) {
    if (record.caller != TableReaderCaller::kCompaction) {
      continue;
    }
    saw_compaction = true;
    // Compaction sets fill_cache=false, so it looks up but never inserts.
    ASSERT_TRUE(record.no_insert);
  }
  ASSERT_TRUE(saw_compaction);
}

TEST_F(KeyLookupTraceDBTest, IteratorAccessesCanBeDisabled) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  for (int i = 0; i < 50; i++) {
    ASSERT_OK(Put("key" + std::to_string(1000 + i), "v"));
  }
  FlushToLevel(1);

  KeyLookupTraceOptions trace_options;
  trace_options.record_iterator_accesses = false;
  StartTrace(trace_options);
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
    }
    ASSERT_OK(it->status());
  }
  ParsedTrace trace = EndTraceAndParseAll();
  ASSERT_TRUE(trace.iters.empty());
}

TEST_F(KeyLookupTraceDBTest, IteratorCallerMaskExcludesCompaction) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  ASSERT_OK(Put("a", "v1"));
  ASSERT_OK(Put("m", "v1"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("a", "v2"));
  ASSERT_OK(Put("m", "v2"));
  ASSERT_OK(Flush());

  KeyLookupTraceOptions trace_options;
  trace_options.iterator_caller_mask =
      static_cast<uint16_t>(~(1u << TableReaderCaller::kCompaction));
  StartTrace(trace_options);
  ASSERT_OK(db_->CompactRange(CompactRangeOptions(), nullptr, nullptr));
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
    }
    ASSERT_OK(it->status());
  }
  ParsedTrace trace = EndTraceAndParseAll();

  bool saw_user_iterator = false;
  for (const ParsedIterRecord& record : trace.iters) {
    ASSERT_NE(TableReaderCaller::kCompaction, record.caller);
    if (record.caller == TableReaderCaller::kUserIterator) {
      saw_user_iterator = true;
    }
  }
  // Masking out compaction must not suppress user iterators.
  ASSERT_TRUE(saw_user_iterator);
}

TEST_F(KeyLookupTraceDBTest, IteratorLiveAcrossEndTraceDoesNotCrash) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  for (int i = 0; i < 50; i++) {
    ASSERT_OK(Put("key" + std::to_string(1000 + i), "v"));
  }
  FlushToLevel(1);

  StartTrace();
  std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  // Stop tracing while the iterator is still alive and holding buffered
  // accesses. Its destructor flush must tolerate the tracer being stopped.
  ASSERT_OK(db_->EndKeyLookupTrace());
  for (; it->Valid(); it->Next()) {
  }
  ASSERT_OK(it->status());
  it.reset();
}

TEST_F(KeyLookupTraceDBTest, LongScanEmitsContinuationRecords) {
  Options options = TraceOptions();
  BlockBasedTableOptions table_options;
  // Tiny blocks so one file holds well over kIterFlushThreshold of them.
  table_options.block_size = 64;
  table_options.filter_policy.reset();
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  DestroyAndReopen(options);
  const int kNumKeys =
      static_cast<int>(KeyLookupTracer::kIterFlushThreshold) * 2 + 100;
  for (int i = 0; i < kNumKeys; i++) {
    ASSERT_OK(Put("key" + std::to_string(100000 + i), std::string(48, 'v')));
  }
  FlushToLevel(1);

  StartTrace();
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    int count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      count++;
    }
    ASSERT_OK(it->status());
    ASSERT_EQ(kNumKeys, count);
  }
  ParsedTrace trace = EndTraceAndParseAll();

  // More blocks than the flush threshold means several records, all sharing
  // one iter_id so a reader can stitch them back together.
  ASSERT_GT(trace.iters.size(), 1u);
  const uint64_t iter_id = trace.iters[0].iter_id;
  size_t total_blocks = 0;
  uint64_t previous_block_seq = 0;
  for (size_t i = 0; i < trace.iters.size(); i++) {
    const ParsedIterRecord& record = trace.iters[i];
    ASSERT_EQ(iter_id, record.iter_id);
    // Every record except the last is a threshold-triggered flush.
    if (i + 1 < trace.iters.size()) {
      ASSERT_EQ(KeyLookupTracer::kIterFlushThreshold, record.blocks.size());
    }
    // Block sequence numbers stay ordered across the record boundary.
    for (const ParsedBlock& block : record.blocks) {
      ASSERT_GT(block.seq, previous_block_seq);
      previous_block_seq = block.seq;
    }
    total_blocks += record.blocks.size();
  }
  ASSERT_GT(total_blocks, KeyLookupTracer::kIterFlushThreshold);
}

TEST_F(KeyLookupTraceDBTest, ConcurrentIteratorsOverOneFileGetDistinctIds) {
  Options options = TraceOptions();
  BlockBasedTableOptions table_options;
  table_options.block_size = 128;
  table_options.filter_policy.reset();
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  DestroyAndReopen(options);
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put("key" + std::to_string(1000 + i), std::string(64, 'v')));
  }
  FlushToLevel(1);

  StartTrace();
  {
    // Two iterators alive at once over the same file. Without a per-iterator
    // id their records would be indistinguishable.
    std::unique_ptr<Iterator> a(db_->NewIterator(ReadOptions()));
    std::unique_ptr<Iterator> b(db_->NewIterator(ReadOptions()));
    for (a->SeekToFirst(); a->Valid(); a->Next()) {
    }
    for (b->SeekToFirst(); b->Valid(); b->Next()) {
    }
    ASSERT_OK(a->status());
    ASSERT_OK(b->status());
  }
  ParsedTrace trace = EndTraceAndParseAll();

  std::vector<uint64_t> ids;
  for (const ParsedIterRecord& record : trace.iters) {
    if (std::find(ids.begin(), ids.end(), record.iter_id) == ids.end()) {
      ids.push_back(record.iter_id);
    }
  }
  ASSERT_EQ(2u, ids.size());
}

TEST_F(KeyLookupTraceDBTest, StaleIteratorDoesNotWriteIntoNextSession) {
  Options options = TraceOptions();
  BlockBasedTableOptions table_options;
  table_options.block_size = 128;
  table_options.filter_policy.reset();
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  DestroyAndReopen(options);
  for (int i = 0; i < 200; i++) {
    ASSERT_OK(Put("key" + std::to_string(1000 + i), std::string(64, 'v')));
  }
  FlushToLevel(1);

  // An iterator created in one trace session, still holding buffered accesses
  // when that session ends.
  StartTrace();
  std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  ASSERT_OK(db_->EndKeyLookupTrace());

  // A second session resets the sequence and iterator id counters, so anything
  // the stale iterator still holds belongs to the previous session and must
  // not land here.
  StartTrace();
  ASSERT_EQ("NOT_FOUND", Get("nonexistent"));
  it.reset();
  ParsedTrace trace = EndTraceAndParseAll();

  ASSERT_TRUE(trace.iters.empty());
  // The second session's own numbering is intact: dense from 1.
  ASSERT_FALSE(trace.seqs_in_file_order.empty());
  std::vector<uint64_t> seqs = trace.seqs_in_file_order;
  std::sort(seqs.begin(), seqs.end());
  ASSERT_EQ(1u, seqs.front());
}

TEST_F(KeyLookupTraceDBTest, EndTraceWithoutStartIsOk) {
  Options options = TraceOptions();
  DestroyAndReopen(options);
  ASSERT_OK(db_->EndKeyLookupTrace());
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
