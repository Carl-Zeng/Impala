// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_RUNTIME_ROW_BATCH_H
#define IMPALA_RUNTIME_ROW_BATCH_H

#include <vector>
#include <cstring>
#include <boost/scoped_ptr.hpp>

#include "codegen/impala-ir.h"
#include "common/compiler-util.h"
#include "common/logging.h"
#include "runtime/buffered-block-mgr.h" // for BufferedBlockMgr::Block
#include "runtime/descriptors.h"
#include "runtime/disk-io-mgr.h"
#include "runtime/mem-pool.h"
#include "runtime/mem-tracker.h"

namespace impala {

class BufferedTupleStream;
template <typename K, typename V> class FixedSizeHashTable;
class MemTracker;
class RowBatchSerializeTest;
class RuntimeState;
class TRowBatch;
class Tuple;
class TupleRow;
class TupleDescriptor;


/// A RowBatch encapsulates a batch of rows, each composed of a number of tuples.
/// The maximum number of rows is fixed at the time of construction.
/// The row batch reference a few different sources of memory.
///   1. TupleRow ptrs - may be malloc'd and owned by the RowBatch or allocated from
///      the tuple pool, depending on whether legacy joins and aggs are enabled.
///      See the comment on tuple_ptrs_ for more details.
///   2. Tuple memory - this is allocated (or transferred to) the row batches tuple pool.
///   3. Auxiliary tuple memory (e.g. string data) - this can either be stored externally
///      (don't copy strings) or from the tuple pool (strings are copied).  If external,
///      the data is in an io buffer that may not be attached to this row batch.  The
///      creator of that row batch has to make sure that the io buffer is not recycled
///      until all batches that reference the memory have been consumed.
/// In order to minimize memory allocations, RowBatches and TRowBatches that have been
/// serialized and sent over the wire should be reused (this prevents compression_scratch_
/// from being needlessly reallocated).
//
/// Row batches and memory usage: We attempt to stream row batches through the plan
/// tree without copying the data. This means that row batches are often not-compact
/// and reference memory outside of the row batch. This results in most row batches
/// having a very small memory footprint and in some row batches having a very large
/// one (it contains all the memory that other row batches are referencing). An example
/// is IoBuffers which are only attached to one row batch. Only when the row batch reaches
/// a blocking operator or the root of the fragment is the row batch memory freed.
/// This means that in some cases (e.g. very selective queries), we still need to
/// pass the row batch through the exec nodes (even if they have no rows) to trigger
/// memory deletion. AtCapacity() encapsulates the check that we are not accumulating
/// excessive memory.
//
/// A row batch is considered at capacity if all the rows are full or it has accumulated
/// auxiliary memory up to a soft cap. (See at_capacity_mem_usage_ comment).
class RowBatch {
 public:
  /// Create RowBatch for a maximum of 'capacity' rows of tuples specified
  /// by 'row_desc'.
  /// tracker cannot be NULL.
  RowBatch(const RowDescriptor& row_desc, int capacity, MemTracker* tracker);

  /// Populate a row batch from input_batch by copying input_batch's
  /// tuple_data into the row batch's mempool and converting all offsets
  /// in the data back into pointers.
  /// TODO: figure out how to transfer the data from input_batch to this RowBatch
  /// (so that we don't need to make yet another copy)
  RowBatch(const RowDescriptor& row_desc, const TRowBatch& input_batch,
      MemTracker* tracker);

  /// Releases all resources accumulated at this row batch.  This includes
  ///  - tuple_ptrs
  ///  - tuple mem pool data
  ///  - buffer handles from the io mgr
  ~RowBatch();

  /// AddRows() is called before adding rows to the batch. Returns the index of the next
  /// row to be added. The caller is responsible for ensuring there is enough remaining
  /// capacity: it is invalid to call AddRows when num_rows_ + n > capacity_. The rows
  /// are uninitialized and each tuple of the row must be set, after which CommitRows()
  /// can be called to update num_rows_. Two consecutive AddRow() calls without a
  /// CommitRows() call between them have the same effect as a single call.
  int ALWAYS_INLINE AddRows(int n) {
    DCHECK_LE(num_rows_ + n, capacity_);
    return num_rows_;
  }

  int ALWAYS_INLINE AddRow() { return AddRows(1); }

  void ALWAYS_INLINE CommitRows(int n) {
    DCHECK_GE(n, 0);
    DCHECK_LE(num_rows_ + n, capacity_);
    num_rows_ += n;
  }

  void ALWAYS_INLINE CommitLastRow() { CommitRows(1); }

  /// Set function can be used to reduce the number of rows in the batch. This is only
  /// used in the limit case where more rows were added than necessary.
  void set_num_rows(int num_rows) {
    DCHECK_LE(num_rows, num_rows_);
    DCHECK_GE(num_rows, 0);
    num_rows_ = num_rows;
  }

  /// Returns true if the row batch has filled rows up to its capacity or has accumulated
  /// enough memory. The memory calculation includes the tuple data pool and any
  /// auxiliary memory attached to the row batch.
  bool ALWAYS_INLINE AtCapacity() {
    DCHECK_LE(num_rows_, capacity_);
    // Check AtCapacity() conditions enforced in MarkNeedToReturn() and AddTupleStream()
    if (need_to_return_) DCHECK_EQ(num_rows_, capacity_);
    if (num_tuple_streams() > 0) DCHECK_EQ(num_rows_, capacity_);
    DCHECK((num_tuple_streams() == 0 && !need_to_return_) || num_rows_ == capacity_);
    int64_t mem_usage = auxiliary_mem_usage_ + tuple_data_pool_.total_allocated_bytes();
    return num_rows_ == capacity_ || mem_usage >= AT_CAPACITY_MEM_USAGE;
  }

  TupleRow* ALWAYS_INLINE GetRow(int row_idx) {
    DCHECK(tuple_ptrs_ != NULL);
    DCHECK_GE(row_idx, 0);
    DCHECK_LT(row_idx, capacity_);
    return reinterpret_cast<TupleRow*>(tuple_ptrs_ + row_idx * num_tuples_per_row_);
  }

  /// An iterator for going through a row batch, starting at 'row_idx'.
  /// This is more efficient than using GetRow() as it avoids loading the
  /// row batch state and doing multiplication on each loop with GetRow().
  class Iterator {
   public:
    IR_ALWAYS_INLINE Iterator(RowBatch* parent, int row_idx) :
        num_tuples_per_row_(parent->num_tuples_per_row_),
        row_(parent->tuple_ptrs_ + row_idx * num_tuples_per_row_),
        row_batch_end_(parent->tuple_ptrs_ + parent->num_rows_ * num_tuples_per_row_),
        parent_(parent) {
      DCHECK_GE(row_idx, 0);
      /// We allow empty row batches with num_rows_ == capacity_ == 0.
      /// That's why we cannot call GetRow() above to initialize 'row_'.
      DCHECK_LE(row_idx, parent->capacity_);
    }

    /// Return the current row pointed to by the row pointer.
    TupleRow* IR_ALWAYS_INLINE Get() { return reinterpret_cast<TupleRow*>(row_); }

    /// Increment the row pointer and return the next row.
    TupleRow* IR_ALWAYS_INLINE Next() {
      row_ += num_tuples_per_row_;
      DCHECK_LE((row_ - parent_->tuple_ptrs_) / num_tuples_per_row_, parent_->capacity_);
      return Get();
    }

    /// Returns true if the iterator is beyond the last row for read iterators.
    /// Useful for read iterators to determine the limit. Write iterators should use
    /// RowBatch::AtCapacity() instead.
    bool IR_ALWAYS_INLINE AtEnd() { return row_ >= row_batch_end_; }

    /// Returns the row batch which this iterator is iterating through.
    RowBatch* parent() { return parent_; }

   private:
    /// Number of tuples per row.
    const int num_tuples_per_row_;

    /// Pointer to the current row.
    Tuple** row_;

    /// Pointer to the row after the last row for read iterators.
    Tuple** const row_batch_end_;

    /// The row batch being iterated on.
    RowBatch* const parent_;
  };

  int num_tuples_per_row() { return num_tuples_per_row_; }
  int row_byte_size() { return num_tuples_per_row_ * sizeof(Tuple*); }
  MemPool* tuple_data_pool() { return &tuple_data_pool_; }
  int num_io_buffers() const { return io_buffers_.size(); }
  int num_tuple_streams() const { return tuple_streams_.size(); }

  /// Resets the row batch, returning all resources it has accumulated.
  void Reset();

  /// Add io buffer to this row batch.
  void AddIoBuffer(DiskIoMgr::BufferDescriptor* buffer);

  /// Add tuple stream to this row batch. The row batch takes ownership of the stream
  /// and will call Close() on the stream and delete it when freeing resources.
  void AddTupleStream(BufferedTupleStream* stream);

  /// Adds a block to this row batch. The block must be pinned. The blocks must be
  /// deleted when freeing resources.
  void AddBlock(BufferedBlockMgr::Block* block);

  /// Called to indicate that resources backing this row batch will be cleaned up after
  /// the next GetNext() call and the batch must be returned up the operator tree.
  /// This is used to control memory management for streaming rows.
  void MarkNeedToReturn() {
    need_to_return_ = true;
    MarkAtCapacity();
  }

  bool need_to_return() { return need_to_return_; }

  /// Transfer ownership of resources to dest.  This includes tuple data in mem
  /// pool and io buffers.
  void TransferResourceOwnership(RowBatch* dest);

  void CopyRow(TupleRow* src, TupleRow* dest) {
    memcpy(dest, src, num_tuples_per_row_ * sizeof(Tuple*));
  }

  /// Copy 'num_rows' rows from 'src' to 'dest' within the batch. Useful for exec
  /// nodes that skip an offset and copied more than necessary.
  void CopyRows(int dest, int src, int num_rows) {
    DCHECK_LE(dest, src);
    DCHECK_LE(src + num_rows, capacity_);
    memmove(tuple_ptrs_ + num_tuples_per_row_ * dest,
        tuple_ptrs_ + num_tuples_per_row_ * src,
        num_rows * num_tuples_per_row_ * sizeof(Tuple*));
  }

  void ClearRow(TupleRow* row) {
    memset(row, 0, num_tuples_per_row_ * sizeof(Tuple*));
  }

  /// Acquires state from the 'src' row batch into this row batch. This includes all IO
  /// buffers and tuple data.
  /// This row batch must be empty and have the same row descriptor as the src batch.
  /// This is used for scan nodes which produce RowBatches asynchronously.  Typically,
  /// an ExecNode is handed a row batch to populate (pull model) but ScanNodes have
  /// multiple threads which push row batches.
  void AcquireState(RowBatch* src);

  /// Deep copy all rows this row batch into dst, using memory allocated from
  /// dst's tuple_data_pool_. Only valid when dst is empty.
  /// TODO: the current implementation of deep copy can produce an oversized
  /// row batch if there are duplicate tuples in this row batch.
  void DeepCopyTo(RowBatch* dst);

  /// Create a serialized version of this row batch in output_batch, attaching all of the
  /// data it references to output_batch.tuple_data. This function attempts to
  /// detect duplicate tuples in the row batch to reduce the serialized size.
  /// output_batch.tuple_data will be snappy-compressed unless the compressed data is
  /// larger than the uncompressed data. Use output_batch.is_compressed to determine
  /// whether tuple_data is compressed. If an in-flight row is present in this row batch,
  /// it is ignored. This function does not Reset().
  Status Serialize(TRowBatch* output_batch);

  /// Utility function: returns total size of batch.
  static int GetBatchSize(const TRowBatch& batch);

  int ALWAYS_INLINE num_rows() const { return num_rows_; }
  int ALWAYS_INLINE capacity() const { return capacity_; }

  const RowDescriptor& row_desc() const { return row_desc_; }

  /// Max memory that this row batch can accumulate before it is considered at capacity.
  /// This is a soft capacity: row batches may exceed the capacity, preferably only by a
  /// row's worth of data.
  static const int AT_CAPACITY_MEM_USAGE = 8 * 1024 * 1024;

  // Max memory out of AT_CAPACITY_MEM_USAGE that should be used for fixed-length data,
  // in order to leave room for variable-length data.
  static const int FIXED_LEN_BUFFER_LIMIT = AT_CAPACITY_MEM_USAGE / 2;

  /// Allocates a buffer large enough for the fixed-length portion of 'capacity_' rows in
  /// this batch from 'tuple_data_pool_'. 'capacity_' is reduced if the allocation would
  /// exceed FIXED_LEN_BUFFER_LIMIT. Always returns enough space for at least one row.
  /// Returns Status::MEM_LIMIT_EXCEEDED and sets 'buffer' to NULL if a memory limit would
  /// have been exceeded. 'state' is used to log the error.
  /// On success, sets 'buffer_size' to the size in bytes and 'buffer' to the buffer.
  Status ResizeAndAllocateTupleBuffer(RuntimeState* state, int64_t* buffer_size,
       uint8_t** buffer);

 private:
  friend class RowBatchSerializeBaseline;
  friend class RowBatchSerializeBenchmark;
  friend class RowBatchSerializeTest;

  /// Decide whether to do full tuple deduplication based on row composition. Full
  /// deduplication is enabled only when there is risk of the serialized size being
  /// much larger than in-memory size due to non-adjacent duplicate tuples.
  bool UseFullDedup();

  /// Overload for testing that allows the test to force the deduplication level.
  Status Serialize(TRowBatch* output_batch, bool full_dedup);

  typedef FixedSizeHashTable<Tuple*, int> DedupMap;

  /// The total size of all data represented in this row batch (tuples and referenced
  /// string and collection data). This is the size of the row batch after removing all
  /// gaps in the auxiliary and deduplicated tuples (i.e. the smallest footprint for the
  /// row batch). If the distinct_tuples argument is non-null, full deduplication is
  /// enabled. The distinct_tuples map must be empty.
  int64_t TotalByteSize(DedupMap* distinct_tuples);

  void SerializeInternal(int64_t size, DedupMap* distinct_tuples,
      TRowBatch* output_batch);

  /// Close owned tuple streams and delete if needed.
  void CloseTupleStreams();

  /// Mark that no more rows should be added to the batch.
  void MarkAtCapacity() {
    DCHECK_LE(num_rows_, capacity_);
    capacity_ = num_rows_;
  }

  /// All members below need to be handled in RowBatch::AcquireState()

  // Class members that are accessed on performance-critical paths should appear
  // up the top to fit in as few cache lines as possible.

  int num_rows_;  // # of committed rows
  int capacity_;  // the value of num_rows_ at which batch is considered full.

  /// If true, this batch references unowned memory that will be cleaned up soon.
  /// See MarkNeedToReturn().
  bool need_to_return_;

  const int num_tuples_per_row_;

  /// Array of pointers with capacity_ * num_tuples_per_row_ elements.
  /// The memory ownership depends on whether legacy joins and aggs are enabled.
  ///
  /// Memory is malloc'd and owned by RowBatch:
  /// If enable_partitioned_hash_join=true and enable_partitioned_aggregation=true
  /// then the memory is owned by this RowBatch and is freed upon its destruction.
  /// This mode is more performant especially with SubplanNodes in the ExecNode tree
  /// because the tuple pointers are not transferred and do not have to be re-created
  /// in every Reset().
  ///
  /// Memory is allocated from MemPool:
  /// Otherwise, the memory is allocated from tuple_data_pool_. As a result, the
  /// pointer memory is transferred just like tuple data, and must be re-created
  /// in Reset(). This mode is required for the legacy join and agg which rely on
  /// the tuple pointers being allocated from the tuple_data_pool_, so they can
  /// acquire ownership of the tuple pointers.
  int tuple_ptrs_size_;
  Tuple** tuple_ptrs_;

  /// Sum of all auxiliary bytes. This includes IoBuffers and memory from
  /// TransferResourceOwnership().
  int64_t auxiliary_mem_usage_;

  /// holding (some of the) data referenced by rows
  MemPool tuple_data_pool_;

  // Less frequently used members that are not accessed on performance-critical paths
  // should go below here.

  /// Full row descriptor for rows in this batch.
  RowDescriptor row_desc_;

  MemTracker* mem_tracker_;  // not owned

  /// IO buffers current owned by this row batch. Ownership of IO buffers transfer
  /// between row batches. Any IO buffer will be owned by at most one row batch
  /// (i.e. they are not ref counted) so most row batches don't own any.
  std::vector<DiskIoMgr::BufferDescriptor*> io_buffers_;

  /// Tuple streams currently owned by this row batch.
  std::vector<BufferedTupleStream*> tuple_streams_;

  /// Blocks attached to this row batch. The underlying memory and block manager client
  /// are owned by the BufferedBlockMgr.
  std::vector<BufferedBlockMgr::Block*> blocks_;

  /// String to write compressed tuple data to in Serialize().
  /// This is a string so we can swap() with the string in the TRowBatch we're serializing
  /// to (we don't compress directly into the TRowBatch in case the compressed data is
  /// longer than the uncompressed data). Swapping avoids copying data to the TRowBatch and
  /// avoids excess memory allocations: since we reuse RowBatchs and TRowBatchs, and
  /// assuming all row batches are roughly the same size, all strings will eventually be
  /// allocated to the right size.
  std::string compression_scratch_;
};

}

/// Macro for iterating through '_row_batch', starting at '_start_row_idx'.
/// '_row_batch' is the row batch to iterate through.
/// '_start_row_idx' is the starting row index.
/// '_row' is the current row which the iterator is pointing to.
#define FOREACH_ROW(_row_batch, _start_row_idx, _row)                              \
  RowBatch::Iterator _##_row_batch##_iter(_row_batch, _start_row_idx);             \
  for (TupleRow* _row = _##_row_batch##_iter.Get(); !_##_row_batch##_iter.AtEnd(); \
      _row = _##_row_batch##_iter.Next())

#endif
