/*
 * Copyright (c) 2014-2015, Hewlett-Packard Development Company, LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * HP designates this particular file as subject to the "Classpath" exception
 * as provided by HP in the LICENSE.txt file that accompanied this code.
 */
#include "foedus/tpce/tpce_driver.hpp"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <sys/wait.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "foedus/engine.hpp"
#include "foedus/engine_options.hpp"
#include "foedus/error_stack.hpp"
#include "foedus/debugging/debugging_supports.hpp"
#include "foedus/debugging/stop_watch.hpp"
#include "foedus/fs/filesystem.hpp"
#include "foedus/log/log_manager.hpp"
#include "foedus/memory/engine_memory.hpp"
#include "foedus/proc/proc_manager.hpp"
#include "foedus/snapshot/snapshot_manager.hpp"
#include "foedus/soc/shared_memory_repo.hpp"
#include "foedus/soc/soc_manager.hpp"
#include "foedus/thread/numa_thread_scope.hpp"
#include "foedus/thread/thread.hpp"
#include "foedus/thread/thread_pool.hpp"
#include "foedus/thread/thread_pool_pimpl.hpp"
#include "foedus/tpce/tpce.hpp"
#include "foedus/tpce/tpce_client.hpp"
#include "foedus/tpce/tpce_load.hpp"
#include "foedus/xct/xct_id.hpp"

namespace foedus {
namespace tpce {
DEFINE_bool(fork_workers, false, "Whether to fork(2) worker threads in child processes rather"
    " than threads in the same process. This is required to scale up to 100+ cores.");
DEFINE_bool(take_snapshot, false, "Whether to run a log gleaner after loading data.");
DEFINE_bool(preload_snapshot_pages, false, "Pre-fetch snapshot pages before execution.");
DEFINE_bool(disable_snapshot_cache, false, "Disable snapshot cache and read from file always.");
DEFINE_string(nvm_folder, "/dev/shm", "Full path of the device representing NVM.");
DEFINE_bool(profile, false, "Whether to profile the execution with gperftools.");
DEFINE_bool(papi, false, "Whether to profile with PAPI.");
DEFINE_int32(volatile_pool_size, 6, "Size of volatile memory pool per NUMA node in GB.");
DEFINE_int32(snapshot_pool_size, 2, "Size of snapshot memory pool per NUMA node in MB.");
DEFINE_int32(reducer_buffer_size, 2, "Size of reducer's buffer per NUMA node in GB.");
DEFINE_int32(loggers_per_node, 1, "Number of log writers per numa node.");
DEFINE_bool(skip_verify, false, "Whether to skip the detailed verification after data load."
  " The verification is single-threaded, and scans all pages. In a big machine, it takes a minute."
  " In case you want to skip it, enable this. But, we should usually check bugs.");
DEFINE_int32(thread_per_node, 2, "Number of threads per NUMA node. 0 uses logical count");
DEFINE_int32(numa_nodes, 2, "Number of NUMA nodes. 0 uses physical count");
DEFINE_int32(log_buffer_mb, 1024, "Size in MB of log buffer for each thread");
DEFINE_bool(null_log_device, true, "Whether to disable log writing.");
DEFINE_int64(duration_micro, 10000000, "Duration of benchmark in microseconds.");
DEFINE_int32(hot_threshold, -1, "Threshold to determine hot/cold pages,"
  " 0 (always hot, 2PL) - 256 (always cold, OCC).");
DEFINE_int64(customers, 1000, "The number of customers, or Scale Factor * tpsE."
  " The Scale Factor (SF) is the number of required customer rows per"
  " single tpsE. SF for Nominal Throughput is 500."
  " For example, for a database size of 5000 customers,"
  " the nominal performance is 10.00 tpsE."
  " The TPC-E spec also defines that the minimal # of customers is 5000,"
  " so tpcE must be 10 or larger. The spec also specifies that this"
  " number must be a multiply of 1000 (Load Unit).");
DEFINE_int64(itd, 1, "The Initial Trade Days (ITD) is the number of Business Days used to"
  " populate the database. This population is made of trade data"
  " that would be generated by the SUT when running at the"
  " Nominal Throughput for the specified number of Business Days."
  " ITD for Nominal Throughput is 300.");
DEFINE_double(symbol_skew, 0.25, "Skewness to pick a security symbol"
  " for both trade-order (insert) and other references."
  " 0 means uniform. Higher value causes higher skew, skewing to lower symbol IDs.");

TpceDriver::Result TpceDriver::run() {
  const EngineOptions& options = engine_->get_options();
  LOG(INFO) << engine_->get_memory_manager()->dump_free_memory_stat();
  scale_ = {
    options.thread_.get_total_thread_count(),
    static_cast<uint64_t>(FLAGS_customers),
    static_cast<uint64_t>(FLAGS_itd),
    FLAGS_symbol_skew,
  };

  if (scale_.get_security_cardinality() > kMaxSymbT) {
    LOG(ERROR) << "Too many customers. We so far assume at most " << kMaxSymbT << " securities,"
      << " but " << scale_.customers_ << " yields " << scale_.get_security_cardinality()
      << " security symbols";
    return Result();
  }

  {
    // first, create empty tables. this is done in single thread
    ErrorStack create_result = create_all(engine_, scale_);
    LOG(INFO) << "creator_result=" << create_result;
    if (create_result.is_error()) {
      COERCE_ERROR(create_result);
      return Result();
    }
  }

  auto* thread_pool = engine_->get_thread_pool();
  {
    // then, load data into the tables.
    // this takes long, so it's parallelized.
    std::vector< thread::ImpersonateSession > sessions;
    for (uint16_t node = 0; node < options.thread_.group_count_; ++node) {
      for (uint16_t ordinal = 0; ordinal < options.thread_.thread_count_per_group_; ++ordinal) {
        TpceLoadTask::Inputs inputs = {
          scale_,
          static_cast<PartitionT>(sessions.size()),
        };
        thread::ImpersonateSession session;
        bool ret = thread_pool->impersonate_on_numa_node(
          node,
          "tpce_load_task",
          &inputs,
          sizeof(inputs),
          &session);
        if (!ret) {
          LOG(FATAL) << "Couldn't impersonate";
        }
        sessions.emplace_back(std::move(session));
      }
    }

    const uint64_t kMaxWaitMs = 1000 * 1000;
    const uint64_t kIntervalMs = 100;
    uint64_t wait_count = 0;
    for (uint16_t i = 0; i < sessions.size();) {
      assorted::memory_fence_acquire();
      if (wait_count * kIntervalMs > kMaxWaitMs) {
        LOG(FATAL) << "Data population is taking much longer than expected. Quiting.";
      }
      if (sessions[i].is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kIntervalMs));
        ++wait_count;
        continue;
      }
      LOG(INFO) << "loader_result[" << i << "]=" << sessions[i].get_result();
      if (sessions[i].get_result().is_error()) {
        LOG(FATAL) << "Failed data load " << sessions[i].get_result();
      }
      sessions[i].release();
      ++i;
    }

    LOG(INFO) << "Completed data load";
  }


  // Verify the loaded data. this is done in single thread
  {
    TpceFinishupTask::Inputs input = {scale_, FLAGS_skip_verify, FLAGS_take_snapshot};
    thread::ImpersonateSession finishup_session;
    bool impersonated = thread_pool->impersonate(
      "tpce_finishup_task",
      &input,
      sizeof(input),
      &finishup_session);
    if (!impersonated) {
      LOG(FATAL) << "Failed to impersonate??";
    }

    const uint64_t kMaxWaitMs = 60 * 1000;
    const uint64_t kIntervalMs = 10;
    uint64_t wait_count = 0;
    LOG(INFO) << "waiting for tpce_finishup_task....";
    while (true) {
      assorted::memory_fence_acquire();
      if (wait_count * kIntervalMs > kMaxWaitMs) {
        LOG(FATAL) << "tpce_finishup_task is taking much longer than expected. Quiting.";
      }
      if (finishup_session.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kIntervalMs));
        ++wait_count;
        continue;
      } else {
        break;
      }
    }

    ASSERT_ND(!finishup_session.is_running());
    ErrorStack finishup_result = finishup_session.get_result();
    finishup_session.release();
    LOG(INFO) << "finish_result=" << finishup_result;
    if (finishup_result.is_error()) {
      COERCE_ERROR(finishup_result);
      return Result();
    }
  }
  LOG(INFO) << engine_->get_memory_manager()->dump_free_memory_stat();


  if (FLAGS_take_snapshot) {
    Epoch global_durable = engine_->get_log_manager()->get_durable_global_epoch();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    LOG(INFO) << "Now taking a snapshot...";
    debugging::StopWatch watch;
    engine_->get_snapshot_manager()->trigger_snapshot_immediate(true);
    watch.stop();
    LOG(INFO) << "Took a snapshot in " << watch.elapsed_ms() << "ms";
    Epoch snapshot_epoch = engine_->get_snapshot_manager()->get_snapshot_epoch();
    if (!snapshot_epoch.is_valid() || snapshot_epoch < global_durable) {
      LOG(FATAL) << "Failed to take snapshot??";
    }
    TpceStorages storages;
    storages.initialize_tables(engine_);
  }

  TpceClientChannel* channel = reinterpret_cast<TpceClientChannel*>(
    engine_->get_soc_manager()->get_shared_memory_repo()->get_global_user_memory());
  channel->initialize();
  channel->preload_snapshot_pages_ = FLAGS_preload_snapshot_pages;

  std::vector< thread::ImpersonateSession > sessions;
  std::vector< const TpceClientTask::Outputs* > outputs;

  for (uint16_t node = 0; node < options.thread_.group_count_; ++node) {
    for (uint16_t ordinal = 0; ordinal < options.thread_.thread_count_per_group_; ++ordinal) {
      TpceClientTask::Inputs inputs = {
        scale_,
        static_cast<PartitionT>(sessions.size()),
      };
      thread::ImpersonateSession session;
      bool ret = thread_pool->impersonate_on_numa_node(
        node,
        "tpce_client_task",
        &inputs,
        sizeof(inputs),
        &session);
      if (!ret) {
        LOG(FATAL) << "Couldn't impersonate";
      }
      outputs.push_back(
        reinterpret_cast<const TpceClientTask::Outputs*>(session.get_raw_output_buffer()));
      sessions.emplace_back(std::move(session));
    }
  }
  LOG(INFO) << "okay, launched all worker threads. waiting for completion of warmup...";
  uint32_t total_thread_count = options.thread_.get_total_thread_count();
  while (channel->warmup_complete_counter_.load() < total_thread_count) {
    LOG(INFO) << "Waiting for warmup completion... done=" << channel->warmup_complete_counter_
      << "/" << total_thread_count;
    if (channel->exit_nodes_ != 0) {
      LOG(FATAL) << "FATAL. Some client exitted with error.";
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  LOG(INFO) << "All warmup done!";
  if (FLAGS_profile) {
    COERCE_ERROR(engine_->get_debug()->start_profile("tpce.prof"));
  }
  if (FLAGS_papi) {
    engine_->get_debug()->start_papi_counters();
  }
  channel->start_rendezvous_.signal();
  assorted::memory_fence_release();
  LOG(INFO) << "Started!";
  debugging::StopWatch duration;
  while (duration.peek_elapsed_ns() < static_cast<uint64_t>(FLAGS_duration_micro) * 1000ULL) {
    // wake up for each second to show intermediate results.
    uint64_t remaining_duration = FLAGS_duration_micro - duration.peek_elapsed_ns() / 1000ULL;
    remaining_duration = std::min<uint64_t>(remaining_duration, 1000000ULL);
    std::this_thread::sleep_for(std::chrono::microseconds(remaining_duration));
    Result result;
    result.duration_sec_ = static_cast<double>(duration.peek_elapsed_ns()) / 1000000000;
    result.worker_count_ = total_thread_count;
    for (uint32_t i = 0; i < sessions.size(); ++i) {
      const TpceClientTask::Outputs* output = outputs[i];
      result.processed_ += output->processed_;
      result.race_aborts_ += output->race_aborts_;
      result.unexpected_aborts_ += output->unexpected_aborts_;
      result.largereadset_aborts_ += output->largereadset_aborts_;
      result.user_requested_aborts_ += output->user_requested_aborts_;
      result.snapshot_cache_hits_ += output->snapshot_cache_hits_;
      result.snapshot_cache_misses_ += output->snapshot_cache_misses_;
    }
    LOG(INFO) << "Intermediate report after " << result.duration_sec_ << " sec";
    LOG(INFO) << result;
    LOG(INFO) << engine_->get_memory_manager()->dump_free_memory_stat();
  }
  LOG(INFO) << "Experiment ended.";

  if (FLAGS_profile) {
    engine_->get_debug()->stop_profile();
  }
  if (FLAGS_papi) {
    engine_->get_debug()->stop_papi_counters();
  }

  Result result;
  duration.stop();
  result.duration_sec_ = duration.elapsed_sec();
  result.worker_count_ = total_thread_count;
  result.papi_results_ = debugging::DebuggingSupports::describe_papi_counters(
    engine_->get_debug()->get_papi_counters());
  assorted::memory_fence_acquire();
  for (uint32_t i = 0; i < sessions.size(); ++i) {
    const TpceClientTask::Outputs* output = outputs[i];
    result.workers_[i].id_ = i;
    result.workers_[i].processed_ = output->processed_;
    result.workers_[i].race_aborts_ = output->race_aborts_;
    result.workers_[i].unexpected_aborts_ = output->unexpected_aborts_;
    result.workers_[i].largereadset_aborts_ = output->largereadset_aborts_;
    result.workers_[i].user_requested_aborts_ = output->user_requested_aborts_;
    result.workers_[i].snapshot_cache_hits_ = output->snapshot_cache_hits_;
    result.workers_[i].snapshot_cache_misses_ = output->snapshot_cache_misses_;
    result.processed_ += output->processed_;
    result.race_aborts_ += output->race_aborts_;
    result.unexpected_aborts_ += output->unexpected_aborts_;
    result.largereadset_aborts_ += output->largereadset_aborts_;
    result.user_requested_aborts_ += output->user_requested_aborts_;
    result.snapshot_cache_hits_ += output->snapshot_cache_hits_;
    result.snapshot_cache_misses_ += output->snapshot_cache_misses_;
  }
  LOG(INFO) << "Shutting down...";

  // output the current memory state at the end
  LOG(INFO) << engine_->get_memory_manager()->dump_free_memory_stat();

  channel->stop_flag_.store(true);

  for (uint32_t i = 0; i < sessions.size(); ++i) {
    LOG(INFO) << "result[" << i << "]=" << sessions[i].get_result();
    sessions[i].release();
  }
  channel->uninitialize();
  return result;
}

/** This method just constructs options and gives it to engine object. Nothing more */
int driver_main(int argc, char **argv) {
  std::vector< proc::ProcAndName > procs;
  procs.emplace_back("tpce_client_task", tpce_client_task);
  procs.emplace_back("tpce_finishup_task", tpce_finishup_task);
  procs.emplace_back("tpce_load_task", tpce_load_task);
  {
    // In case the main() was called for exec()-style SOC engines.
    soc::SocManager::trap_spawned_soc_main(procs);
  }
  gflags::SetUsageMessage("TPC-E implementation for FOEDUS");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  fs::Path folder("/dev/shm/foedus_tpce");
  if (fs::exists(folder)) {
    fs::remove_all(folder);
  }
  if (!fs::create_directories(folder)) {
    std::cerr << "Couldn't create " << folder << ". err=" << assorted::os_error();
    return 1;
  }

  EngineOptions options;

  fs::Path savepoint_path(folder);
  savepoint_path /= "savepoint.xml";
  options.savepoint_.savepoint_path_.assign(savepoint_path.string());
  ASSERT_ND(!fs::exists(savepoint_path));

  std::cout << "NUMA node count=" << static_cast<int>(options.thread_.group_count_) << std::endl;
  if (FLAGS_numa_nodes != 0) {
    std::cout << "numa_nodes specified:" << FLAGS_numa_nodes << std::endl;
    options.thread_.group_count_ = FLAGS_numa_nodes;
  }

  options.snapshot_.folder_path_pattern_ = "/dev/shm/foedus_tpce/snapshot/node_$NODE$";
  options.log_.folder_path_pattern_ = "/dev/shm/foedus_tpce/log/node_$NODE$/logger_$LOGGER$";
  options.log_.loggers_per_node_ = FLAGS_loggers_per_node;
  options.log_.flush_at_shutdown_ = false;
  options.snapshot_.snapshot_interval_milliseconds_ = 100000000U;

  if (FLAGS_take_snapshot) {
    std::cout << "Will take snapshot after initial data load." << std::endl;
    FLAGS_null_log_device = false;

    if (FLAGS_disable_snapshot_cache) {
      std::cout << "Oh, snapshot cache is disabled. will read from file everytime" << std::endl;
      options.cache_.snapshot_cache_enabled_ = false;
    }

    options.snapshot_.log_mapper_io_buffer_mb_ = 1 << 8;
    options.snapshot_.log_mapper_bucket_kb_ = 1 << 12;
    options.snapshot_.log_reducer_buffer_mb_ = FLAGS_reducer_buffer_size << 10;
    options.snapshot_.snapshot_writer_page_pool_size_mb_ = 1 << 10;
    options.snapshot_.snapshot_writer_intermediate_pool_size_mb_ = 1 << 8;
    options.cache_.snapshot_cache_size_mb_per_node_ = FLAGS_snapshot_pool_size;
    if (FLAGS_reducer_buffer_size > 10) {  // probably OLAP experiment in a large machine?
      options.snapshot_.log_mapper_io_buffer_mb_ = 1 << 11;
      options.snapshot_.log_mapper_bucket_kb_ = 1 << 15;
      options.snapshot_.snapshot_writer_page_pool_size_mb_ = 1 << 13;
      options.snapshot_.snapshot_writer_intermediate_pool_size_mb_ = 1 << 11;
      options.snapshot_.log_reducer_read_io_buffer_kb_ = FLAGS_reducer_buffer_size * 1024;
    }

    fs::Path nvm_folder(FLAGS_nvm_folder);
    if (!fs::exists(nvm_folder)) {
      std::cerr << "The NVM-folder " << nvm_folder << " not mounted yet";
      return 1;
    }

    fs::Path tpce_folder(nvm_folder);
    tpce_folder /= "foedus_tpce";
    if (fs::exists(tpce_folder)) {
      fs::remove_all(tpce_folder);
    }
    if (!fs::create_directories(tpce_folder)) {
      std::cerr << "Couldn't create " << tpce_folder << ". err=" << assorted::os_error();
      return 1;
    }

    savepoint_path = tpce_folder;
    savepoint_path /= "savepoint.xml";
    if (fs::exists(savepoint_path)) {
      fs::remove(savepoint_path);
    }
    ASSERT_ND(!fs::exists(savepoint_path));
    options.savepoint_.savepoint_path_.assign(savepoint_path.string());

    fs::Path snapshot_folder(tpce_folder);
    snapshot_folder /= "snapshot";
    if (fs::exists(snapshot_folder)) {
      fs::remove_all(snapshot_folder);
    }
    fs::Path snapshot_pattern(snapshot_folder);
    snapshot_pattern /= "node_$NODE$";
    options.snapshot_.folder_path_pattern_.assign(snapshot_pattern.string());

    fs::Path log_folder(tpce_folder);
    log_folder /= "log";
    if (fs::exists(log_folder)) {
      fs::remove_all(log_folder);
    }
    fs::Path log_pattern(log_folder);
    log_pattern /= "node_$NODE$/logger_$LOGGER$";
    options.log_.folder_path_pattern_.assign(log_pattern.string());
  }

  options.debugging_.debug_log_min_threshold_
    = debugging::DebuggingOptions::kDebugLogInfo;
    // = debugging::DebuggingOptions::kDebugLogWarning;
  options.debugging_.verbose_modules_ = "";
  options.debugging_.verbose_log_level_ = -1;

  options.log_.log_buffer_kb_ = FLAGS_log_buffer_mb << 10;
  std::cout << "log_buffer_mb=" << FLAGS_log_buffer_mb << "MB per thread" << std::endl;
  options.log_.log_file_size_mb_ = 1 << 15;
  std::cout << "volatile_pool_size=" << FLAGS_volatile_pool_size << "GB per NUMA node" << std::endl;
  options.memory_.page_pool_size_mb_per_node_ = (FLAGS_volatile_pool_size) << 10;

  if (FLAGS_thread_per_node != 0) {
    std::cout << "thread_per_node=" << FLAGS_thread_per_node << std::endl;
    options.thread_.thread_count_per_group_ = FLAGS_thread_per_node;
  }

  if (FLAGS_null_log_device) {
    std::cout << "/dev/null log device" << std::endl;
    options.log_.emulation_.null_device_ = true;
  }

  if (FLAGS_fork_workers) {
    std::cout << "Will fork workers in child processes" << std::endl;
    options.soc_.soc_type_ = kChildForked;
  }

  if (FLAGS_hot_threshold > 256) {
    std::cout << "Hot page threshold is too large: " << FLAGS_hot_threshold
      << ". Choose a value between 0 and 256 (inclusive)." << std::endl;
    return 1;
  }
  options.storage_.hot_threshold_ = FLAGS_hot_threshold;
  std::cout << "Hot record threshold: " << options.storage_.hot_threshold_ << std::endl;

  TpceDriver::Result result;
  {
    Engine engine(options);
    for (const proc::ProcAndName& proc : procs) {
      engine.get_proc_manager()->pre_register(proc);
    }
    COERCE_ERROR(engine.initialize());
    {
      UninitializeGuard guard(&engine);
      TpceDriver driver(&engine);
      result = driver.run();
      COERCE_ERROR(engine.uninitialize());
    }
  }

  // wait just for a bit to avoid mixing stdout
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  for (uint32_t i = 0; i < result.worker_count_; ++i) {
    LOG(INFO) << result.workers_[i];
  }
  LOG(INFO) << "final result:" << result;
  if (FLAGS_papi) {
    LOG(INFO) << "PAPI results:";
    for (uint16_t i = 0; i < result.papi_results_.size(); ++i) {
      LOG(INFO) << result.papi_results_[i];
    }
  }
  if (FLAGS_profile) {
    std::cout << "Check out the profile result: pprof --pdf tpce tpce.prof > prof.pdf; "
      "okular prof.pdf" << std::endl;
  }

  return 0;
}

std::ostream& operator<<(std::ostream& o, const TpceDriver::Result& v) {
  o << "<total_result>"
    << "<duration_sec_>" << v.duration_sec_ << "</duration_sec_>"
    << "<worker_count_>" << v.worker_count_ << "</worker_count_>"
    << "<processed_>" << v.processed_ << "</processed_>"
    << "<MTPS>" << ((v.processed_ / v.duration_sec_) / 1000000) << "</MTPS>"
    << "<user_requested_aborts_>" << v.user_requested_aborts_ << "</user_requested_aborts_>"
    << "<race_aborts_>" << v.race_aborts_ << "</race_aborts_>"
    << "<largereadset_aborts_>" << v.largereadset_aborts_ << "</largereadset_aborts_>"
    << "<unexpected_aborts_>" << v.unexpected_aborts_ << "</unexpected_aborts_>"
    << "<snapshot_cache_hits_>" << v.snapshot_cache_hits_ << "</snapshot_cache_hits_>"
    << "<snapshot_cache_misses_>" << v.snapshot_cache_misses_ << "</snapshot_cache_misses_>"
    << "</total_result>";
  return o;
}

std::ostream& operator<<(std::ostream& o, const TpceDriver::WorkerResult& v) {
  o << "  <worker_><id>" << v.id_ << "</id>"
    << "<txn>" << v.processed_ << "</txn>"
    << "<usrab>" << v.user_requested_aborts_ << "</usrab>"
    << "<raceab>" << v.race_aborts_ << "</raceab>"
    << "<rsetab>" << v.largereadset_aborts_ << "</rsetab>"
    << "<unexab>" << v.unexpected_aborts_ << "</unexab>"
    << "<sphit>" << v.snapshot_cache_hits_ << "</sphit>"
    << "<spmis>" << v.snapshot_cache_misses_ << "</spmis>"
    << "</worker>";
  return o;
}

}  // namespace tpce
}  // namespace foedus
