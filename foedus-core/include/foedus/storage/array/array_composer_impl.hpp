/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_STORAGE_ARRAY_ARRAY_COMPOSER_IMPL_HPP_
#define FOEDUS_STORAGE_ARRAY_ARRAY_COMPOSER_IMPL_HPP_

#include <stdint.h>

#include <iosfwd>
#include <string>

#include "foedus/compiler.hpp"
#include "foedus/fwd.hpp"
#include "foedus/memory/fwd.hpp"
#include "foedus/snapshot/fwd.hpp"
#include "foedus/storage/composer.hpp"
#include "foedus/storage/page.hpp"
#include "foedus/storage/array/array_id.hpp"
#include "foedus/storage/array/array_route.hpp"
#include "foedus/storage/array/fwd.hpp"
#include "foedus/xct/xct_id.hpp"

namespace foedus {
namespace storage {
namespace array {
/**
 * @brief Composer for an array storage.
 * @ingroup ARRAY
 * @details
 *
 * @note
 * This is a private implementation-details of \ref ARRAY, thus file name ends with _impl.
 * Do not include this header from a client program. There is no case client program needs to
 * access this internal class.
 */
class ArrayComposer final : public Composer {
 public:
  /**
   * Output of one compose() call, which are then combined in construct_root().
   * If the root page is leaf page (single-page array), this contains just one pointer to the root.
   * If not, this contains pointers to direct children of root.
   */
  struct RootInfoPage final {
    PageHeader          header_;

    /** Pointers to direct children of root. 0 if not set in this compose() */
    SnapshotPagePointer pointers_[kInteriorFanout];

    char                filler_[
      kPageSize
      - sizeof(PageHeader)
      - kInteriorFanout * sizeof(SnapshotPagePointer)];
  };

  ArrayComposer(
    Engine *engine,
    const ArrayPartitioner* partitioner,
    snapshot::SnapshotWriter* snapshot_writer,
    cache::SnapshotFileSet* previous_snapshot_files,
    const snapshot::Snapshot& new_snapshot);
  ~ArrayComposer() {}

  ArrayComposer() = delete;
  explicit ArrayComposer(const ArrayPartitioner& other) = delete;
  ArrayComposer& operator=(const ArrayPartitioner& other) = delete;

  std::string to_string() const override;
  void describe(std::ostream* o) const override;

  ErrorStack compose(
    snapshot::SortedBuffer* const* log_streams,
    uint32_t log_streams_count,
    const memory::AlignedMemorySlice& work_memory,
    Page* root_info_page) override;

  ErrorStack construct_root(
    const Page* const*  root_info_pages,
    uint32_t            root_info_pages_count,
    const memory::AlignedMemorySlice& work_memory,
    SnapshotPagePointer* new_root_page_pointer) override;

  uint64_t get_required_work_memory_size(
    snapshot::SortedBuffer** /*log_streams*/,
    uint32_t log_streams_count) const override {
    return sizeof(StreamStatus) * log_streams_count + kMaxLevels * kPageSize;
  }

 private:
  /** Represents one sorted input stream with its status. */
  struct StreamStatus {
    void init(snapshot::SortedBuffer* stream);
    ErrorCode next() ALWAYS_INLINE;
    void read_entry() ALWAYS_INLINE;
    const ArrayOverwriteLogType* get_entry() const ALWAYS_INLINE;

    snapshot::SortedBuffer* stream_;
    const char*     buffer_;
    uint64_t        buffer_size_;
    uint64_t        cur_absolute_pos_;
    uint64_t        cur_relative_pos_;
    uint64_t        end_absolute_pos_;
    ArrayOffset     cur_value_;
    xct::XctId      cur_xct_id_;
    uint32_t        cur_length_;
    bool            ended_;
  };

  ArrayStorage* const       storage_casted_;
  const uint8_t             levels_;
  /** Calculates LookupRoute from offset. */
  const LookupRouteFinder   route_finder_;

  /**
   * The offset interval a single page represents in each level. index=level.
   * So, offset_intervals[0] is the number of records in a leaf page.
   */
  uint64_t                  offset_intervals_[kMaxLevels];

  /////// variables for compose() BEGIN ///////
  // properties below are initialized in init_context() and used while compose() only
  RootInfoPage*             root_info_page_;
  StreamStatus*             inputs_;
  uint32_t                  inputs_count_;
  uint32_t                  ended_inputs_count_;
  /** root page image is separately maintained because we don't write it out in compose() */
  ArrayPage*                root_page_;

  /** path_[0] points to root, path_[1] points to its child we are now modifying..*/
  ArrayPage*                cur_path_[kMaxLevels];
  /** [0] means record ordinal in leaf, [1] in its parent page, [2]...*/
  LookupRoute               cur_route_;

  // this set of next_xxx indicates the min input to be applied next
  uint32_t                  next_input_;
  ArrayOffset               next_key_;
  xct::XctId                next_xct_id_;
  /** [0] means record ordinal in leaf, [1] in its parent page, [2]...*/
  LookupRoute               next_route_;
  ArrayOffset               next_page_starts_;
  ArrayOffset               next_page_ends_;

  /**
   * Offset that will be returned by \b next snapshot_writer_->allocate_new_page() call.
   * This is used only for sanity check.
   */
  memory::PagePoolOffset    alloc_inmemory_offset_;
  /**
   * Permanent page ID of the page allocated \b next.
   * We do know this beforehand because we will write out all pages we allocate.
   */
  SnapshotPagePointer       alloc_page_id;
  /////// variables during compose() END ///////

  // subroutines of compose()
  ErrorCode compose_init_context(
    RootInfoPage* root_info_page,
    const memory::AlignedMemorySlice& work_memory,
    snapshot::SortedBuffer* const* inputs,
    uint32_t inputs_count);
  /** sub routine of compose_init_context to initialize cur_xxx with the first page. */
  ErrorCode compose_init_context_cur_path();
  ErrorStack compose_strawman_tournament();

  ErrorCode advance() ALWAYS_INLINE;
  /** @return whether next key belongs to a different page */
  bool update_next_route() ALWAYS_INLINE;
  ErrorCode update_cur_path();
  const ArrayOverwriteLogType* get_next_entry() const ALWAYS_INLINE;

  /** @pre levels_ > level. */
  ArrayRange calculate_array_range(LookupRoute route, uint8_t level) const ALWAYS_INLINE;
};
static_assert(sizeof(ArrayComposer::RootInfoPage) == kPageSize, "incorrect sizeof(RootInfoPage)");
}  // namespace array
}  // namespace storage
}  // namespace foedus
#endif  // FOEDUS_STORAGE_ARRAY_ARRAY_COMPOSER_IMPL_HPP_