//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "db/blob/blob_file_completion_callback.h"
#include "db/column_family.h"
#include "db/compaction/compaction_iterator.h"
#include "db/compaction/compaction_outputs.h"
#include "db/flush_scheduler.h"
#include "db/internal_stats.h"
#include "db/job_context.h"
#include "db/log_writer.h"
#include "db/memtable_list.h"
#include "db/range_del_aggregator.h"
#include "db/seqno_to_time_mapping.h"
#include "db/version_edit.h"
#include "db/write_controller.h"
#include "db/write_thread.h"
#include "logging/event_logger.h"
#include "options/cf_options.h"
#include "options/db_options.h"
#include "port/port.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/compaction_job_stats.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/transaction_log.h"
#include "util/autovector.h"
#include "util/stop_watch.h"
#include "util/thread_local.h"

namespace ROCKSDB_NAMESPACE {

class Arena;
class CompactionState;
class ErrorHandler;
class MemTable;
class SnapshotChecker;
class SystemClock;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;

class SubcompactionState;

// CompactionJob is responsible for executing the compaction. Each (manual or
// automated) compaction corresponds to a CompactionJob object, and usually
// goes through the stages of `Prepare()`->`Run()`->`Install()`. CompactionJob
// will divide the compaction into subcompactions and execute them in parallel
// if needed.
//
// CompactionJob has 2 main stats:
// 1. CompactionJobStats job_stats_
//    CompactionJobStats is a public data structure which is part of Compaction
//    event listener that rocksdb share the job stats with the user.
//    Internally it's an aggregation of all the compaction_job_stats from each
//    `SubcompactionState`:
//                                           +------------------------+
//                                           | SubcompactionState     |
//                                           |                        |
//                                +--------->|   compaction_job_stats |
//                                |          |                        |
//                                |          +------------------------+
// +------------------------+     |
// | CompactionJob          |     |          +------------------------+
// |                        |     |          | SubcompactionState     |
// |   job_stats            +-----+          |                        |
// |                        |     +--------->|   compaction_job_stats |
// |                        |     |          |                        |
// +------------------------+     |          +------------------------+
//                                |
//                                |          +------------------------+
//                                |          | SubcompactionState     |
//                                |          |                        |
//                                +--------->+   compaction_job_stats |
//                                |          |                        |
//                                |          +------------------------+
//                                |
//                                |          +------------------------+
//                                |          |       ...              |
//                                +--------->+                        |
//                                           +------------------------+
//
// 2. CompactionStatsFull internal_stats_
//    `CompactionStatsFull` is an internal stats about the compaction, which
//    is eventually sent to `ColumnFamilyData::internal_stats_` and used for
//    logging and public metrics.
//    Internally, it's an aggregation of stats_ from each `SubcompactionState`.
//    It has 2 parts, ordinary output level stats and the proximal level output
//    stats.
//                                                +---------------------------+
//                                                | SubcompactionState        |
//                                                |                           |
//                                                | +----------------------+  |
//                                                | | CompactionOutputs    |  |
//                                                | | (normal output)      |  |
//                                            +---->|   stats_             |  |
//                                            |   | +----------------------+  |
//                                            |   |                           |
//                                            |   | +----------------------+  |
// +--------------------------------+         |   | | CompactionOutputs    |  |
// | CompactionJob                  |         |   | | (proximal_level)     |  |
// |                                |    +--------->|   stats_             |  |
// |   internal_stats_              |    |    |   | +----------------------+  |
// |    +-------------------------+ |    |    |   |                           |
// |    |output_level_stats       |------|----+   +---------------------------+
// |    +-------------------------+ |    |    |
// |                                |    |    |
// |    +-------------------------+ |    |    |   +---------------------------+
// |    |proximal_level_stats     |------+    |   | SubcompactionState        |
// |    +-------------------------+ |    |    |   |                           |
// |                                |    |    |   | +----------------------+  |
// |                                |    |    |   | | CompactionOutputs    |  |
// +--------------------------------+    |    |   | | (normal output)      |  |
//                                       |    +---->|   stats_             |  |
//                                       |        | +----------------------+  |
//                                       |        |                           |
//                                       |        | +----------------------+  |
//                                       |        | | CompactionOutputs    |  |
//                                       |        | | (proximal_level)     |  |
//                                       +--------->|   stats_             |  |
//                                                | +----------------------+  |
//                                                |                           |
//                                                +---------------------------+

class CompactionJob {
 public:
  CompactionJob(int job_id, Compaction* compaction,
		const DBOptions& db_options,
                const ImmutableDBOptions& immutable_db_options,
		std::shared_ptr<Cache> input_block_cache,
                const MutableDBOptions& mutable_db_options,
                const FileOptions& file_options, VersionSet* versions,
                const std::atomic<bool>* shutting_down,
		const EnvOptions& env_options,
		LogBuffer* log_buffer,
                FSDirectory* db_directory, FSDirectory* output_directory,
                FSDirectory* blob_output_directory, Statistics* stats,
                InstrumentedMutex* db_mutex, ErrorHandler* db_error_handler,
                JobContext* job_context, std::shared_ptr<Cache> table_cache,
                EventLogger* event_logger, bool paranoid_file_checks,
                bool measure_io_stats, const std::string& dbname,
                CompactionJobStats* compaction_job_stats,
                Env::Priority thread_pri,
                const std::shared_ptr<IOTracer>& io_tracer,
		const std::atomic<bool>& manual_compaction_canceled,
		const ImmutableCFOptions& immutable_cf_options,
		const MutableCFOptions& mutable_cf_options,
                const std::string& db_id = "",
                const std::string& db_session_id = "",
                std::string full_history_ts_low = "", std::string trim_ts = "",
                BlobFileCompletionCallback* blob_callback = nullptr,
                int* bg_compaction_scheduled = nullptr,
                int* bg_bottom_compaction_scheduled = nullptr);

  virtual ~CompactionJob();

  // no copy/move
  CompactionJob(CompactionJob&& job) = delete;
  CompactionJob(const CompactionJob& job) = delete;
  CompactionJob& operator=(const CompactionJob& job) = delete;

  // REQUIRED: mutex held
  // Prepare for the compaction by setting up boundaries for each subcompaction
  // and organizing seqno <-> time info. `known_single_subcompact` is non-null
  // if we already have a known single subcompaction, with optional key bounds
  // (currently for executing a remote compaction).
  void Prepare(
      std::optional<std::pair<std::optional<Slice>, std::optional<Slice>>>
          known_single_subcompact);

  // REQUIRED mutex not held
  // Launch threads for each subcompaction and wait for them to finish. After
  // that, verify table is usable and finally do bookkeeping to unify
  // subcompaction results
  Status Run();

  // REQUIRED: mutex held
  // Add compaction input/output to the current version
  // Releases compaction file through Compaction::ReleaseCompactionFiles().
  // Sets *compaction_released to true if compaction is released.
  Status Install(bool* compaction_released);

  // Return the IO status
  IOStatus io_status() const { return io_status_; }

 protected:
  FileOptions file_options_for_compaction_;
  MutableCFOptions mutable_cf_options_;
  const DBOptions& db_options_;
  const ImmutableDBOptions& immutable_db_options_;
  void UpdateCompactionJobOutputStats(
      const InternalStats::CompactionStatsFull& internal_stats) const;

  void LogCompaction();
  virtual void RecordCompactionIOStats();
  void CleanupCompaction();

  // Iterate through input and compact the kv-pairs.
  void ProcessKeyValueCompaction(SubcompactionState* sub_compact);

  CompactionState* compact_;
  InternalStats::CompactionStatsFull internal_stats_;
  const MutableDBOptions mutable_db_options_copy_;
  LogBuffer* log_buffer_;
  FSDirectory* output_directory_;
  Statistics* stats_;
  // Is this compaction creating a file in the bottom most level?
  bool bottommost_level_;

  Env::WriteLifeTimeHint write_hint_;

  IOStatus io_status_;

  CompactionJobStats* job_stats_;

 private:
  //const DBOptions& db_options_;
  //const ImmutableDBOptions& immutable_db_options_;
  std::shared_ptr<Cache> block_cache_;
  friend class CompactionJobTestBase;
  ColumnFamilyData* cfd_;
  EnvOptions env_options_;
  
  ImmutableCFOptions immutable_cf_options_;
  
  FileSystemPtr fs_;


 
 
 

  std::shared_ptr<Cache> block_cache;
  // Collect the following stats from Input Table Properties
  // - num_input_files_in_non_output_levels
  // - num_input_files_in_output_level
  // - bytes_read_non_output_levels
  // - bytes_read_output_level
  // - num_input_records
  // - bytes_read_blob
  // - num_dropped_records
  // and set them in internal_stats_.output_level_stats
  //
  // @param num_input_range_del if non-null, will be set to the number of range
  // deletion entries in this compaction input.
  //
  // Returns true iff internal_stats_.output_level_stats.num_input_records and
  // num_input_range_del are calculated successfully.
  //
  // This should be called only once for compactions (not per subcompaction)
  bool BuildStatsFromInputTableProperties(
      uint64_t* num_input_range_del = nullptr);

  void UpdateCompactionJobInputStats(
      const InternalStats::CompactionStatsFull& internal_stats,
      uint64_t num_input_range_del) const;

  Status VerifyInputRecordCount(uint64_t num_input_range_del) const;

  // Generates a histogram representing potential divisions of key ranges from
  // the input. It adds the starting and/or ending keys of certain input files
  // to the working set and then finds the approximate size of data in between
  // each consecutive pair of slices. Then it divides these ranges into
  // consecutive groups such that each group has a similar size.
  void GenSubcompactionBoundaries();

  // Get the number of planned subcompactions based on max_subcompactions and
  // extra reserved resources
  uint64_t GetSubcompactionsLimit();

  // Additional reserved threads are reserved and the number is stored in
  // extra_num_subcompaction_threads_reserved__. For now, this happens only if
  // the compaction priority is round-robin and max_subcompactions is not
  // sufficient (extra resources may be needed)
  void AcquireSubcompactionResources(int num_extra_required_subcompactions);

  // Additional threads may be reserved during IncreaseSubcompactionResources()
  // if num_actual_subcompactions is less than num_planned_subcompactions.
  // Additional threads will be released and the bg_compaction_scheduled_ or
  // bg_bottom_compaction_scheduled_ will be updated if they are used.
  // DB Mutex lock is required.
  void ShrinkSubcompactionResources(uint64_t num_extra_resources);

  // Release all reserved threads and update the compaction limits.
  void ReleaseSubcompactionResources();

  CompactionServiceJobStatus ProcessKeyValueCompactionWithCompactionService(
      SubcompactionState* sub_compact);

  // update the thread status for starting a compaction.
  void ReportStartedCompaction(Compaction* compaction);

  Status FinishCompactionOutputFile(const Status& input_status,
                                    SubcompactionState* sub_compact,
                                    CompactionOutputs& outputs,
                                    const Slice& next_table_min_key,
                                    const Slice* comp_start_user_key,
                                    const Slice* comp_end_user_key);
  Status InstallCompactionResults(bool* compaction_released);
  Status OpenCompactionOutputFile(SubcompactionState* sub_compact,
                                  CompactionOutputs& outputs);

  void RecordDroppedKeys(const CompactionIterationStats& c_iter_stats,
                         CompactionJobStats* compaction_job_stats = nullptr);

  void NotifyOnSubcompactionBegin(SubcompactionState* sub_compact);

  void NotifyOnSubcompactionCompleted(SubcompactionState* sub_compact);

  uint32_t job_id_;

  // DBImpl state
  const std::string& dbname_;
  const std::string db_id_;
  const std::string db_session_id_;
  const FileOptions file_options_;

  Env* env_;
  std::shared_ptr<IOTracer> io_tracer_;
  // env_option optimized for compaction table reads
  FileOptions file_options_for_read_;
  VersionSet* versions_;
  const std::atomic<bool>* shutting_down_;
  const std::atomic<bool>& manual_compaction_canceled_;
  FSDirectory* db_directory_;
  FSDirectory* blob_output_directory_;
  InstrumentedMutex* db_mutex_;
  ErrorHandler* db_error_handler_;

  SequenceNumber earliest_snapshot_;
  JobContext* job_context_;

  std::shared_ptr<Cache> table_cache_;

  EventLogger* event_logger_;

  bool paranoid_file_checks_;
  bool measure_io_stats_;
  // Stores the Slices that designate the boundaries for each subcompaction
  std::vector<std::string> boundaries_;
  Env::Priority thread_pri_;
  std::string full_history_ts_low_;
  std::string trim_ts_;
  BlobFileCompletionCallback* blob_callback_;

  uint64_t GetCompactionId(SubcompactionState* sub_compact) const;
  // Stores the number of reserved threads in shared env_ for the number of
  // extra subcompaction in kRoundRobin compaction priority
  int extra_num_subcompaction_threads_reserved_;

  // Stores the pointer to bg_compaction_scheduled_,
  // bg_bottom_compaction_scheduled_ in DBImpl. Mutex is required when accessing
  // or updating it.
  int* bg_compaction_scheduled_;
  int* bg_bottom_compaction_scheduled_;

  // Stores the sequence number to time mapping gathered from all input files
  // it also collects the smallest_seqno -> oldest_ancester_time from the SST.
  SeqnoToTimeMapping seqno_to_time_mapping_;

  // Max seqno that can be zeroed out in last level, including for preserving
  // write times.
  SequenceNumber preserve_seqno_after_ = kMaxSequenceNumber;

  // Minimal sequence number to preclude the data from the last level. If the
  // key has bigger (newer) sequence number than this, it will be precluded from
  // the last level (output to proximal level).
  SequenceNumber proximal_after_seqno_ = kMaxSequenceNumber;

  // Options File Number used for Remote Compaction
  // Setting this requires DBMutex.
  uint64_t options_file_number_ = 0;

  // Get table file name in where it's outputting to, which should also be in
  // `output_directory_`.
  virtual std::string GetTableFileName(uint64_t file_number);
  // The rate limiter priority (io_priority) is determined dynamically here.
  // The Compaction Read and Write priorities are the same for different
  // scenarios, such as write stalled.
  Env::IOPriority GetRateLimiterPriority();
};

// CompactionServiceInput is used the pass compaction information between two
// db instances. It contains the information needed to do a compaction. It
// doesn't contain the LSM tree information, which is passed though MANIFEST
// file.
struct CompactionServiceInput {
  std::string cf_name;

  std::vector<SequenceNumber> snapshots;

  // SST files for compaction, it should already be expended to include all the
  // files needed for this compaction, for both input level files and output
  // level files.
  std::vector<std::string> input_files;
  int output_level = 0;

  // db_id is used to generate unique id of sst on the remote compactor
  std::string db_id;

  // information for subcompaction
  bool has_begin = false;
  std::string begin;
  bool has_end = false;
  std::string end;

  uint64_t options_file_number = 0;

  // serialization interface to read and write the object
  static Status Read(const std::string& data_str, CompactionServiceInput* obj);
  Status Write(std::string* output);

#ifndef NDEBUG
  bool TEST_Equals(CompactionServiceInput* other);
  bool TEST_Equals(CompactionServiceInput* other, std::string* mismatch);
#endif  // NDEBUG
};

// CompactionServiceOutputFile is the metadata for the output SST file
struct CompactionServiceOutputFile {
  std::string file_name;
  uint64_t file_size{};
  SequenceNumber smallest_seqno{};
  SequenceNumber largest_seqno{};
  std::string smallest_internal_key;
  std::string largest_internal_key;
  uint64_t oldest_ancester_time = kUnknownOldestAncesterTime;
  uint64_t file_creation_time = kUnknownFileCreationTime;
  uint64_t epoch_number = kUnknownEpochNumber;
  std::string file_checksum = kUnknownFileChecksum;
  std::string file_checksum_func_name = kUnknownFileChecksumFuncName;
  uint64_t paranoid_hash{};
  bool marked_for_compaction;
  UniqueId64x2 unique_id{};
  TableProperties table_properties;
  bool is_proximal_level_output;
  Temperature file_temperature = Temperature::kUnknown;

  CompactionServiceOutputFile() = default;
  CompactionServiceOutputFile(
      const std::string& name, uint64_t size, SequenceNumber smallest,
      SequenceNumber largest, std::string _smallest_internal_key,
      std::string _largest_internal_key, uint64_t _oldest_ancester_time,
      uint64_t _file_creation_time, uint64_t _epoch_number,
      const std::string& _file_checksum,
      const std::string& _file_checksum_func_name, uint64_t _paranoid_hash,
      bool _marked_for_compaction, UniqueId64x2 _unique_id,
      const TableProperties& _table_properties, bool _is_proximal_level_output,
      Temperature _file_temperature)
      : file_name(name),
        file_size(size),
        smallest_seqno(smallest),
        largest_seqno(largest),
        smallest_internal_key(std::move(_smallest_internal_key)),
        largest_internal_key(std::move(_largest_internal_key)),
        oldest_ancester_time(_oldest_ancester_time),
        file_creation_time(_file_creation_time),
        epoch_number(_epoch_number),
        file_checksum(_file_checksum),
        file_checksum_func_name(_file_checksum_func_name),
        paranoid_hash(_paranoid_hash),
        marked_for_compaction(_marked_for_compaction),
        unique_id(std::move(_unique_id)),
        table_properties(_table_properties),
        is_proximal_level_output(_is_proximal_level_output),
        file_temperature(_file_temperature) {}
};

// CompactionServiceResult contains the compaction result from a different db
// instance, with these information, the primary db instance with write
// permission is able to install the result to the DB.
struct CompactionServiceResult {
  Status status;
  std::vector<CompactionServiceOutputFile> output_files;
  int output_level = 0;

  // location of the output files
  std::string output_path;

  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;

  // Job-level Compaction Stats.
  //
  // NOTE: Job level stats cannot be rebuilt from scratch by simply aggregating
  // per-level stats due to some fields populated directly during compaction
  // (e.g. RecordDroppedKeys()). This is why we need both job-level stats and
  // per-level in the serialized result. If rebuilding job-level stats from
  // per-level stats become possible in the future, consider deprecating this
  // field.
  CompactionJobStats stats;
  
  InternalStats::CompactionStatsFull internal_stats;
  // Per-level Compaction Stats for both output_level_stats and
  // proximal_level_stats

  // serialization interface to read and write the object
  static Status Read(const std::string& data_str, CompactionServiceResult* obj);
  Status Write(std::string* output);

#ifndef NDEBUG
  bool TEST_Equals(CompactionServiceResult* other);
  bool TEST_Equals(CompactionServiceResult* other, std::string* mismatch);
#endif  // NDEBUG
};

// CompactionServiceCompactionJob is an read-only compaction job, it takes
// input information from `compaction_service_input` and put result information
// in `compaction_service_result`, the SST files are generated to `output_path`.
class CompactionServiceCompactionJob : private CompactionJob {
 public:
  CompactionServiceCompactionJob(
      int job_id, Compaction* compaction,
      const DBOptions& db_options,
      const ImmutableDBOptions& immutable_db_options,
      std::shared_ptr<Cache> input_block_cache,
      const MutableDBOptions& mutable_db_options,
      const FileOptions& file_options, VersionSet* versions,
      const std::atomic<bool>* shutting_down,
      const EnvOptions& env_options,  
      LogBuffer* log_buffer,
      FSDirectory* db_directory, 
      FSDirectory* output_directory,
      FSDirectory* blob_output_directory, 
      Statistics* stats,
      InstrumentedMutex* db_mutex, ErrorHandler* db_error_handler,
      JobContext* job_context, std::shared_ptr<Cache> table_cache,
      EventLogger* event_logger, const std::string& dbname,
      const std::shared_ptr<IOTracer>& io_tracer,
      const std::atomic<bool>& manual_compaction_canceled,
      const ImmutableCFOptions& immutable_cf_options,
      const MutableCFOptions& mutable_cf_options,
      const std::string& db_id, const std::string& db_session_id,
      std::string full_history_ts_low, std::string trim_ts,
      BlobFileCompletionCallback* blob_callback,
      int* bg_compaction_scheduled,
      int* bg_bottom_compaction_scheduled,
      std::string output_path,
      const CompactionServiceInput& compaction_service_input,
      CompactionServiceResult* compaction_service_result);

  // REQUIRED: mutex held
  // Like CompactionJob::Prepare()
  void Prepare();

  // Run the compaction in current thread and return the result
  Status Run();

  void CleanupCompaction();

  IOStatus io_status() const { return CompactionJob::io_status(); }

 protected:
  void RecordCompactionIOStats() override;

 private:
  // Get table file name in output_path
  std::string GetTableFileName(uint64_t file_number) override;
  // Specific the compaction output path, otherwise it uses default DB path
  const std::string output_path_;

  // Compaction job input
  const CompactionServiceInput& compaction_input_;

  // Compaction job result
  CompactionServiceResult* compaction_result_;
};

}  // namespace ROCKSDB_NAMESPACE
