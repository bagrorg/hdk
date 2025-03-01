/*
 * Copyright 2021 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file    ResultSet.cpp
 * @author  Alex Suhan <alex@mapd.com>
 * @brief   Basic constructors and methods of the row set interface.
 */

#include "ResultSet.h"
#include "DataMgr/Allocators/GpuAllocator.h"
#include "DataMgr/BufferMgr/BufferMgr.h"
#include "Execute.h"
#include "GpuMemUtils.h"
#include "InPlaceSort.h"
#include "OutputBufferInitialization.h"
#include "RuntimeFunctions.h"
#include "Shared/InlineNullValues.h"
#include "Shared/Intervals.h"
#include "Shared/SqlTypesLayout.h"
#include "Shared/checked_alloc.h"
#include "Shared/likely.h"
#include "Shared/parallel_sort.h"
#include "Shared/thread_count.h"
#include "Shared/threading.h"

#ifdef HAVE_TBB
#include "tbb/parallel_sort.h"
#endif

#include <algorithm>
#include <atomic>
#include <bitset>
#include <future>
#include <numeric>

constexpr int64_t uninitialized_cached_row_count{-1};

void ResultSet::keepFirstN(const size_t n) {
  invalidateCachedRowCount();
  keep_first_ = n;
}

void ResultSet::dropFirstN(const size_t n) {
  invalidateCachedRowCount();
  drop_first_ = n;
}

ResultSet::ResultSet(const std::vector<TargetInfo>& targets,
                     const ExecutorDeviceType device_type,
                     const QueryMemoryDescriptor& query_mem_desc,
                     const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                     Data_Namespace::DataMgr* data_mgr,
                     BufferProvider* buffer_provider,
                     const unsigned block_size,
                     const unsigned grid_size)
    : targets_(targets)
    , device_type_(device_type)
    , device_id_(-1)
    , query_mem_desc_(query_mem_desc)
    , crt_row_buff_idx_(0)
    , fetched_so_far_(0)
    , drop_first_(0)
    , keep_first_(0)
    , row_set_mem_owner_(row_set_mem_owner)
    , block_size_(block_size)
    , grid_size_(grid_size)
    , data_mgr_(data_mgr)
    , buffer_provider_(buffer_provider)
    , separate_varlen_storage_valid_(false)
    , just_explain_(false)
    , for_validation_only_(false)
    , cached_row_count_(uninitialized_cached_row_count) {}

ResultSet::ResultSet(const std::vector<TargetInfo>& targets,
                     const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info,
                     const std::vector<std::vector<const int8_t*>>& col_buffers,
                     const std::vector<std::vector<int64_t>>& frag_offsets,
                     const std::vector<int64_t>& consistent_frag_sizes,
                     const ExecutorDeviceType device_type,
                     const int device_id,
                     const QueryMemoryDescriptor& query_mem_desc,
                     const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                     Data_Namespace::DataMgr* data_mgr,
                     BufferProvider* buffer_provider,
                     const unsigned block_size,
                     const unsigned grid_size)
    : targets_(targets)
    , device_type_(device_type)
    , device_id_(device_id)
    , query_mem_desc_(query_mem_desc)
    , crt_row_buff_idx_(0)
    , fetched_so_far_(0)
    , drop_first_(0)
    , keep_first_(0)
    , row_set_mem_owner_(row_set_mem_owner)
    , block_size_(block_size)
    , grid_size_(grid_size)
    , lazy_fetch_info_(lazy_fetch_info)
    , col_buffers_{col_buffers}
    , frag_offsets_{frag_offsets}
    , consistent_frag_sizes_{consistent_frag_sizes}
    , data_mgr_(data_mgr)
    , buffer_provider_(buffer_provider)
    , separate_varlen_storage_valid_(false)
    , just_explain_(false)
    , for_validation_only_(false)
    , cached_row_count_(uninitialized_cached_row_count) {}

ResultSet::ResultSet(const std::shared_ptr<const Analyzer::Estimator> estimator,
                     const ExecutorDeviceType device_type,
                     const int device_id,
                     Data_Namespace::DataMgr* data_mgr,
                     BufferProvider* buffer_provider)
    : device_type_(device_type)
    , device_id_(device_id)
    , query_mem_desc_{}
    , crt_row_buff_idx_(0)
    , estimator_(estimator)
    , data_mgr_(data_mgr)
    , buffer_provider_(buffer_provider)
    , separate_varlen_storage_valid_(false)
    , just_explain_(false)
    , for_validation_only_(false)
    , cached_row_count_(uninitialized_cached_row_count) {
  if (device_type == ExecutorDeviceType::GPU) {
    device_estimator_buffer_ = GpuAllocator::allocGpuAbstractBuffer(
        buffer_provider_, estimator_->getBufferSize(), device_id_);
    buffer_provider_->zeroDeviceMem(device_estimator_buffer_->getMemoryPtr(),
                                    estimator_->getBufferSize(),
                                    device_id_);
  } else {
    host_estimator_buffer_ =
        static_cast<int8_t*>(checked_calloc(estimator_->getBufferSize(), 1));
  }
}

ResultSet::ResultSet(const std::string& explanation)
    : device_type_(ExecutorDeviceType::CPU)
    , device_id_(-1)
    , fetched_so_far_(0)
    , separate_varlen_storage_valid_(false)
    , explanation_(explanation)
    , just_explain_(true)
    , for_validation_only_(false)
    , cached_row_count_(uninitialized_cached_row_count) {}

ResultSet::ResultSet(int64_t queue_time_ms,
                     const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner)
    : device_type_(ExecutorDeviceType::CPU)
    , device_id_(-1)
    , fetched_so_far_(0)
    , row_set_mem_owner_(row_set_mem_owner)
    , timings_(QueryExecutionTimings{queue_time_ms, 0, 0})
    , separate_varlen_storage_valid_(false)
    , just_explain_(true)
    , for_validation_only_(false)
    , cached_row_count_(uninitialized_cached_row_count) {}

ResultSet::~ResultSet() {
  if (storage_) {
    if (!storage_->buff_is_provided_) {
      CHECK(storage_->getUnderlyingBuffer());
      free(storage_->getUnderlyingBuffer());
    }
  }
  for (auto& storage : appended_storage_) {
    if (storage && !storage->buff_is_provided_) {
      free(storage->getUnderlyingBuffer());
    }
  }
  if (host_estimator_buffer_) {
    CHECK(device_type_ == ExecutorDeviceType::CPU || device_estimator_buffer_);
    free(host_estimator_buffer_);
  }
  if (device_estimator_buffer_) {
    CHECK(data_mgr_);
    data_mgr_->free(device_estimator_buffer_);
  }
}

std::string ResultSet::summaryToString() const {
  std::ostringstream oss;
  oss << "Result Set Info" << std::endl;
  oss << "\tLayout: " << query_mem_desc_.queryDescTypeToString() << std::endl;
  oss << "\tColumns: " << colCount() << std::endl;
  oss << "\tRows: " << rowCount() << std::endl;
  oss << "\tEntry count: " << entryCount() << std::endl;
  const std::string is_empty = isEmpty() ? "True" : "False";
  oss << "\tIs empty: " << is_empty << std::endl;
  const std::string did_output_columnar = didOutputColumnar() ? "True" : "False;";
  oss << "\tColumnar: " << did_output_columnar << std::endl;
  oss << "\tLazy-fetched columns: " << getNumColumnsLazyFetched() << std::endl;
  const std::string is_direct_columnar_conversion_possible =
      isDirectColumnarConversionPossible() ? "True" : "False";
  oss << "\tDirect columnar conversion possible: "
      << is_direct_columnar_conversion_possible << std::endl;

  size_t num_columns_zero_copy_columnarizable{0};
  for (size_t target_idx = 0; target_idx < targets_.size(); target_idx++) {
    if (isZeroCopyColumnarConversionPossible(target_idx)) {
      num_columns_zero_copy_columnarizable++;
    }
  }
  oss << "\tZero-copy columnar conversion columns: "
      << num_columns_zero_copy_columnarizable << std::endl;

  oss << "\tPermutation size: " << permutation_.size() << std::endl;
  oss << "\tLimit: " << keep_first_ << std::endl;
  oss << "\tOffset: " << drop_first_ << std::endl;
  return oss.str();
}

ExecutorDeviceType ResultSet::getDeviceType() const {
  return device_type_;
}

const ResultSetStorage* ResultSet::allocateStorage() const {
  CHECK(!storage_);
  CHECK(row_set_mem_owner_);
  auto buff = row_set_mem_owner_->allocate(
      query_mem_desc_.getBufferSizeBytes(device_type_), /*thread_idx=*/0);
  storage_.reset(
      new ResultSetStorage(targets_, query_mem_desc_, buff, /*buff_is_provided=*/true));
  return storage_.get();
}

const ResultSetStorage* ResultSet::allocateStorage(
    int8_t* buff,
    const std::vector<int64_t>& target_init_vals,
    std::shared_ptr<VarlenOutputInfo> varlen_output_info) const {
  CHECK(buff);
  CHECK(!storage_);
  storage_.reset(new ResultSetStorage(targets_, query_mem_desc_, buff, true));
  // TODO: add both to the constructor
  storage_->target_init_vals_ = target_init_vals;
  if (varlen_output_info) {
    storage_->varlen_output_info_ = varlen_output_info;
  }
  return storage_.get();
}

const ResultSetStorage* ResultSet::allocateStorage(
    const std::vector<int64_t>& target_init_vals) const {
  CHECK(!storage_);
  CHECK(row_set_mem_owner_);
  auto buff = row_set_mem_owner_->allocate(
      query_mem_desc_.getBufferSizeBytes(device_type_), /*thread_idx=*/0);
  storage_.reset(
      new ResultSetStorage(targets_, query_mem_desc_, buff, /*buff_is_provided=*/true));
  storage_->target_init_vals_ = target_init_vals;
  return storage_.get();
}

size_t ResultSet::getCurrentRowBufferIndex() const {
  if (crt_row_buff_idx_ == 0) {
    throw std::runtime_error("current row buffer iteration index is undefined");
  }
  return crt_row_buff_idx_ - 1;
}

// Note: that.appended_storage_ does not get appended to this.
void ResultSet::append(ResultSet& that) {
  invalidateCachedRowCount();
  if (!that.storage_) {
    return;
  }
  appended_storage_.push_back(std::move(that.storage_));
  query_mem_desc_.setEntryCount(
      query_mem_desc_.getEntryCount() +
      appended_storage_.back()->query_mem_desc_.getEntryCount());
  chunks_.insert(chunks_.end(), that.chunks_.begin(), that.chunks_.end());
  col_buffers_.insert(
      col_buffers_.end(), that.col_buffers_.begin(), that.col_buffers_.end());
  frag_offsets_.insert(
      frag_offsets_.end(), that.frag_offsets_.begin(), that.frag_offsets_.end());
  consistent_frag_sizes_.insert(consistent_frag_sizes_.end(),
                                that.consistent_frag_sizes_.begin(),
                                that.consistent_frag_sizes_.end());
  chunk_iters_.insert(
      chunk_iters_.end(), that.chunk_iters_.begin(), that.chunk_iters_.end());
  if (separate_varlen_storage_valid_) {
    CHECK(that.separate_varlen_storage_valid_);
    serialized_varlen_buffer_.insert(serialized_varlen_buffer_.end(),
                                     that.serialized_varlen_buffer_.begin(),
                                     that.serialized_varlen_buffer_.end());
  }
  for (auto& buff : that.literal_buffers_) {
    literal_buffers_.push_back(std::move(buff));
  }
}

const ResultSetStorage* ResultSet::getStorage() const {
  return storage_.get();
}

size_t ResultSet::colCount() const {
  return just_explain_ ? 1 : targets_.size();
}

const hdk::ir::Type* ResultSet::colType(const size_t col_idx) const {
  if (just_explain_) {
    return hdk::ir::Context::defaultCtx().text();
  }
  CHECK_LT(col_idx, targets_.size());
  return targets_[col_idx].agg_kind == hdk::ir::AggType::kAvg
             ? hdk::ir::Context::defaultCtx().fp64()
             : targets_[col_idx].type;
}

StringDictionaryProxy* ResultSet::getStringDictionaryProxy(int const dict_id) const {
  constexpr bool with_generation = true;
  return row_set_mem_owner_->getOrAddStringDictProxy(dict_id, with_generation);
}

class ResultSet::CellCallback {
  StringDictionaryProxy::IdMap const id_map_;
  int64_t const null_int_;

 public:
  CellCallback(StringDictionaryProxy::IdMap&& id_map, int64_t const null_int)
      : id_map_(std::move(id_map)), null_int_(null_int) {}
  void operator()(int8_t const* const cell_ptr) const {
    using StringId = int32_t;
    StringId* const string_id_ptr =
        const_cast<StringId*>(reinterpret_cast<StringId const*>(cell_ptr));
    if (*string_id_ptr != null_int_) {
      *string_id_ptr = id_map_[*string_id_ptr];
    }
  }
};

void ResultSet::translateDictEncodedColumns(std::vector<TargetInfo> const& targets,
                                            size_t const start_idx) {
  if (storage_) {
    CHECK_EQ(targets.size(), storage_->targets_.size());
    RowIterationState state;
    for (size_t target_idx = start_idx; target_idx < targets.size(); ++target_idx) {
      auto type_lhs = targets[target_idx].type;
      if (type_lhs->isExtDictionary()) {
        auto type_rhs = storage_->targets_[target_idx].type;
        CHECK(type_rhs->isExtDictionary());
        auto lhs_dict_id = type_lhs->as<hdk::ir::ExtDictionaryType>()->dictId();
        auto rhs_dict_id = type_rhs->as<hdk::ir::ExtDictionaryType>()->dictId();
        if (lhs_dict_id != rhs_dict_id) {
          auto* const sdp_lhs = getStringDictionaryProxy(lhs_dict_id);
          CHECK(sdp_lhs);
          auto const* const sdp_rhs = getStringDictionaryProxy(rhs_dict_id);
          CHECK(sdp_rhs);
          state.cur_target_idx_ = target_idx;
          CellCallback const translate_string_ids(sdp_lhs->transientUnion(*sdp_rhs),
                                                  inline_int_null_value(type_rhs));
          eachCellInColumn(state, translate_string_ids);
          const_cast<TargetInfo&>(storage_->targets_[target_idx]).type =
              type_rhs->ctx().extDict(
                  type_rhs->as<hdk::ir::ExtDictionaryType>()->elemType(),
                  lhs_dict_id,
                  type_rhs->size());
        }
      }
    }
  }
}

// For each cell in column target_idx, callback func with pointer to datum.
// This currently assumes the column type is a dictionary-encoded string, but this logic
// can be generalized to other types.
void ResultSet::eachCellInColumn(RowIterationState& state, CellCallback const& func) {
  size_t const target_idx = state.cur_target_idx_;
  QueryMemoryDescriptor& storage_qmd = storage_->query_mem_desc_;
  CHECK_LT(target_idx, lazy_fetch_info_.size());
  auto& col_lazy_fetch = lazy_fetch_info_[target_idx];
  CHECK(col_lazy_fetch.is_lazily_fetched);
  int const target_size = storage_->targets_[target_idx].type->size();
  CHECK_LT(0, target_size) << storage_->targets_[target_idx].toString();
  size_t const nrows = storage_->binSearchRowCount();
  if (storage_qmd.didOutputColumnar()) {
    // Logic based on ResultSet::ColumnWiseTargetAccessor::initializeOffsetsForStorage()
    if (state.buf_ptr_ == nullptr) {
      state.buf_ptr_ = get_cols_ptr(storage_->buff_, storage_qmd);
      state.compact_sz1_ = storage_qmd.getPaddedSlotWidthBytes(state.agg_idx_)
                               ? storage_qmd.getPaddedSlotWidthBytes(state.agg_idx_)
                               : query_mem_desc_.getEffectiveKeyWidth();
    }
    for (size_t j = state.prev_target_idx_; j < state.cur_target_idx_; ++j) {
      size_t const next_target_idx = j + 1;  // Set state to reflect next target_idx j+1
      state.buf_ptr_ = advance_to_next_columnar_target_buff(
          state.buf_ptr_, storage_qmd, state.agg_idx_);
      auto const& next_agg_info = storage_->targets_[next_target_idx];
      state.agg_idx_ =
          advance_slot(state.agg_idx_, next_agg_info, separate_varlen_storage_valid_);
      state.compact_sz1_ = storage_qmd.getPaddedSlotWidthBytes(state.agg_idx_)
                               ? storage_qmd.getPaddedSlotWidthBytes(state.agg_idx_)
                               : query_mem_desc_.getEffectiveKeyWidth();
    }
    for (size_t i = 0; i < nrows; ++i) {
      int8_t const* const pos_ptr = state.buf_ptr_ + i * state.compact_sz1_;
      int64_t pos = read_int_from_buff(pos_ptr, target_size);
      CHECK_GE(pos, 0);
      auto& frag_col_buffers = getColumnFrag(0, target_idx, pos);
      CHECK_LT(size_t(col_lazy_fetch.local_col_id), frag_col_buffers.size());
      int8_t const* const col_frag = frag_col_buffers[col_lazy_fetch.local_col_id];
      func(col_frag + pos * target_size);
    }
  } else {
    size_t const key_bytes_with_padding =
        align_to_int64(get_key_bytes_rowwise(storage_qmd));
    for (size_t i = 0; i < nrows; ++i) {
      int8_t const* const keys_ptr = row_ptr_rowwise(storage_->buff_, storage_qmd, i);
      int8_t const* const rowwise_target_ptr = keys_ptr + key_bytes_with_padding;
      int64_t pos = *reinterpret_cast<int64_t const*>(rowwise_target_ptr);
      auto& frag_col_buffers = getColumnFrag(0, target_idx, pos);
      CHECK_LT(size_t(col_lazy_fetch.local_col_id), frag_col_buffers.size());
      int8_t const* const col_frag = frag_col_buffers[col_lazy_fetch.local_col_id];
      func(col_frag + pos * target_size);
    }
  }
}

namespace {

size_t get_truncated_row_count(size_t total_row_count, size_t limit, size_t offset) {
  if (total_row_count < offset) {
    return 0;
  }

  size_t total_truncated_row_count = total_row_count - offset;

  if (limit) {
    return std::min(total_truncated_row_count, limit);
  }

  return total_truncated_row_count;
}

}  // namespace

size_t ResultSet::rowCountImpl(const bool force_parallel) const {
  if (just_explain_) {
    return 1;
  }
  if (!permutation_.empty()) {
    // keep_first_ corresponds to SQL LIMIT
    // drop_first_ corresponds to SQL OFFSET
    return get_truncated_row_count(permutation_.size(), keep_first_, drop_first_);
  }
  if (!storage_) {
    return 0;
  }
  CHECK(permutation_.empty());
  if (query_mem_desc_.getQueryDescriptionType() == QueryDescriptionType::Projection) {
    return binSearchRowCount();
  }

  constexpr size_t auto_parallel_row_count_threshold{20000UL};
  if (force_parallel || entryCount() >= auto_parallel_row_count_threshold) {
    return parallelRowCount();
  }
  std::lock_guard<std::mutex> lock(row_iteration_mutex_);
  moveToBegin();
  size_t row_count{0};
  while (true) {
    auto crt_row = getNextRowUnlocked(false, false);
    if (crt_row.empty()) {
      break;
    }
    ++row_count;
  }
  moveToBegin();
  return row_count;
}

size_t ResultSet::rowCount(const bool force_parallel) const {
  // cached_row_count_ is atomic, so fetch it into a local variable first
  // to avoid repeat fetches
  const int64_t cached_row_count = cached_row_count_;
  if (cached_row_count != uninitialized_cached_row_count) {
    CHECK_GE(cached_row_count, 0);
    return cached_row_count;
  }
  setCachedRowCount(rowCountImpl(force_parallel));
  return cached_row_count_;
}

void ResultSet::invalidateCachedRowCount() const {
  cached_row_count_ = uninitialized_cached_row_count;
}

void ResultSet::setCachedRowCount(const size_t row_count) const {
  const int64_t signed_row_count = static_cast<int64_t>(row_count);
  const int64_t old_cached_row_count = cached_row_count_.exchange(signed_row_count);
  CHECK(old_cached_row_count == uninitialized_cached_row_count ||
        old_cached_row_count == signed_row_count);
}

size_t ResultSet::binSearchRowCount() const {
  if (!storage_) {
    return 0;
  }

  size_t row_count = storage_->binSearchRowCount();
  for (auto& s : appended_storage_) {
    row_count += s->binSearchRowCount();
  }

  return get_truncated_row_count(row_count, getLimit(), drop_first_);
}

size_t ResultSet::parallelRowCount() const {
  using namespace threading;
  auto execute_parallel_row_count = [this, query_id = logger::query_id()](
                                        const blocked_range<size_t>& r,
                                        size_t row_count) {
    auto qid_scope_guard = logger::set_thread_local_query_id(query_id);
    for (size_t i = r.begin(); i < r.end(); ++i) {
      if (!isRowAtEmpty(i)) {
        ++row_count;
      }
    }
    return row_count;
  };
  const auto row_count = parallel_reduce(blocked_range<size_t>(0, entryCount()),
                                         size_t(0),
                                         execute_parallel_row_count,
                                         std::plus<int>());
  return get_truncated_row_count(row_count, getLimit(), drop_first_);
}

bool ResultSet::isEmpty() const {
  // To simplify this function and de-dup logic with ResultSet::rowCount()
  // (mismatches between the two were causing bugs), we modified this function
  // to simply fetch rowCount(). The potential downside of this approach is that
  // in some cases more work will need to be done, as we can't just stop at the first row.
  // Mitigating that for most cases is the following:
  // 1) rowCount() is cached, so the logic for actually computing row counts will run only
  // once
  //    per result set.
  // 2) If the cache is empty (cached_row_count_ == -1), rowCount() will use parallel
  //    methods if deemed appropriate, which in many cases could be faster for a sparse
  //    large result set that single-threaded iteration from the beginning
  // 3) Often where isEmpty() is needed, rowCount() is also needed. Since the first call
  // to rowCount()
  //    will be cached, there is no extra overhead in these cases

  return rowCount() == size_t(0);
}

bool ResultSet::definitelyHasNoRows() const {
  return (!storage_ && !estimator_ && !just_explain_) || cached_row_count_ == 0;
}

const QueryMemoryDescriptor& ResultSet::getQueryMemDesc() const {
  CHECK(storage_);
  return storage_->query_mem_desc_;
}

const std::vector<TargetInfo>& ResultSet::getTargetInfos() const {
  return targets_;
}

const std::vector<int64_t>& ResultSet::getTargetInitVals() const {
  CHECK(storage_);
  return storage_->target_init_vals_;
}

int8_t* ResultSet::getDeviceEstimatorBuffer() const {
  CHECK(device_type_ == ExecutorDeviceType::GPU);
  CHECK(device_estimator_buffer_);
  return device_estimator_buffer_->getMemoryPtr();
}

int8_t* ResultSet::getHostEstimatorBuffer() const {
  return host_estimator_buffer_;
}

void ResultSet::syncEstimatorBuffer() const {
  CHECK(device_type_ == ExecutorDeviceType::GPU);
  CHECK(!host_estimator_buffer_);
  CHECK_EQ(size_t(0), estimator_->getBufferSize() % sizeof(int64_t));
  host_estimator_buffer_ =
      static_cast<int8_t*>(checked_calloc(estimator_->getBufferSize(), 1));
  CHECK(device_estimator_buffer_);
  auto device_buffer_ptr = device_estimator_buffer_->getMemoryPtr();
  buffer_provider_->copyFromDevice(
      host_estimator_buffer_, device_buffer_ptr, estimator_->getBufferSize(), device_id_);
}

void ResultSet::setQueueTime(const int64_t queue_time) {
  timings_.executor_queue_time = queue_time;
}

void ResultSet::setKernelQueueTime(const int64_t kernel_queue_time) {
  timings_.kernel_queue_time = kernel_queue_time;
}

void ResultSet::addCompilationQueueTime(const int64_t compilation_queue_time) {
  timings_.compilation_queue_time += compilation_queue_time;
}

int64_t ResultSet::getQueueTime() const {
  return timings_.executor_queue_time + timings_.kernel_queue_time +
         timings_.compilation_queue_time;
}

void ResultSet::moveToBegin() const {
  crt_row_buff_idx_ = 0;
  fetched_so_far_ = 0;
}

bool ResultSet::isTruncated() const {
  return keep_first_ + drop_first_;
}

bool ResultSet::isExplain() const {
  return just_explain_;
}

void ResultSet::setValidationOnlyRes() {
  for_validation_only_ = true;
}

bool ResultSet::isValidationOnlyRes() const {
  return for_validation_only_;
}

int ResultSet::getDeviceId() const {
  return device_id_;
}

QueryMemoryDescriptor ResultSet::fixupQueryMemoryDescriptor(
    const QueryMemoryDescriptor& query_mem_desc) {
  auto query_mem_desc_copy = query_mem_desc;
  query_mem_desc_copy.resetGroupColWidths(
      std::vector<int8_t>(query_mem_desc_copy.getGroupbyColCount(), 8));
  if (query_mem_desc.didOutputColumnar()) {
    return query_mem_desc_copy;
  }
  query_mem_desc_copy.alignPaddedSlots();
  return query_mem_desc_copy;
}

template <typename T>
void sort_on_cpu(T* val_buff,
                 PermutationView pv,
                 const hdk::ir::OrderEntry& order_entry) {
  int64_t begin = 0;
  int64_t end = pv.size() - 1;

  if (order_entry.nulls_first) {
    while (end >= begin) {
      auto val = val_buff[end];
      if (val == inline_null_value<T>()) {
        if (val_buff[begin] != inline_null_value<T>()) {
          std::swap(val_buff[begin], val_buff[end]);
          std::swap(pv[begin], pv[end]);
          --end;
        }
        ++begin;
      } else {
        --end;
      }
    }
    end = pv.size() - 1;
  } else {
    while (end >= begin) {
      auto val = val_buff[begin];
      if (val == inline_null_value<T>()) {
        if (val_buff[end] != inline_null_value<T>()) {
          std::swap(val_buff[end], val_buff[begin]);
          std::swap(pv[end], pv[begin]);
          ++begin;
        }
        --end;
      } else {
        ++begin;
      }
    }
    begin = 0;
  }

  if (order_entry.is_desc) {
    parallel_sort_by_key(val_buff + begin,
                         pv.begin() + begin,
                         (size_t)(end - begin + 1),
                         std::greater<T>());
  } else {
    parallel_sort_by_key(
        val_buff + begin, pv.begin() + begin, (size_t)(end - begin + 1), std::less<T>());
  }
}

void sort_onecol_cpu(int8_t* val_buff,
                     PermutationView pv,
                     const hdk::ir::Type* type,
                     const size_t slot_width,
                     const hdk::ir::OrderEntry& order_entry) {
  if (type->isInteger() || type->isDecimal()) {
    switch (slot_width) {
      case 1:
        sort_on_cpu(reinterpret_cast<int8_t*>(val_buff), pv, order_entry);
        break;
      case 2:
        sort_on_cpu(reinterpret_cast<int16_t*>(val_buff), pv, order_entry);
        break;
      case 4:
        sort_on_cpu(reinterpret_cast<int32_t*>(val_buff), pv, order_entry);
        break;
      case 8:
        sort_on_cpu(reinterpret_cast<int64_t*>(val_buff), pv, order_entry);
        break;
      default:
        CHECK(false);
    }
  } else if (type->isFloatingPoint()) {
    switch (slot_width) {
      case 4:
        sort_on_cpu(reinterpret_cast<float*>(val_buff), pv, order_entry);
        break;
      case 8:
        sort_on_cpu(reinterpret_cast<double*>(val_buff), pv, order_entry);
        break;
      default:
        CHECK(false);
    }
  } else {
    UNREACHABLE() << "Unsupported element type";
  }
}

void ResultSet::sort(const std::list<hdk::ir::OrderEntry>& order_entries,
                     size_t top_n,
                     const Executor* executor) {
  auto timer = DEBUG_TIMER(__func__);

  if (!storage_) {
    return;
  }
  invalidateCachedRowCount();
  CHECK(!targets_.empty());
#ifdef HAVE_CUDA
  if (canUseFastBaselineSort(order_entries, top_n)) {
    baselineSort(order_entries, top_n, executor);
    return;
  }
#endif  // HAVE_CUDA
  if (query_mem_desc_.sortOnGpu()) {
    try {
      radixSortOnGpu(executor ? executor->getConfig() : Config(), order_entries);
    } catch (const OutOfMemory&) {
      LOG(WARNING) << "Out of GPU memory during sort, finish on CPU";
      radixSortOnCpu(order_entries);
    } catch (const std::bad_alloc&) {
      LOG(WARNING) << "Out of GPU memory during sort, finish on CPU";
      radixSortOnCpu(order_entries);
    }
    return;
  }
  // This check isn't strictly required, but allows the index buffer to be 32-bit.
  if (query_mem_desc_.getEntryCount() > std::numeric_limits<uint32_t>::max()) {
    throw RowSortException("Sorting more than 4B elements not supported");
  }

  CHECK(permutation_.empty());

  if (top_n && executor && executor->getConfig().exec.parallel_top_min < entryCount()) {
    if (executor->getConfig().exec.watchdog.enable &&
        executor->getConfig().exec.watchdog.parallel_top_max < entryCount()) {
      throw WatchdogException("Sorting the result would be too slow");
    }
    parallelTop(order_entries, top_n, executor);
  } else {
    if (executor && executor->getConfig().exec.watchdog.enable &&
        executor->getConfig().exec.group_by.baseline_threshold < entryCount()) {
      throw WatchdogException("Sorting the result would be too slow");
    }

    if (top_n == 0 && size_t(1) == order_entries.size() &&
        (!executor || executor->getConfig().rs.enable_direct_columnarization) &&
        isDirectColumnarConversionPossible() && query_mem_desc_.didOutputColumnar() &&
        query_mem_desc_.getQueryDescriptionType() == QueryDescriptionType::Projection) {
      const auto& order_entry = order_entries.front();
      const auto target_idx = order_entry.tle_no - 1;
      const auto& lazy_fetch_info = getLazyFetchInfo();
      bool is_not_lazy =
          lazy_fetch_info.empty() || !lazy_fetch_info[target_idx].is_lazily_fetched;
      const auto entry_type = get_compact_type(targets_[target_idx]);
      const auto slot_width = query_mem_desc_.getPaddedSlotWidthBytes(target_idx);
      if (is_not_lazy && slot_width > 0 && entry_type->isNumber()) {
        const size_t buf_size = query_mem_desc_.getEntryCount() * slot_width;
        // std::vector<int8_t> sortkey_val_buff(buf_size);
        std::unique_ptr<int8_t[]> sortkey_val_buff(new int8_t[buf_size]);
        copyColumnIntoBuffer(
            target_idx, reinterpret_cast<int8_t*>(&sortkey_val_buff[0]), buf_size);
        permutation_.resize(query_mem_desc_.getEntryCount());
        PermutationView pv(permutation_.data(), 0, permutation_.size());
        pv = initPermutationBuffer(pv, 0, permutation_.size());
        sort_onecol_cpu(reinterpret_cast<int8_t*>(&sortkey_val_buff[0]),
                        pv,
                        entry_type,
                        slot_width,
                        order_entry);
        if (pv.size() < permutation_.size()) {
          permutation_.resize(pv.size());
          permutation_.shrink_to_fit();
        }
        return;
      }
    }
    permutation_.resize(query_mem_desc_.getEntryCount());
    // PermutationView is used to share common API with parallelTop().
    PermutationView pv(permutation_.data(), 0, permutation_.size());
    pv = initPermutationBuffer(pv, 0, permutation_.size());
    if (top_n == 0) {
      top_n = pv.size();  // top_n == 0 implies a full sort
    }
    pv = topPermutation(
        pv, top_n, createComparator(order_entries, pv, executor, false), false);
    if (pv.size() < permutation_.size()) {
      permutation_.resize(pv.size());
      permutation_.shrink_to_fit();
    }
  }
}

#ifdef HAVE_CUDA
void ResultSet::baselineSort(const std::list<hdk::ir::OrderEntry>& order_entries,
                             const size_t top_n,
                             const Executor* executor) {
  auto timer = DEBUG_TIMER(__func__);
  // If we only have on GPU, it's usually faster to do multi-threaded radix sort on CPU
  if (getGpuCount() > 1) {
    try {
      doBaselineSort(ExecutorDeviceType::GPU, order_entries, top_n, executor);
    } catch (...) {
      doBaselineSort(ExecutorDeviceType::CPU, order_entries, top_n, executor);
    }
  } else {
    doBaselineSort(ExecutorDeviceType::CPU, order_entries, top_n, executor);
  }
}
#endif  // HAVE_CUDA

// Append non-empty indexes i in [begin,end) from findStorage(i) to permutation.
PermutationView ResultSet::initPermutationBuffer(PermutationView permutation,
                                                 PermutationIdx const begin,
                                                 PermutationIdx const end) const {
  auto timer = DEBUG_TIMER(__func__);
  for (PermutationIdx i = begin; i < end; ++i) {
    const auto storage_lookup_result = findStorage(i);
    const auto lhs_storage = storage_lookup_result.storage_ptr;
    const auto off = storage_lookup_result.fixedup_entry_idx;
    CHECK(lhs_storage);
    if (!lhs_storage->isEmptyEntry(off)) {
      permutation.push_back(i);
    }
  }
  return permutation;
}

const Permutation& ResultSet::getPermutationBuffer() const {
  return permutation_;
}

void ResultSet::parallelTop(const std::list<hdk::ir::OrderEntry>& order_entries,
                            const size_t top_n,
                            const Executor* executor) {
  auto timer = DEBUG_TIMER(__func__);
  const size_t nthreads = cpu_threads();

  // Split permutation_ into nthreads subranges and top-sort in-place.
  permutation_.resize(query_mem_desc_.getEntryCount());
  std::vector<PermutationView> permutation_views(nthreads);
  threading::task_group top_sort_threads;
  for (auto interval : makeIntervals<PermutationIdx>(0, permutation_.size(), nthreads)) {
    top_sort_threads.run([this,
                          &order_entries,
                          &permutation_views,
                          top_n,
                          executor,
                          query_id = logger::query_id(),
                          interval] {
      auto qid_scope_guard = logger::set_thread_local_query_id(query_id);
      PermutationView pv(permutation_.data() + interval.begin, 0, interval.size());
      pv = initPermutationBuffer(pv, interval.begin, interval.end);
      const auto compare = createComparator(order_entries, pv, executor, true);
      permutation_views[interval.index] = topPermutation(pv, top_n, compare, true);
    });
  }
  top_sort_threads.wait();

  // In case you are considering implementing a parallel reduction, note that the
  // ResultSetComparator constructor is O(N) in order to materialize some of the aggregate
  // columns as necessary to perform a comparison. This cost is why reduction is chosen to
  // be serial instead; only one more Comparator is needed below.

  // Left-copy disjoint top-sorted subranges into one contiguous range.
  // ++++....+++.....+++++...  ->  ++++++++++++............
  auto end = permutation_.begin() + permutation_views.front().size();
  for (size_t i = 1; i < nthreads; ++i) {
    std::copy(permutation_views[i].begin(), permutation_views[i].end(), end);
    end += permutation_views[i].size();
  }

  // Top sort final range.
  PermutationView pv(permutation_.data(), end - permutation_.begin());
  const auto compare = createComparator(order_entries, pv, executor, false);
  pv = topPermutation(pv, top_n, compare, false);
  permutation_.resize(pv.size());
  permutation_.shrink_to_fit();
}

std::pair<size_t, size_t> ResultSet::getStorageIndex(const size_t entry_idx) const {
  size_t fixedup_entry_idx = entry_idx;
  auto entry_count = storage_->query_mem_desc_.getEntryCount();
  const bool is_rowwise_layout = !storage_->query_mem_desc_.didOutputColumnar();
  if (fixedup_entry_idx < entry_count) {
    return {0, fixedup_entry_idx};
  }
  fixedup_entry_idx -= entry_count;
  for (size_t i = 0; i < appended_storage_.size(); ++i) {
    const auto& desc = appended_storage_[i]->query_mem_desc_;
    CHECK_NE(is_rowwise_layout, desc.didOutputColumnar());
    entry_count = desc.getEntryCount();
    if (fixedup_entry_idx < entry_count) {
      return {i + 1, fixedup_entry_idx};
    }
    fixedup_entry_idx -= entry_count;
  }
  UNREACHABLE() << "entry_idx = " << entry_idx << ", query_mem_desc_.getEntryCount() = "
                << query_mem_desc_.getEntryCount();
  return {};
}

template struct ResultSet::ResultSetComparator<ResultSet::RowWiseTargetAccessor>;
template struct ResultSet::ResultSetComparator<ResultSet::ColumnWiseTargetAccessor>;

ResultSet::StorageLookupResult ResultSet::findStorage(const size_t entry_idx) const {
  auto [stg_idx, fixedup_entry_idx] = getStorageIndex(entry_idx);
  return {stg_idx ? appended_storage_[stg_idx - 1].get() : storage_.get(),
          fixedup_entry_idx,
          stg_idx};
}

template <typename BUFFER_ITERATOR_TYPE>
void ResultSet::ResultSetComparator<
    BUFFER_ITERATOR_TYPE>::materializeCountDistinctColumns() {
  for (const auto& order_entry : order_entries_) {
    if (is_distinct_target(result_set_->targets_[order_entry.tle_no - 1])) {
      count_distinct_materialized_buffers_.emplace_back(
          materializeCountDistinctColumn(order_entry));
    }
  }
}

template <typename BUFFER_ITERATOR_TYPE>
ResultSet::ApproxQuantileBuffers ResultSet::ResultSetComparator<
    BUFFER_ITERATOR_TYPE>::materializeApproxQuantileColumns() const {
  ResultSet::ApproxQuantileBuffers approx_quantile_materialized_buffers;
  for (const auto& order_entry : order_entries_) {
    if (result_set_->targets_[order_entry.tle_no - 1].agg_kind ==
        hdk::ir::AggType::kApproxQuantile) {
      approx_quantile_materialized_buffers.emplace_back(
          materializeApproxQuantileColumn(order_entry));
    }
  }
  return approx_quantile_materialized_buffers;
}

template <typename BUFFER_ITERATOR_TYPE>
std::vector<int64_t>
ResultSet::ResultSetComparator<BUFFER_ITERATOR_TYPE>::materializeCountDistinctColumn(
    const hdk::ir::OrderEntry& order_entry) const {
  const size_t num_storage_entries = result_set_->query_mem_desc_.getEntryCount();
  std::vector<int64_t> count_distinct_materialized_buffer(num_storage_entries);
  const CountDistinctDescriptor count_distinct_descriptor =
      result_set_->query_mem_desc_.getCountDistinctDescriptor(order_entry.tle_no - 1);
  const size_t num_non_empty_entries = permutation_.size();

  const auto work = [&, query_id = logger::query_id()](const size_t start,
                                                       const size_t end) {
    auto qid_scope_guard = logger::set_thread_local_query_id(query_id);
    for (size_t i = start; i < end; ++i) {
      const PermutationIdx permuted_idx = permutation_[i];
      const auto storage_lookup_result = result_set_->findStorage(permuted_idx);
      const auto storage = storage_lookup_result.storage_ptr;
      const auto off = storage_lookup_result.fixedup_entry_idx;
      const auto value = buffer_itr_.getColumnInternal(
          storage->buff_, off, order_entry.tle_no - 1, storage_lookup_result);
      count_distinct_materialized_buffer[permuted_idx] =
          count_distinct_set_size(value.i1, count_distinct_descriptor);
    }
  };
  // TODO(tlm): Allow use of tbb after we determine how to easily encapsulate the choice
  // between thread pool types
  if (single_threaded_) {
    work(0, num_non_empty_entries);
  } else {
    threading::task_group thread_pool;
    for (auto interval : makeIntervals<size_t>(0, num_non_empty_entries, cpu_threads())) {
      thread_pool.run([=] { work(interval.begin, interval.end); });
    }
    thread_pool.wait();
  }
  return count_distinct_materialized_buffer;
}

double ResultSet::calculateQuantile(quantile::TDigest* const t_digest) {
  static_assert(sizeof(int64_t) == sizeof(quantile::TDigest*));
  CHECK(t_digest);
  t_digest->mergeBuffer();
  double const quantile = t_digest->quantile();
  return boost::math::isnan(quantile) ? NULL_DOUBLE : quantile;
}

template <typename BUFFER_ITERATOR_TYPE>
ResultSet::ApproxQuantileBuffers::value_type
ResultSet::ResultSetComparator<BUFFER_ITERATOR_TYPE>::materializeApproxQuantileColumn(
    const hdk::ir::OrderEntry& order_entry) const {
  ResultSet::ApproxQuantileBuffers::value_type materialized_buffer(
      result_set_->query_mem_desc_.getEntryCount());
  const size_t size = permutation_.size();
  const auto work = [&, query_id = logger::query_id()](const size_t start,
                                                       const size_t end) {
    auto qid_scope_guard = logger::set_thread_local_query_id(query_id);
    for (size_t i = start; i < end; ++i) {
      const PermutationIdx permuted_idx = permutation_[i];
      const auto storage_lookup_result = result_set_->findStorage(permuted_idx);
      const auto storage = storage_lookup_result.storage_ptr;
      const auto off = storage_lookup_result.fixedup_entry_idx;
      const auto value = buffer_itr_.getColumnInternal(
          storage->buff_, off, order_entry.tle_no - 1, storage_lookup_result);
      materialized_buffer[permuted_idx] =
          value.i1 ? calculateQuantile(reinterpret_cast<quantile::TDigest*>(value.i1))
                   : NULL_DOUBLE;
    }
  };
  if (single_threaded_) {
    work(0, size);
  } else {
    threading::task_group thread_pool;
    for (auto interval : makeIntervals<size_t>(0, size, cpu_threads())) {
      thread_pool.run([=] { work(interval.begin, interval.end); });
    }
    thread_pool.wait();
  }
  return materialized_buffer;
}

template <typename BUFFER_ITERATOR_TYPE>
bool ResultSet::ResultSetComparator<BUFFER_ITERATOR_TYPE>::operator()(
    const PermutationIdx lhs,
    const PermutationIdx rhs) const {
  // NB: The compare function must define a strict weak ordering, otherwise
  // std::sort will trigger a segmentation fault (or corrupt memory).
  const auto lhs_storage_lookup_result = result_set_->findStorage(lhs);
  const auto rhs_storage_lookup_result = result_set_->findStorage(rhs);
  const auto lhs_storage = lhs_storage_lookup_result.storage_ptr;
  const auto rhs_storage = rhs_storage_lookup_result.storage_ptr;
  const auto fixedup_lhs = lhs_storage_lookup_result.fixedup_entry_idx;
  const auto fixedup_rhs = rhs_storage_lookup_result.fixedup_entry_idx;
  size_t materialized_count_distinct_buffer_idx{0};
  size_t materialized_approx_quantile_buffer_idx{0};

  for (const auto& order_entry : order_entries_) {
    CHECK_GE(order_entry.tle_no, 1);
    const auto& agg_info = result_set_->targets_[order_entry.tle_no - 1];
    const auto entry_type = get_compact_type(agg_info);
    bool float_argument_input = takes_float_argument(agg_info);
    // Need to determine if the float value has been stored as float
    // or if it has been compacted to a different (often larger 8 bytes)
    // in distributed case the floats are actually 4 bytes
    // TODO the above takes_float_argument() is widely used wonder if this problem
    // exists elsewhere
    if (entry_type->isFp32()) {
      const auto is_col_lazy =
          !result_set_->lazy_fetch_info_.empty() &&
          result_set_->lazy_fetch_info_[order_entry.tle_no - 1].is_lazily_fetched;
      if (result_set_->query_mem_desc_.getPaddedSlotWidthBytes(order_entry.tle_no - 1) ==
          sizeof(float)) {
        float_argument_input =
            result_set_->query_mem_desc_.didOutputColumnar() ? !is_col_lazy : true;
      }
    }

    if (UNLIKELY(is_distinct_target(agg_info))) {
      CHECK_LT(materialized_count_distinct_buffer_idx,
               count_distinct_materialized_buffers_.size());

      const auto& count_distinct_materialized_buffer =
          count_distinct_materialized_buffers_[materialized_count_distinct_buffer_idx];
      const auto lhs_sz = count_distinct_materialized_buffer[lhs];
      const auto rhs_sz = count_distinct_materialized_buffer[rhs];
      ++materialized_count_distinct_buffer_idx;
      if (lhs_sz == rhs_sz) {
        continue;
      }
      return (lhs_sz < rhs_sz) != order_entry.is_desc;
    } else if (UNLIKELY(agg_info.agg_kind == hdk::ir::AggType::kApproxQuantile)) {
      CHECK_LT(materialized_approx_quantile_buffer_idx,
               approx_quantile_materialized_buffers_.size());
      const auto& approx_quantile_materialized_buffer =
          approx_quantile_materialized_buffers_[materialized_approx_quantile_buffer_idx];
      const auto lhs_value = approx_quantile_materialized_buffer[lhs];
      const auto rhs_value = approx_quantile_materialized_buffer[rhs];
      ++materialized_approx_quantile_buffer_idx;
      if (lhs_value == rhs_value) {
        continue;
      } else if (entry_type->nullable()) {
        if (lhs_value == NULL_DOUBLE) {
          return order_entry.nulls_first;
        } else if (rhs_value == NULL_DOUBLE) {
          return !order_entry.nulls_first;
        }
      }
      return (lhs_value < rhs_value) != order_entry.is_desc;
    }

    const auto lhs_v = buffer_itr_.getColumnInternal(lhs_storage->buff_,
                                                     fixedup_lhs,
                                                     order_entry.tle_no - 1,
                                                     lhs_storage_lookup_result);
    const auto rhs_v = buffer_itr_.getColumnInternal(rhs_storage->buff_,
                                                     fixedup_rhs,
                                                     order_entry.tle_no - 1,
                                                     rhs_storage_lookup_result);

    if (UNLIKELY(isNull(entry_type, lhs_v, float_argument_input) &&
                 isNull(entry_type, rhs_v, float_argument_input))) {
      continue;
    }
    if (UNLIKELY(isNull(entry_type, lhs_v, float_argument_input) &&
                 !isNull(entry_type, rhs_v, float_argument_input))) {
      return order_entry.nulls_first;
    }
    if (UNLIKELY(isNull(entry_type, rhs_v, float_argument_input) &&
                 !isNull(entry_type, lhs_v, float_argument_input))) {
      return !order_entry.nulls_first;
    }

    if (LIKELY(lhs_v.isInt())) {
      CHECK(rhs_v.isInt());
      if (UNLIKELY(entry_type->isExtDictionary())) {
        CHECK_EQ(4, entry_type->canonicalSize());
        CHECK(executor_);
        const auto string_dict_proxy = executor_->getStringDictionaryProxy(
            entry_type->as<hdk::ir::ExtDictionaryType>()->dictId(),
            result_set_->row_set_mem_owner_,
            false);
        auto lhs_str = string_dict_proxy->getString(lhs_v.i1);
        auto rhs_str = string_dict_proxy->getString(rhs_v.i1);
        if (lhs_str == rhs_str) {
          continue;
        }
        return (lhs_str < rhs_str) != order_entry.is_desc;
      }

      if (lhs_v.i1 == rhs_v.i1) {
        continue;
      }
      if (entry_type->isFloatingPoint()) {
        if (float_argument_input) {
          const auto lhs_dval = *reinterpret_cast<const float*>(may_alias_ptr(&lhs_v.i1));
          const auto rhs_dval = *reinterpret_cast<const float*>(may_alias_ptr(&rhs_v.i1));
          return (lhs_dval < rhs_dval) != order_entry.is_desc;
        } else {
          const auto lhs_dval =
              *reinterpret_cast<const double*>(may_alias_ptr(&lhs_v.i1));
          const auto rhs_dval =
              *reinterpret_cast<const double*>(may_alias_ptr(&rhs_v.i1));
          return (lhs_dval < rhs_dval) != order_entry.is_desc;
        }
      }
      return (lhs_v.i1 < rhs_v.i1) != order_entry.is_desc;
    } else {
      if (lhs_v.isPair()) {
        CHECK(rhs_v.isPair());
        const auto lhs =
            pair_to_double({lhs_v.i1, lhs_v.i2}, entry_type, float_argument_input);
        const auto rhs =
            pair_to_double({rhs_v.i1, rhs_v.i2}, entry_type, float_argument_input);
        if (lhs == rhs) {
          continue;
        }
        return (lhs < rhs) != order_entry.is_desc;
      } else {
        CHECK(lhs_v.isStr() && rhs_v.isStr());
        const auto lhs = lhs_v.strVal();
        const auto rhs = rhs_v.strVal();
        if (lhs == rhs) {
          continue;
        }
        return (lhs < rhs) != order_entry.is_desc;
      }
    }
  }
  return false;
}

// Partial sort permutation into top(least by compare) n elements.
// If permutation.size() <= n then sort entire permutation by compare.
// Return PermutationView with new size() = min(n, permutation.size()).
PermutationView ResultSet::topPermutation(PermutationView permutation,
                                          const size_t n,
                                          const Comparator& compare,
                                          const bool single_threaded) {
  auto timer = DEBUG_TIMER(__func__);
  if (n < permutation.size()) {
    std::partial_sort(
        permutation.begin(), permutation.begin() + n, permutation.end(), compare);
    permutation.resize(n);
#ifdef HAVE_TBB
  } else if (!single_threaded) {
    tbb::parallel_sort(permutation.begin(), permutation.end(), compare);
#endif
  } else {
    std::sort(permutation.begin(), permutation.end(), compare);
  }
  return permutation;
}

void ResultSet::radixSortOnGpu(
    const Config& config,
    const std::list<hdk::ir::OrderEntry>& order_entries) const {
  auto timer = DEBUG_TIMER(__func__);
  const int device_id{0};
  GpuAllocator cuda_allocator(buffer_provider_, device_id);
  CHECK_GT(block_size_, 0);
  CHECK_GT(grid_size_, 0);
  std::vector<int64_t*> group_by_buffers(block_size_);
  group_by_buffers[0] = reinterpret_cast<int64_t*>(storage_->getUnderlyingBuffer());
  auto dev_group_by_buffers =
      create_dev_group_by_buffers(&cuda_allocator,
                                  config,
                                  group_by_buffers,
                                  query_mem_desc_,
                                  block_size_,
                                  grid_size_,
                                  device_id,
                                  ExecutorDispatchMode::KernelPerFragment,
                                  /*num_input_rows=*/-1,
                                  /*prepend_index_buffer=*/true,
                                  /*always_init_group_by_on_host=*/true,
                                  /*use_bump_allocator=*/false,
                                  /*has_varlen_output=*/false,
                                  /*insitu_allocator*=*/nullptr);
  inplace_sort_gpu(
      order_entries, query_mem_desc_, dev_group_by_buffers, buffer_provider_, device_id);
  copy_group_by_buffers_from_gpu(
      buffer_provider_,
      group_by_buffers,
      query_mem_desc_.getBufferSizeBytes(ExecutorDeviceType::GPU),
      dev_group_by_buffers.data,
      query_mem_desc_,
      block_size_,
      grid_size_,
      device_id,
      /*use_bump_allocator=*/false,
      /*has_varlen_output=*/false);
}

void ResultSet::radixSortOnCpu(
    const std::list<hdk::ir::OrderEntry>& order_entries) const {
  auto timer = DEBUG_TIMER(__func__);
  CHECK(!query_mem_desc_.hasKeylessHash());
  std::vector<int64_t> tmp_buff(query_mem_desc_.getEntryCount());
  std::vector<int32_t> idx_buff(query_mem_desc_.getEntryCount());
  CHECK_EQ(size_t(1), order_entries.size());
  auto buffer_ptr = storage_->getUnderlyingBuffer();
  for (const auto& order_entry : order_entries) {
    const auto target_idx = order_entry.tle_no - 1;
    const auto sortkey_val_buff = reinterpret_cast<int64_t*>(
        buffer_ptr + query_mem_desc_.getColOffInBytes(target_idx));
    const auto slot_width = query_mem_desc_.getPaddedSlotWidthBytes(target_idx);
    sort_groups_cpu(sortkey_val_buff,
                    &idx_buff[0],
                    query_mem_desc_.getEntryCount(),
                    order_entry.is_desc,
                    slot_width);
    apply_permutation_cpu(reinterpret_cast<int64_t*>(buffer_ptr),
                          &idx_buff[0],
                          query_mem_desc_.getEntryCount(),
                          &tmp_buff[0],
                          sizeof(int64_t));
    for (size_t target_idx = 0; target_idx < query_mem_desc_.getSlotCount();
         ++target_idx) {
      if (static_cast<int>(target_idx) == order_entry.tle_no - 1) {
        continue;
      }
      const auto slot_width = query_mem_desc_.getPaddedSlotWidthBytes(target_idx);
      const auto satellite_val_buff = reinterpret_cast<int64_t*>(
          buffer_ptr + query_mem_desc_.getColOffInBytes(target_idx));
      apply_permutation_cpu(satellite_val_buff,
                            &idx_buff[0],
                            query_mem_desc_.getEntryCount(),
                            &tmp_buff[0],
                            slot_width);
    }
  }
}

size_t ResultSet::getLimit() const {
  return keep_first_;
}

const std::vector<std::string> ResultSet::getStringDictionaryPayloadCopy(
    const int dict_id) const {
  const auto sdp =
      row_set_mem_owner_->getOrAddStringDictProxy(dict_id, /*with_generation=*/true);
  CHECK(sdp);
  return sdp->getDictionary()->copyStrings();
}

const std::pair<std::vector<int32_t>, std::vector<std::string>>
ResultSet::getUniqueStringsForDictEncodedTargetCol(const size_t col_idx) const {
  const auto col_type = colType(col_idx);
  CHECK(col_type->isExtDictionary());
  std::unordered_set<int32_t> unique_string_ids_set;
  const size_t num_entries = entryCount();
  std::vector<bool> targets_to_skip(colCount(), true);
  targets_to_skip[col_idx] = false;
  const auto null_val = inline_fixed_encoding_null_value(col_type);

  for (size_t row_idx = 0; row_idx < num_entries; ++row_idx) {
    const auto result_row = getRowAtNoTranslations(row_idx, targets_to_skip);
    if (!result_row.empty()) {
      const auto scalar_col_val = boost::get<ScalarTargetValue>(result_row[col_idx]);
      const int32_t string_id = static_cast<int32_t>(boost::get<int64_t>(scalar_col_val));
      if (string_id != null_val) {
        unique_string_ids_set.emplace(string_id);
      }
    }
  }

  const size_t num_unique_strings = unique_string_ids_set.size();
  std::vector<int32_t> unique_string_ids(num_unique_strings);
  size_t string_idx{0};
  for (const auto unique_string_id : unique_string_ids_set) {
    unique_string_ids[string_idx++] = unique_string_id;
  }

  const int32_t dict_id = col_type->as<hdk::ir::ExtDictionaryType>()->dictId();
  const auto sdp = row_set_mem_owner_->getOrAddStringDictProxy(dict_id,
                                                               /*with_generation=*/true);
  CHECK(sdp);

  return std::make_pair(unique_string_ids, sdp->getStrings(unique_string_ids));
}

/**
 * Determines if it is possible to directly form a ColumnarResults class from this
 * result set, bypassing the default columnarization.
 *
 * NOTE: If there exists a permutation vector (i.e., in some ORDER BY queries), it
 * becomes equivalent to the row-wise columnarization.
 */
bool ResultSet::isDirectColumnarConversionPossible() const {
  if (query_mem_desc_.didOutputColumnar()) {
    return permutation_.empty() && (query_mem_desc_.getQueryDescriptionType() ==
                                        QueryDescriptionType::Projection ||
                                    (query_mem_desc_.getQueryDescriptionType() ==
                                         QueryDescriptionType::GroupByPerfectHash ||
                                     query_mem_desc_.getQueryDescriptionType() ==
                                         QueryDescriptionType::GroupByBaselineHash));
  } else {
    return permutation_.empty() && (query_mem_desc_.getQueryDescriptionType() ==
                                        QueryDescriptionType::GroupByPerfectHash ||
                                    query_mem_desc_.getQueryDescriptionType() ==
                                        QueryDescriptionType::GroupByBaselineHash);
  }
}

bool ResultSet::isZeroCopyColumnarConversionPossible(size_t column_idx) const {
  return query_mem_desc_.didOutputColumnar() &&
         query_mem_desc_.getQueryDescriptionType() == QueryDescriptionType::Projection &&
         appended_storage_.empty() && storage_ &&
         (lazy_fetch_info_.empty() || !lazy_fetch_info_[column_idx].is_lazily_fetched);
}

bool ResultSet::isChunkedZeroCopyColumnarConversionPossible(size_t column_idx) const {
  return query_mem_desc_.didOutputColumnar() &&
         query_mem_desc_.getQueryDescriptionType() == QueryDescriptionType::Projection &&
         storage_ &&
         (lazy_fetch_info_.empty() || !lazy_fetch_info_[column_idx].is_lazily_fetched);
}

const int8_t* ResultSet::getColumnarBuffer(size_t column_idx) const {
  CHECK(isZeroCopyColumnarConversionPossible(column_idx));
  return storage_->getUnderlyingBuffer() + query_mem_desc_.getColOffInBytes(column_idx);
}

std::vector<std::pair<const int8_t*, size_t>> ResultSet::getChunkedColumnarBuffer(
    size_t column_idx) const {
  CHECK(isChunkedZeroCopyColumnarConversionPossible(column_idx));

  std::vector<std::pair<const int8_t*, size_t>> retval;
  retval.reserve(1 + appended_storage_.size());

  retval.emplace_back(
      storage_->getUnderlyingBuffer() + storage_->getColOffInBytes(column_idx),
      storage_->binSearchRowCount());

  for (auto& chunk_uptr : appended_storage_) {
    const int8_t* ptr =
        chunk_uptr->getUnderlyingBuffer() + chunk_uptr->getColOffInBytes(column_idx);
    size_t row_count = chunk_uptr->binSearchRowCount();
    retval.emplace_back(ptr, row_count);
  }

  return retval;
}

// Returns a bitmap (and total number) of all single slot targets
std::tuple<std::vector<bool>, size_t> ResultSet::getSingleSlotTargetBitmap() const {
  std::vector<bool> target_bitmap(targets_.size(), true);
  size_t num_single_slot_targets = 0;
  for (size_t target_idx = 0; target_idx < targets_.size(); target_idx++) {
    auto sql_type = targets_[target_idx].type;
    if (targets_[target_idx].is_agg &&
        targets_[target_idx].agg_kind == hdk::ir::AggType::kAvg) {
      target_bitmap[target_idx] = false;
    } else if (sql_type->isString() || sql_type->isArray()) {
      target_bitmap[target_idx] = false;
    } else {
      num_single_slot_targets++;
    }
  }
  return std::make_tuple(std::move(target_bitmap), num_single_slot_targets);
}

/**
 * This function returns a bitmap and population count of it, where it denotes
 * all supported single-column targets suitable for direct columnarization.
 *
 * The final goal is to remove the need for such selection, but at the moment for any
 * target that doesn't qualify for direct columnarization, we use the traditional
 * result set's iteration to handle it (e.g., count distinct, approximate count distinct)
 */
std::tuple<std::vector<bool>, size_t> ResultSet::getSupportedSingleSlotTargetBitmap()
    const {
  CHECK(isDirectColumnarConversionPossible());
  auto [single_slot_targets, num_single_slot_targets] = getSingleSlotTargetBitmap();

  for (size_t target_idx = 0; target_idx < single_slot_targets.size(); target_idx++) {
    const auto& target = targets_[target_idx];
    if (single_slot_targets[target_idx] &&
        (is_distinct_target(target) ||
         target.agg_kind == hdk::ir::AggType::kApproxQuantile ||
         (target.is_agg && target.agg_kind == hdk::ir::AggType::kSample &&
          target.type->isFp32()))) {
      single_slot_targets[target_idx] = false;
      num_single_slot_targets--;
    }
  }
  CHECK_GE(num_single_slot_targets, size_t(0));
  return std::make_tuple(std::move(single_slot_targets), num_single_slot_targets);
}

// Returns the starting slot index for all targets in the result set
std::vector<size_t> ResultSet::getSlotIndicesForTargetIndices() const {
  std::vector<size_t> slot_indices(targets_.size(), 0);
  size_t slot_index = 0;
  for (size_t target_idx = 0; target_idx < targets_.size(); target_idx++) {
    slot_indices[target_idx] = slot_index;
    slot_index = advance_slot(slot_index, targets_[target_idx], false);
  }
  return slot_indices;
}

// namespace result_set

bool result_set::can_use_parallel_algorithms(const ResultSet& rows) {
  return !rows.isTruncated();
}

bool result_set::use_parallel_algorithms(const ResultSet& rows) {
  return result_set::can_use_parallel_algorithms(rows) && rows.entryCount() >= 20000;
}
