/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_XCT_XCT_INL_HPP_
#define FOEDUS_XCT_XCT_INL_HPP_
#include <foedus/assert_nd.hpp>
#include <foedus/compiler.hpp>
#include <foedus/error_stack.hpp>
#include <foedus/assorted/atomic_fences.hpp>
#include <foedus/storage/record.hpp>
#include <foedus/xct/xct.hpp>
#include <foedus/xct/xct_access.hpp>
// For log verification. Only in debug mode
#ifndef NDEBUG
#include <foedus/log/log_type_invoke.hpp>
#endif  // NDEBUG

#include <iosfwd>

/**
 * @file foedus/xct/xct_inl.hpp
 * @brief Inline functions of Xct.
 * @ingroup XCT
 */
namespace foedus {
namespace xct {

inline ErrorCode Xct::add_to_read_set(storage::Storage* storage, storage::Record* record) {
    ASSERT_ND(storage);
    ASSERT_ND(record);
    if (isolation_level_ == DIRTY_READ_PREFER_SNAPSHOT
        || isolation_level_ == DIRTY_READ_PREFER_VOLATILE) {
        return ERROR_CODE_OK;
    } else if (UNLIKELY(read_set_size_ >= max_read_set_size_)) {
        return ERROR_CODE_XCT_READ_SET_OVERFLOW;
    }

    ASSERT_ND(record->owner_id_.data_.components.epoch().is_valid());

    // If the record is locked, we will surely abort at commit time.
    // Rather, spin here to avoid wasted effort. In our engine, lock happens in commit time,
    // so no worry about deadlock or long wait.
    if (UNLIKELY(record->owner_id_.is_locked())) {
        record->owner_id_.spin_while_locked();
    }
    // yes, still some one might lock at _this_ moment, but we will see it at commit time.
    read_set_[read_set_size_].observed_owner_id_ = record->owner_id_;

    // for RCU protocol, make sure compiler/CPU don't reorder the data access before tag copy.
    // This is _consume rather than _acquire because it's fine to see stale information as far as
    // we don't access before the tag copy.
    assorted::memory_fence_consume();
    read_set_[read_set_size_].storage_ = storage;
    read_set_[read_set_size_].record_ = record;
    ++read_set_size_;
    return ERROR_CODE_OK;
}
inline ErrorCode Xct::add_to_write_set(storage::Storage* storage, storage::Record* record,
                                       void* log_entry) {
    ASSERT_ND(storage);
    ASSERT_ND(record);
    ASSERT_ND(log_entry);
    if (UNLIKELY(write_set_size_ >= max_write_set_size_)) {
        return ERROR_CODE_XCT_WRITE_SET_OVERFLOW;
    }

#ifndef NDEBUG
    log::invoke_assert_valid(log_entry);
#endif  // NDEBUG

    write_set_[write_set_size_].observed_owner_id_ = record->owner_id_;
    write_set_[write_set_size_].storage_ = storage;
    write_set_[write_set_size_].record_ = record;
    write_set_[write_set_size_].log_entry_ = log_entry;
    ++write_set_size_;
    return ERROR_CODE_OK;
}

}  // namespace xct
}  // namespace foedus
#endif  // FOEDUS_XCT_XCT_INL_HPP_
