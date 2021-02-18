// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/status.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zircon/device/vfs.h>
#include <zircon/time.h>

#include <algorithm>
#include <limits>
#include <memory>

#include <fbl/algorithm.h>
#include <fbl/auto_call.h>
#include <fbl/string_piece.h>
#include <fs/debug.h>
#include <fs/vfs_types.h>
#include <safemath/checked_math.h>

#include "zircon/assert.h"

#ifdef __Fuchsia__
#include <lib/fidl-utils/bind.h>
#include <zircon/syscalls.h>

#include <utility>

#include <fbl/auto_lock.h>
#endif

#include "src/storage/minfs/file.h"
#include "src/storage/minfs/minfs_private.h"
#include "src/storage/minfs/unowned_vmo_buffer.h"
#include "src/storage/minfs/vnode.h"

namespace minfs {

File::File(Minfs* fs) : VnodeMinfs(fs) {}

#ifdef __Fuchsia__

// AllocateAndCommitData does the following operations:
//  - Allocates data blocks,
//  - Frees old data blocks (if this were overwritten),
//  - Issues data and metadata writes,
//  - Updates inode to reflect new size and modification time.
//      Writes or fragments of a write may change inode's size, block_count or
//      file block table (dnum, inum, dinum).
void File::AllocateAndCommitData(std::unique_ptr<Transaction> transaction) {
  // Calculate the maximum number of data blocks we can update within one transaction. This is
  // the smallest between half the capacity of the writeback buffer, and the number of direct
  // blocks needed to touch the maximum allowed number of indirect blocks.
  const uint32_t max_direct_blocks =
      kMinfsDirect + (kMinfsDirectPerIndirect * Vfs()->Limits().GetMaximumMetaDataBlocks());
  const uint32_t max_writeback_blocks = static_cast<blk_t>(Vfs()->WritebackCapacity() / 2);
  const uint32_t max_blocks = std::min(max_direct_blocks, max_writeback_blocks);

  fbl::Array<blk_t> allocated_blocks(new blk_t[max_blocks], max_blocks);

  // Iterate through all relative block ranges and acquire absolute blocks for each of them.
  while (true) {
    blk_t expected_blocks = allocation_state_.GetTotalPending();
    ZX_ASSERT(expected_blocks <= max_blocks);

    if (expected_blocks == 0) {
      if (GetInode()->size != allocation_state_.GetNodeSize()) {
        GetMutableInode()->size = allocation_state_.GetNodeSize();
        ValidateVmoTail(GetInode()->size);
      }

      // Since we may have pending reservations from an expected update, reset the allocation
      // state. This may happen if the same block range is allocated and de-allocated (e.g.
      // written and truncated) before the state is resolved.
      ZX_ASSERT(allocation_state_.GetNodeSize() == GetInode()->size);
      allocation_state_.Reset(allocation_state_.GetNodeSize());
      ZX_DEBUG_ASSERT(allocation_state_.IsEmpty());
      break;
    }

    blk_t bno_start, bno_count;
    ZX_ASSERT(allocation_state_.GetNextRange(&bno_start, &bno_count) == ZX_OK);
    ZX_ASSERT(bno_count <= max_blocks);

    // Since we reserved enough space ahead of time, this should not fail.
    ZX_ASSERT(BlocksSwap(transaction.get(), bno_start, bno_count, &allocated_blocks[0]) == ZX_OK);

    // Enqueue each data block one at a time, as they may not be contiguous on disk.
    UnownedVmoBuffer buffer(vmo());
    for (blk_t i = 0; i < bno_count; i++) {
      storage::Operation operation = {
          .type = storage::OperationType::kWrite,
          .vmo_offset = bno_start + i,
          .dev_offset = allocated_blocks[i] + Vfs()->Info().dat_block,
          .length = 1,
      };
      transaction->EnqueueData(operation, &buffer);
    }

    // Since we are updating the file in "chunks", only update the on-disk inode size
    // with the portion we've written so far.
    blk_t last_byte = (bno_start + bno_count) * Vfs()->BlockSize();
    ZX_ASSERT(last_byte <= fbl::round_up(allocation_state_.GetNodeSize(), Vfs()->BlockSize()));

    if (last_byte > GetInode()->size && last_byte < allocation_state_.GetNodeSize()) {
      // If we have written past the end of the recorded size but have not yet reached the
      // allocated size, update the recorded size to the last byte written.
      GetMutableInode()->size = last_byte;
    } else if (allocation_state_.GetNodeSize() <= last_byte) {
      // If we have just written to the allocated inode size, update the recorded size
      // accordingly.
      GetMutableInode()->size = allocation_state_.GetNodeSize();
    }

    ValidateVmoTail(GetInode()->size);

    // In the future we could resolve on a per state (i.e. reservation) basis, but since swaps
    // are currently only made within a single thread, for now it is okay to resolve
    // everything.
    transaction->PinVnode(fbl::RefPtr(this));
  }

  // At this point there should not be any pending allocations. Following code block
  // prints and asserts it.
  if (allocation_state_.GetTotalPending() != 0) {
    FX_LOGS(ERROR) << "Found modified blocks(" << allocation_state_.GetTotalPending()
                   << ") after marking them clean";
    for (auto modified_blocks = allocation_state_.cbegin();
         modified_blocks != allocation_state_.cend(); ++modified_blocks) {
      FX_LOGS(ERROR) << "   bitoff:" << modified_blocks->bitoff
                     << " bitlen:" << modified_blocks->bitlen;
    }
    ZX_ASSERT(allocation_state_.GetTotalPending() == 0);
  }

  InodeSync(transaction.get(), DirtyCacheEnabled() ? kMxFsSyncDefault : kMxFsSyncMtime);
  Vfs()->CommitTransaction(std::move(transaction));
}

zx_status_t File::BlocksSwap(Transaction* transaction, blk_t start, blk_t count, blk_t* bnos) {
  if (count == 0)
    return ZX_OK;

  VnodeMapper mapper(this);
  VnodeIterator iterator;
  zx_status_t status = iterator.Init(&mapper, transaction, start);
  if (status != ZX_OK)
    return status;

  while (count > 0) {
    const blk_t file_block = static_cast<blk_t>(iterator.file_block());
    ZX_DEBUG_ASSERT(allocation_state_.IsPending(file_block));
    blk_t old_block = iterator.Blk();
    // TODO(fxbug.dev/51587): A value of zero for the block pointer has special meaning: the block
    // is sparse or unmapped. We should add something for this magic constant and fix all places
    // that currently hard code zero.
    if (old_block == 0) {
      GetMutableInode()->block_count++;
    }
    // For copy-on-write, swap the block out if it's a data block.
    blk_t new_block = old_block;
    Vfs()->BlockSwap(transaction, old_block, &new_block);
    zx_status_t status = iterator.SetBlk(new_block);
    if (status != ZX_OK)
      return status;
    *bnos++ = new_block;
    bool cleared = allocation_state_.ClearPending(file_block, old_block != 0);
    ZX_DEBUG_ASSERT(cleared);
    // We have cleared pending bit for the block. Update the accounding for the
    // dirty block.
    Vfs()->SubtractDirtyBytes(Vfs()->BlockSize(), old_block != 0);
    --count;
    status = iterator.Advance();
    if (status != ZX_OK)
      return status;
  }
  return iterator.Flush();
}

#endif

void File::UpdateModificationTime() {
  zx_time_t cur_time = GetTimeUTC();
  GetMutableInode()->modify_time = cur_time;
}

blk_t File::GetBlockCount() const {
#ifdef __Fuchsia__
  return GetInode()->block_count + allocation_state_.GetNewPending();
#else
  return GetInode()->block_count;
#endif
}

uint64_t File::GetSize() const {
#ifdef __Fuchsia__
  return allocation_state_.GetNodeSize();
#endif
  return GetInode()->size;
}

void File::SetSize(uint32_t new_size) {
#ifdef __Fuchsia__
  allocation_state_.SetNodeSize(new_size);
#else
  GetMutableInode()->size = new_size;
#endif
}

void File::AcquireWritableBlock(Transaction* transaction, blk_t local_bno, blk_t old_bno,
                                blk_t* out_bno) {
  bool using_new_block = (old_bno == 0);
#ifdef __Fuchsia__
  allocation_state_.SetPending(local_bno, !using_new_block);
  [[maybe_unused]] auto unused_ = Vfs()->AddDirtyBytes(Vfs()->BlockSize(), !using_new_block);
#else
  if (using_new_block) {
    Vfs()->BlockNew(transaction, out_bno);
    GetMutableInode()->block_count++;
  } else {
    *out_bno = old_bno;
  }
#endif
}

void File::DeleteBlock(PendingWork* transaction, blk_t local_bno, blk_t old_bno, bool indirect) {
  // If we found a block that was previously allocated, delete it.
  if (old_bno != 0) {
    transaction->DeallocateBlock(old_bno);
    GetMutableInode()->block_count--;
  }
#ifdef __Fuchsia__
  if (!indirect) {
    if (allocation_state_.IsPending(local_bno)) {
      Vfs()->SubtractDirtyBytes(Vfs()->BlockSize(), old_bno != 0);
    }
    // Remove this block from the pending allocation map in case it's set so we do not
    // proceed to allocate a new block.
    allocation_state_.ClearPending(local_bno, old_bno != 0);
  }
#endif
}

#ifdef __Fuchsia__
void File::IssueWriteback(Transaction* transaction, blk_t vmo_offset, blk_t dev_offset,
                          blk_t block_count) {
  // This is a no-op. The blocks are swapped later.
}

bool File::HasPendingAllocation(blk_t vmo_offset) {
  return allocation_state_.IsPending(vmo_offset);
}

void File::CancelPendingWriteback() {
  // Drop all pending writes, revert the size of the inode to the "pre-pending-write" size.
  allocation_state_.Reset(GetInode()->size);
}

#endif

zx_status_t File::CanUnlink() const { return ZX_OK; }

fs::VnodeProtocolSet File::GetProtocols() const { return fs::VnodeProtocol::kFile; }

zx_status_t File::Read(void* data, size_t len, size_t off, size_t* out_actual) {
  TRACE_DURATION("minfs", "File::Read", "ino", GetIno(), "len", len, "off", off);
  FX_LOGS(DEBUG) << "minfs_read() vn=" << this << "(#" << GetIno() << ") len=" << len
                 << " off=" << off;

  fs::Ticker ticker(Vfs()->StartTicker());
  auto get_metrics = fbl::MakeAutoCall(
      [&ticker, &out_actual, this]() { Vfs()->UpdateReadMetrics(*out_actual, ticker.End()); });

  Transaction transaction(Vfs());
  return ReadInternal(&transaction, data, len, off, out_actual);
}

zx::status<uint32_t> File::GetRequiredBlockCount(size_t offset, size_t length) {
  zx::status<blk_t> uncached = zx::error(ZX_ERR_INVALID_ARGS);
  uncached = ::minfs::GetRequiredBlockCount(offset, length, Vfs()->BlockSize());
  if (!DirtyCacheEnabled()) {
    return uncached;
  }

  if (uncached.is_error()) {
    return uncached;
  }

  return GetRequiredBlockCountForDirtyCache(offset, length, uncached.value());
}

zx::status<> File::CheckAndFlush(bool is_truncate, size_t length, size_t offset) {
  auto status = ShouldFlush(is_truncate, length, offset);
  if (status.is_error()) {
    return zx::error(status.error_value());
  }
  if (!status.value()) {
    return zx::ok();
  }
  return FlushCachedWrites();
}

zx::status<std::unique_ptr<Transaction>> File::GetTransaction(uint32_t reserve_blocks) {
  std::unique_ptr<Transaction> transaction;
  std::unique_ptr<CachedBlockTransaction> cached_transaction;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cached_transaction = std::move(cached_transaction_);
    if (!DirtyCacheEnabled()) {
      ZX_ASSERT(cached_transaction == nullptr);
    }
  }
  zx_status_t status;
  if (!DirtyCacheEnabled() || cached_transaction == nullptr) {
    if ((status = Vfs()->BeginTransaction(0, reserve_blocks, &transaction)) != ZX_OK) {
      return zx::error(status);
    }
  } else {
    if ((status = Vfs()->ContinueTransaction(reserve_blocks, std::move(cached_transaction),
                                             &transaction)) != ZX_OK) {
      // Failure here means that we ran out of space. Force flush pending writes
      // and return the failure.
      if (transaction != nullptr) {
        [[maybe_unused]] auto error =
            FlushTransaction(std::move(transaction), /*force_flush=*/true).status_value();
      }
      return zx::error(status);
    }
  }

  return zx::ok(std::move(transaction));
}

zx_status_t File::Write(const void* data, size_t len, size_t offset, size_t* out_actual) {
  TRACE_DURATION("minfs", "File::Write", "ino", GetIno(), "len", len, "off", offset);
  FX_LOGS(DEBUG) << "minfs_write() vn=" << this << "(#" << GetIno() << ") len=" << len
                 << " off=" << offset;

  *out_actual = 0;
  fs::Ticker ticker(Vfs()->StartTicker());
  auto get_metrics = fbl::MakeAutoCall(
      [&ticker, &out_actual, this]() { Vfs()->UpdateWriteMetrics(*out_actual, ticker.End()); });

  auto new_size_or = safemath::CheckAdd(offset, len);
  if (!new_size_or.IsValid() || new_size_or.ValueOrDie() > kMinfsMaxFileSize) {
    return ZX_ERR_FILE_BIG;
  }

  // If this file's pending blocks have crossed a limit or if there are no free blocks in the
  // filesystem, try to flush before we proceed.
  if (auto status = CheckAndFlush(false, len, offset); status.is_error()) {
    return status.error_value();
  }

  // Calculate maximum number of blocks to reserve for this write operation.
  auto reserve_blocks_or = GetRequiredBlockCount(offset, len);
  if (reserve_blocks_or.is_error()) {
    return reserve_blocks_or.error_value();
  }

  size_t reserve_blocks = reserve_blocks_or.value();
  zx_status_t status;
  auto transaction_or = GetTransaction(reserve_blocks);
  if (transaction_or.is_error()) {
    return transaction_or.error_value();
  }
  std::unique_ptr<Transaction> transaction = std::move(transaction_or.value());
  // We mark block that has writes pending only after we have enough blocks reserved through
  // BeginTransaction or through ContinueTransaction.
  if (DirtyCacheEnabled()) {
    if (auto status = MarkRequiredBlocksPending(offset, len); status.is_error()) {
      return status.error_value();
    }
  }

  status =
      WriteInternal(transaction.get(), static_cast<const uint8_t*>(data), len, offset, out_actual);
  if (status != ZX_OK) {
    return status;
  }

  if (*out_actual == 0) {
    return ZX_OK;
  }

  // If anything was written, enqueue operations allocated within WriteInternal.
  UpdateModificationTime();
  return FlushTransaction(std::move(transaction)).status_value();
}

zx_status_t File::Append(const void* data, size_t len, size_t* out_end, size_t* out_actual) {
  zx_status_t status = Write(data, len, GetSize(), out_actual);
  *out_end = GetSize();
  return status;
}

zx_status_t File::Truncate(size_t len) {
  TRACE_DURATION("minfs", "File::Truncate");
  if (len > kMinfsMaxFileSize) {
    return ZX_ERR_INVALID_ARGS;
  }

  // TODO(unknown): Following can be optimized.
  // - do not flush part of the file that will be truncated.
  // - conditionally flush unaffected part if necessary.
  if (auto status = FlushCachedWrites(); status.is_error()) {
    return status.error_value();
  }

  fs::Ticker ticker(Vfs()->StartTicker());
  auto get_metrics =
      fbl::MakeAutoCall([&ticker, this] { Vfs()->UpdateTruncateMetrics(ticker.End()); });

  std::unique_ptr<Transaction> transaction;
  // Due to file copy-on-write, up to 1 new (data) block may be required.
  size_t reserve_blocks = 1;
  zx_status_t status;

  if ((status = Vfs()->BeginTransaction(0, reserve_blocks, &transaction)) != ZX_OK) {
    return status;
  }

  if ((status = TruncateInternal(transaction.get(), len)) != ZX_OK) {
    return status;
  }

  // Force sync the inode to persistent storage: although our data blocks will be allocated
  // later, the act of truncating may have allocated indirect blocks.
  //
  // Ensure our inode is consistent with that metadata.
  UpdateModificationTime();
  auto result = FlushTransaction(std::move(transaction), true);
  ZX_ASSERT(result.is_ok());
  return ZX_OK;
}

}  // namespace minfs
