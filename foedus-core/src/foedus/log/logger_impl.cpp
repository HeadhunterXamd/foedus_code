/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include <foedus/engine.hpp>
#include <foedus/error_stack_batch.hpp>
#include <foedus/log/common_log_types.hpp>
#include <foedus/log/logger_impl.hpp>
#include <foedus/log/log_type.hpp>
#include <foedus/log/thread_log_buffer_impl.hpp>
#include <foedus/fs/filesystem.hpp>
#include <foedus/fs/direct_io_file.hpp>
#include <foedus/engine_options.hpp>
#include <foedus/savepoint/savepoint.hpp>
#include <foedus/savepoint/savepoint_manager.hpp>
#include <foedus/thread/thread_pool.hpp>
#include <foedus/thread/thread_pool_pimpl.hpp>
#include <foedus/thread/thread.hpp>
#include <foedus/memory/engine_memory.hpp>
#include <foedus/memory/numa_node_memory.hpp>
#include <foedus/assert_nd.hpp>
#include <foedus/xct/xct_manager.hpp>
#include <glog/logging.h>
#include <numa.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
namespace foedus {
namespace log {
fs::Path Logger::construct_suffixed_log_path(LogFileOrdinal ordinal) const {
    std::stringstream path_str;
    path_str << log_path_.string() << "." << ordinal;
    return fs::Path(path_str.str());
}

ErrorStack Logger::initialize_once() {
    // clear all variables
    current_file_ = nullptr;
    oldest_ordinal_ = 0;
    current_ordinal_ = 0;
    node_memory_ = nullptr;
    logger_buffer_cursor_ = 0;
    durable_epoch_ = xct::Epoch();
    current_epoch_ = xct::Epoch();
    numa_node_ = static_cast<int>(thread::decompose_numa_node(assigned_thread_ids_[0]));
    LOG(INFO) << "Initializing Logger-" << id_ << ". assigned " << assigned_thread_ids_.size()
        << " threads, starting from " << assigned_thread_ids_[0] << ", numa_node_="
        << static_cast<int>(numa_node_);

    // this is during initialization. no race.
    const savepoint::Savepoint &savepoint = engine_->get_savepoint_manager().get_savepoint_fast();
    current_file_path_ = construct_suffixed_log_path(savepoint.current_log_files_[id_]);
    // open the log file
    current_file_ = new fs::DirectIoFile(current_file_path_,
                                         engine_->get_options().log_.emulation_);
    CHECK_ERROR(current_file_->open(false, true, true, savepoint.empty()));
    uint64_t desired_length = savepoint.current_log_files_offset_durable_[id_];
    uint64_t current_length = fs::file_size(current_file_path_);
    if (desired_length < fs::file_size(current_file_path_)) {
        // there are non-durable regions as an incomplete remnant of previous execution.
        // probably there was a crash. in this case, we discard the non-durable regions.
        LOG(ERROR) << "Logger-" << id_ << "'s log file has a non-durable region. Probably there"
            << " was a crash. Will truncate it to " << desired_length << " from " << current_length;
        CHECK_ERROR(current_file_->truncate(desired_length, true));  // also sync right now
    }

    // which threads are assigned to me?
    for (auto thread_id : assigned_thread_ids_) {
        assigned_threads_.push_back(
            engine_->get_thread_pool().get_pimpl()->get_thread(thread_id));
    }

    // grab a buffer to do file I/O
    node_memory_ = engine_->get_memory_manager().get_node_memory(numa_node_);
    logger_buffer_ = node_memory_->get_logger_buffer_memory_piece(id_);
    LOG(INFO) << "Logger-" << id_ << " grabbed a I/O buffer. size=" << logger_buffer_.count_;

    // log file and buffer prepared. let's launch the logger thread
    logger_thread_.initialize("Logger-", id_,
                    std::thread(&Logger::handle_logger, this), std::chrono::milliseconds(10));
    return RET_OK;
}

ErrorStack Logger::uninitialize_once() {
    LOG(INFO) << "Uninitializing Logger-" << id_ << ".";
    ErrorStackBatch batch;
    logger_thread_.stop();
    if (current_file_) {
        current_file_->close();
        delete current_file_;
        current_file_ = nullptr;
    }
    return RET_OK;
}
void Logger::handle_logger() {
    LOG(INFO) << "Logger-" << id_ << " started. pin on NUMA node-" << static_cast<int>(numa_node_);
    ::numa_run_on_node(numa_node_);
    while (!logger_thread_.sleep()) {
        bool more_log_to_process = false;
        do {
            xct::Epoch min_skipped_epoch;
            bool any_log_processed = false;

            for (thread::Thread* the_thread : assigned_threads_) {
                if (logger_thread_.is_stop_requested()) {
                    break;
                }

                // we FIRST take offset_current_xct_begin with memory fence for a reason below.
                ThreadLogBuffer& buffer = the_thread->get_thread_log_buffer();
                const uint64_t current_xct_begin = buffer.get_offset_current_xct_begin();
                if (current_xct_begin == buffer.get_offset_durable()) {
                    VLOG(1) << "Thread-" << the_thread->get_thread_id() << " has no log to flush.";
                    continue;
                }
                VLOG(0) << "Thread-" << the_thread->get_thread_id() << " has "
                    << (current_xct_begin - buffer.get_offset_durable()) << " bytes logs to flush.";
                std::atomic_thread_fence(std::memory_order_acquire);

                // (if we need to) we consume epoch mark AFTER the fence. Thus, we don't miss a
                // case where the thread adds a new epoch mark after we read current_xct_begin.
                if (!buffer.logger_epoch_.is_valid() ||
                    (!buffer.logger_epoch_open_ended_
                        && buffer.logger_epoch_ends_ == buffer.offset_durable_)) {
                    // then, we need to consume an epoch mark. otherwise no logs to write out.
                    if (!buffer.consume_epoch_mark()) {
                        LOG(WARNING) << "Couldn't get epoch marks??";
                        continue;
                    }
                }

                ASSERT_ND(buffer.logger_epoch_.is_valid());
                ASSERT_ND(buffer.logger_epoch_open_ended_
                    || buffer.logger_epoch_ends_ != buffer.offset_durable_);
                if (buffer.logger_epoch_ < current_epoch_) {
                    LOG(FATAL) << "WHAT? Did I miss something? current_epoch_=" << current_epoch_
                        << ", buffer.logger_epoch_=" << buffer.logger_epoch_;
                } else if (buffer.logger_epoch_ > current_epoch_) {
                    // then skip it for now. we must finish the current epoch first.
                    VLOG(0) << "Skipped " << the_thread->get_thread_id() << "'s log. too recent.";
                    more_log_to_process = true;
                    if (!min_skipped_epoch.is_valid() || buffer.logger_epoch_ < min_skipped_epoch) {
                        min_skipped_epoch = buffer.logger_epoch_;
                    }
                    continue;
                } else {
                    // okay, let's write out logs in this buffer
                    more_log_to_process = true;
                    any_log_processed = true;
                    uint64_t upto_offset;
                    if (buffer.logger_epoch_open_ended_) {
                        // then, we write out upto current_xct_end. however, consider the case:
                        // 1) buffer has no mark (open ended) durable=10, cur_xct_end=20, ep=3.
                        // 2) this logger comes by with current_epoch=3. Sees no mark in buffer.
                        // 3) buffer receives new log in the meantime, ep=4, new mark added,
                        //   and cur_xct_end is now 30.
                        // 4) logger "okay, I will flush out all logs up to cur_xct_end(30)".
                        // 5) logger writes out all logs up to 30, as ep=3.
                        // To prevent this case, we first read cur_xct_end, take fence, then
                        // check epoch mark.
                        upto_offset = current_xct_begin;
                    } else {
                        upto_offset = buffer.logger_epoch_ends_;
                    }

                    COERCE_ERROR(write_log(&buffer, upto_offset));
                }
            }

            if (!any_log_processed && more_log_to_process) {
                switch_current_epoch(min_skipped_epoch);  // then we advance our current_epoch
            }
        } while (more_log_to_process && !logger_thread_.is_stop_requested());
    }
    LOG(INFO) << "Logger-" << id_ << " ended.";
}

void Logger::switch_current_epoch(const xct::Epoch& new_epoch) {
    ASSERT_ND(new_epoch.is_valid());
    ASSERT_ND(current_epoch_ < new_epoch);
    VLOG(0) << "Logger-" << id_ << " advances its current_epoch from " << current_epoch_
        << " to " << new_epoch;

    COERCE_ERROR(flush_log());

    ASSERT_ND(logger_buffer_cursor_ + sizeof(EpochMarkerLogType) <= logger_buffer_.size());
    EpochMarkerLogType* epoch_marker = reinterpret_cast<EpochMarkerLogType*>(
        logger_buffer_.get_block() + logger_buffer_cursor_);
    epoch_marker->header_.storage_id_ = 0;
    epoch_marker->header_.log_length_ = sizeof(EpochMarkerLogType);
    epoch_marker->header_.log_type_code_ = get_log_code<EpochMarkerLogType>();
    epoch_marker->new_epoch_ = new_epoch;
    epoch_marker->old_epoch_ = current_epoch_;
    logger_buffer_cursor_ += sizeof(EpochMarkerLogType);
    current_epoch_ = new_epoch;
}

ErrorStack Logger::flush_log() {
    if (logger_buffer_cursor_ == 0) {
        return RET_OK;
    }
    // we must write in 4kb unit. pad it with a filler.
    if (logger_buffer_cursor_ % FillerLogType::LOG_WRITE_UNIT_SIZE != 0) {
        uint64_t filler_size = FillerLogType::LOG_WRITE_UNIT_SIZE
            - (logger_buffer_cursor_ % FillerLogType::LOG_WRITE_UNIT_SIZE);
        FillerLogType* filler = reinterpret_cast<FillerLogType*>(
            logger_buffer_.get_block() + logger_buffer_cursor_);
        filler->header_.storage_id_ = 0;
        filler->header_.log_length_ = filler_size;
        filler->header_.log_type_code_ = get_log_code<FillerLogType>();
        logger_buffer_cursor_ += filler_size;
    }

    const uint64_t max_file_size = (engine_->get_options().log_.log_file_size_mb_ << 20);
    if (logger_buffer_cursor_ + current_file_offset_end_ > max_file_size) {
        // TODO(Hideaki) now switch the file.
    }

    CHECK_ERROR_CODE(current_file_->write(logger_buffer_cursor_, logger_buffer_));
    logger_buffer_cursor_ = 0;
    return RET_OK;
}

ErrorStack Logger::write_log(ThreadLogBuffer* buffer, uint64_t upto_offset) {
    uint64_t from_offset = buffer->get_offset_durable();
    ASSERT_ND(from_offset != upto_offset);
    if (from_offset > upto_offset) {
        // this means wrap-around in the buffer
        // let's write up to the end of the circular buffer, then from the beginning.
        VLOG(0) << "Buffer for " << buffer->get_thread_id() << " wraps around. " << from_offset
            << " to " << upto_offset;
        CHECK_ERROR(write_log(buffer, buffer->buffer_size_));
        ASSERT_ND(buffer->get_offset_durable() == 0);
        CHECK_ERROR(write_log(buffer, upto_offset));
        ASSERT_ND(buffer->get_offset_durable() == upto_offset);
        return RET_OK;
    }

    // write out with our I/O buffer.
    while (from_offset < upto_offset) {
        if (logger_buffer_.size() == logger_buffer_cursor_) {
            CHECK_ERROR(flush_log());
        }
        uint64_t write_size = std::min<uint64_t>(upto_offset - from_offset,
                                                 logger_buffer_.size() - logger_buffer_cursor_);
        std::memcpy(logger_buffer_.get_block() + logger_buffer_cursor_,
                    buffer->buffer_ + from_offset, write_size);
        logger_buffer_cursor_ += write_size;
        from_offset += write_size;
    }
    return RET_OK;
}

}  // namespace log
}  // namespace foedus