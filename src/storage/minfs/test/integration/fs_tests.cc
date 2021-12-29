// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Tests for MinFS-specific behavior.

#include <fcntl.h>
#include <fidl/fuchsia.hardware.block.volume/cpp/wire.h>
#include <fidl/fuchsia.io.admin/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fuchsia/minfs/c/fidl.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/vfs.h>
#include <lib/zx/vmo.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/device/vfs.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <algorithm>
#include <string>
#include <vector>

#include <fbl/algorithm.h>
#include <fbl/array.h>
#include <fbl/unique_fd.h>

#include "src/storage/fs_test/fs_test_fixture.h"
#include "src/storage/fvm/format.h"
#include "src/storage/minfs/format.h"
#include "src/storage/minfs/transaction_limits.h"

namespace fs_test {
namespace {

namespace fio = fuchsia_io;

// Tests using MinfsTest will get tested with and without FVM.
using MinfsTest = FilesystemTest;

void QueryInfo(const TestFilesystem& fs, fuchsia_io_admin::wire::FilesystemInfo* info) {
  // Sync before querying fs so that we can obtain an accurate number of used bytes. Otherwise,
  // blocks which are reserved but not yet allocated won't be counted.
  fbl::unique_fd root_fd = fs.GetRootFd();
  fsync(root_fd.get());

  fdio_cpp::FdioCaller caller(std::move(root_fd));
  auto result =
      fidl::WireCall<fuchsia_io_admin::DirectoryAdmin>(caller.channel())->QueryFilesystem();
  ASSERT_EQ(result.status(), ZX_OK);
  ASSERT_EQ(result.Unwrap()->s, ZX_OK);
  ASSERT_NE(result.Unwrap()->info, nullptr);
  *info = *(result.Unwrap()->info);

  // For now, info->name.data is a fixed size array.
  const char* kFsName = "minfs";
  ASSERT_EQ(memcmp(info->name.data(), kFsName, strlen(kFsName) + 1), 0)
      << "Unexpected filesystem mounted";

  ASSERT_EQ(info->block_size, minfs::kMinfsBlockSize);
  ASSERT_EQ(info->max_filename_size, minfs::kMinfsMaxNameSize);
  ASSERT_EQ(info->fs_type, VFS_TYPE_MINFS);
  ASSERT_NE(info->fs_id, 0ul);
  ASSERT_EQ(info->used_bytes % info->block_size, 0ul);
  ASSERT_EQ(info->total_bytes % info->block_size, 0ul);
  ASSERT_EQ(info->free_shared_pool_bytes % info->block_size, 0ul);
}

void GetFreeBlocks(const TestFilesystem& fs, uint32_t* out_free_blocks) {
  fuchsia_io_admin::wire::FilesystemInfo info;
  ASSERT_NO_FATAL_FAILURE(QueryInfo(fs, &info));
  uint64_t total_bytes = info.total_bytes + info.free_shared_pool_bytes;
  uint64_t used_bytes = info.used_bytes;
  *out_free_blocks = static_cast<uint32_t>((total_bytes - used_bytes) / info.block_size);
}

// Write to the file until at most |max_remaining_blocks| remain in the partition.
// Return the new remaining block count as |actual_remaining_blocks|.
void FillPartition(const TestFilesystem& fs, int fd, uint32_t max_remaining_blocks,
                   uint32_t* actual_remaining_blocks) {
  std::vector data(1'048'576, 0xaa);
  uint32_t free_blocks;

  while (true) {
    ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs, &free_blocks));
    if (free_blocks <= max_remaining_blocks) {
      break;
    }

    uint32_t blocks = free_blocks - max_remaining_blocks;
    // Assume that writing 1 block might require writing 2 additional indirect blocks, so if there
    // are more than 2 blocks to go, subtract 2, and if there are only 2 blocks to go, only do 1
    // block.
    if (blocks > 2) {
      blocks -= 2;
    } else if (blocks == 2) {
      --blocks;
    }
    size_t bytes = std::min<size_t>(data.size(), blocks * minfs::kMinfsBlockSize);
    ASSERT_EQ(write(fd, data.data(), bytes), static_cast<ssize_t>(bytes));
  }

  ASSERT_LE(free_blocks, max_remaining_blocks);

  *actual_remaining_blocks = free_blocks;
}

// Tests using MinfsFvmTest will only run with FVM.
class MinfsFvmTest : public BaseFilesystemTest {
 public:
  MinfsFvmTest(const TestFilesystemOptions& options = OptionsWithDescription("MinfsWithFvm"))
      : BaseFilesystemTest(options) {}

 protected:
  // Get the FVM path from the RamDisk. fs().GetDevicePath() returns the partition path like
  // "/dev/class/block/001" which isn't what we want.
  fbl::unique_fd GetFvmFd() {
    // This expects to be set up with a RamDisk. The filesystem has some variants and this could be
    // on a RamNand, but then this code would need updating.
    storage::RamDisk* ram_disk = fs().GetRamDisk();
    EXPECT_TRUE(ram_disk);

    // Want something like "/dev/sys/platform/00:00:2d/ramctl/ramdisk-0/block/fvm"
    std::string fvm_path = ram_disk->path() + "/fvm";
    fbl::unique_fd fd(open(fvm_path.c_str(), O_RDWR));
    EXPECT_TRUE(fd);
    return fd;
  }

  // Returns the GUID associated with the minfs partition inside FVM.
  zx::status<fuchsia_hardware_block_partition::wire::Guid> GetMinfsPartitionGuid() {
    zx::status<std::string> device_path_or = fs().DevicePath();
    EXPECT_TRUE(device_path_or.is_ok());
    fbl::unique_fd fd(open(device_path_or->c_str(), O_RDWR));
    EXPECT_TRUE(fd);

    fdio_cpp::UnownedFdioCaller caller(fd);
    auto response =
        fidl::WireCall(
            fidl::UnownedClientEnd<fuchsia_hardware_block_partition::Partition>(caller.channel()))
            ->GetInstanceGuid();
    if (response.status() != ZX_OK)
      return zx::error(response.status());
    if (response->status != ZX_OK)
      return zx::error(response->status);
    return zx::ok(*response->guid);
  }

  zx::status<fuchsia_hardware_block_volume::wire::VolumeManagerInfo> GetVolumeManagerInfo() {
    fbl::unique_fd fvm_fd = GetFvmFd();
    EXPECT_TRUE(fvm_fd);

    fdio_cpp::UnownedFdioCaller caller(fvm_fd.get());
    auto response =
        fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_volume::VolumeManager>(
                           caller.borrow_channel()))
            ->GetInfo();
    if (response.status() != ZX_OK)
      return zx::error(response.status());
    if (response->status != ZX_OK)
      return zx::error(response->status);
    return zx::ok(*response->info);
  }

  zx_status_t SetPartitionLimit(uint64_t slice_limit) {
    fbl::unique_fd fvm_fd = GetFvmFd();
    EXPECT_TRUE(fvm_fd);

    auto guid_or = GetMinfsPartitionGuid();
    EXPECT_TRUE(guid_or.is_ok());

    fdio_cpp::UnownedFdioCaller caller(fvm_fd.get());
    auto set_response =
        fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_volume::VolumeManager>(
                           caller.borrow_channel()))
            ->SetPartitionLimit(*guid_or, slice_limit);
    if (set_response.status() != ZX_OK)
      return set_response.status();
    if (set_response->status != ZX_OK)
      return set_response->status;

    // Query the partition limit to make sure it worked.
    auto get_response =
        fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_volume::VolumeManager>(
                           caller.borrow_channel()))
            ->GetPartitionLimit(*guid_or);
    if (get_response.status() != ZX_OK)
      return get_response.status();
    if (get_response->status != ZX_OK)
      return get_response->status;

    EXPECT_EQ(slice_limit, get_response->slice_count);
    return ZX_OK;
  }

  void ToggleMetrics(bool enabled) const {
    fbl::unique_fd fd = fs().GetRootFd();
    ASSERT_TRUE(fd);
    fdio_cpp::FdioCaller caller(std::move(fd));
    zx_status_t status;
    ASSERT_EQ(fuchsia_minfs_MinfsToggleMetrics(caller.borrow_channel(), enabled, &status), ZX_OK);
    ASSERT_EQ(status, ZX_OK);
  }

  zx::status<fuchsia_minfs_Metrics> GetMetrics() const {
    fbl::unique_fd fd = fs().GetRootFd();
    if (!fd)
      return zx::error(ZX_ERR_IO);
    fdio_cpp::FdioCaller caller(std::move(fd));
    fuchsia_minfs_Metrics metrics;
    zx_status_t status;
    zx_status_t fidl_status =
        fuchsia_minfs_MinfsGetMetrics(caller.borrow_channel(), &status, &metrics);
    if (fidl_status != ZX_OK)
      return zx::error(fidl_status);
    if (status != ZX_OK)
      return zx::error(status);
    return zx::ok(metrics);
  }
};

class MinfsFvmTestWith8MiBSliceSize : public MinfsFvmTest {
 public:
  static constexpr uint64_t kSliceSize = 1024 * 1024 * 8;

  static TestFilesystemOptions GetOptions() {
    auto options = OptionsWithDescription("MinfsWithFvm");
    options.fvm_slice_size = kSliceSize;
    return options;
  }

  MinfsFvmTestWith8MiBSliceSize() : MinfsFvmTest(GetOptions()) {}
};

// Tests using MinfsWithoutFvmTest will only run without FVM.
class MinfsWithoutFvmTest : public BaseFilesystemTest {
 public:
  MinfsWithoutFvmTest() : BaseFilesystemTest(OptionsWithDescription("MinfsWithoutFvm")) {}

 protected:
  void GetAllocations(zx::vmo* out_vmo, uint64_t* out_count) const {
    fbl::unique_fd fd = fs().GetRootFd();
    ASSERT_TRUE(fd);
    zx_status_t status;
    fdio_cpp::FdioCaller caller(std::move(fd));
    zx_handle_t vmo_handle;
    ASSERT_EQ(fuchsia_minfs_MinfsGetAllocatedRegions(caller.borrow_channel(), &status, &vmo_handle,
                                                     out_count),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    out_vmo->reset(vmo_handle);
  }

  void GetAllocatedBlocks(uint64_t* out_allocated_blocks) const {
    fuchsia_io_admin::wire::FilesystemInfo info;
    ASSERT_NO_FATAL_FAILURE(QueryInfo(fs(), &info));
    *out_allocated_blocks = static_cast<uint64_t>(info.used_bytes) / info.block_size;
  }
};

// Verify initial conditions on a filesystem, and validate that filesystem modifications adjust the
// query info accordingly.
TEST_F(MinfsFvmTest, QueryInitialState) {
  fuchsia_io_admin::wire::FilesystemInfo info;
  ASSERT_NO_FATAL_FAILURE(QueryInfo(fs(), &info));

  EXPECT_EQ(fs().options().fvm_slice_size, info.total_bytes);
  // TODO(fxbug.dev/31276): Adjust this once minfs accounting on truncate is fixed.
  EXPECT_EQ(2 * minfs::kMinfsBlockSize, info.used_bytes);
  // The inodes will use the required slices (rounded up). Since the current default inode data size
  // divides evenly into the slice size, the values should match exactly.
  EXPECT_EQ(fs().options().fvm_slice_size, 32768u);  // Verify expectations of this test.
  EXPECT_EQ(minfs::kMinfsDefaultInodeCount, info.total_nodes);
  // The "zero-th" inode is reserved, as well as the root directory.
  const uint64_t kInitialUsedNodes = 2;
  EXPECT_EQ(kInitialUsedNodes, info.used_nodes);

  // Allocate kExtraNodeCount new files, each using truncated (sparse) files.
  const uint64_t kExtraNodeCount = 16;
  for (uint64_t i = 0; i < kExtraNodeCount; i++) {
    const std::string path = GetPath("file_" + std::to_string(i));

    fbl::unique_fd fd(open(path.c_str(), O_CREAT | O_RDWR));
    ASSERT_GT(fd.get(), 0);
    ASSERT_EQ(ftruncate(fd.get(), 30 * 1024), 0);
  }

  // Adjust our query expectations: We should see the new nodes.
  ASSERT_NO_FATAL_FAILURE(QueryInfo(fs(), &info));
  EXPECT_EQ(kInitialUsedNodes + kExtraNodeCount, info.used_nodes);
}

// Validate that Minfs metrics are functioning correctly.
TEST_F(MinfsFvmTest, Metrics) {
  ASSERT_EQ(GetMetrics().status_value(), ZX_ERR_UNAVAILABLE);
  ASSERT_NO_FATAL_FAILURE(ToggleMetrics(true));

  zx::status<fuchsia_minfs_Metrics> metrics_or = GetMetrics();
  ASSERT_EQ(metrics_or.status_value(), ZX_OK);
  auto metrics = std::move(metrics_or).value();

  ASSERT_EQ(metrics.fs_metrics.create.success.total_calls, 0ul);
  ASSERT_EQ(metrics.fs_metrics.create.failure.total_calls, 0ul);

  const std::string path = GetPath("test-file");
  fbl::unique_fd fd(open(path.c_str(), O_CREAT | O_RDWR));
  ASSERT_TRUE(fd);
  metrics_or = GetMetrics();
  ASSERT_EQ(metrics_or.status_value(), ZX_OK);
  metrics = std::move(metrics_or).value();
  ASSERT_EQ(metrics.fs_metrics.create.success.total_calls, 1ul);
  ASSERT_EQ(metrics.fs_metrics.create.failure.total_calls, 0ul);
  ASSERT_NE(metrics.fs_metrics.create.success.total_time_spent, 0ul);
  ASSERT_EQ(metrics.fs_metrics.create.failure.total_time_spent, 0ul);

  fd.reset(open(path.c_str(), O_CREAT | O_RDWR | O_EXCL));
  ASSERT_FALSE(fd);
  metrics_or = GetMetrics();
  ASSERT_EQ(metrics_or.status_value(), ZX_OK);
  metrics = std::move(metrics_or).value();
  ASSERT_EQ(metrics.fs_metrics.create.success.total_calls, 1ul);
  ASSERT_EQ(metrics.fs_metrics.create.failure.total_calls, 1ul);
  ASSERT_NE(metrics.fs_metrics.create.success.total_time_spent, 0ul);
  ASSERT_NE(metrics.fs_metrics.create.failure.total_time_spent, 0ul);

  metrics_or = GetMetrics();
  ASSERT_EQ(metrics_or.status_value(), ZX_OK);
  metrics = std::move(metrics_or).value();
  ASSERT_EQ(metrics.fs_metrics.unlink.success.total_calls, 0ul);
  ASSERT_EQ(metrics.fs_metrics.unlink.failure.total_calls, 0ul);
  ASSERT_EQ(metrics.fs_metrics.unlink.success.total_time_spent, 0ul);
  ASSERT_EQ(metrics.fs_metrics.unlink.failure.total_time_spent, 0ul);

  ASSERT_EQ(unlink(path.c_str()), 0);
  metrics_or = GetMetrics();
  ASSERT_EQ(metrics_or.status_value(), ZX_OK);
  metrics = std::move(metrics_or).value();
  ASSERT_EQ(metrics.fs_metrics.unlink.success.total_calls, 1ul);
  ASSERT_EQ(metrics.fs_metrics.unlink.failure.total_calls, 0ul);
  ASSERT_NE(metrics.fs_metrics.unlink.success.total_time_spent, 0ul);
  ASSERT_EQ(metrics.fs_metrics.unlink.failure.total_time_spent, 0ul);

  ASSERT_NE(unlink(path.c_str()), 0);
  metrics_or = GetMetrics();
  ASSERT_EQ(metrics_or.status_value(), ZX_OK);
  metrics = std::move(metrics_or).value();
  ASSERT_EQ(metrics.fs_metrics.unlink.success.total_calls, 1ul);
  ASSERT_EQ(metrics.fs_metrics.unlink.failure.total_calls, 1ul);
  ASSERT_NE(metrics.fs_metrics.unlink.success.total_time_spent, 0ul);
  ASSERT_NE(metrics.fs_metrics.unlink.failure.total_time_spent, 0ul);

  ASSERT_NO_FATAL_FAILURE(ToggleMetrics(false));
  metrics_or = GetMetrics();
  ASSERT_EQ(metrics_or.status_value(), ZX_ERR_UNAVAILABLE);
}

// Return number of blocks allocated by the file at |fd|.
void GetFileBlocks(int fd, uint64_t* blocks) {
  struct stat stats;
  ASSERT_EQ(fstat(fd, &stats), 0);
  off_t size = stats.st_blocks * VNATTR_BLKSIZE;
  ASSERT_EQ(size % minfs::kMinfsBlockSize, 0);
  *blocks = static_cast<uint64_t>(size / minfs::kMinfsBlockSize);
}

// Fill a directory to at most |max_blocks| full of direntries.
// We assume the directory is empty to begin with, and any files we are adding do not already exist.
void FillDirectory(const TestFilesystem& fs, int dir_fd, uint32_t max_blocks) {
  uint32_t file_count = 0;
  int entries_per_iteration = 150;
  while (true) {
    std::string path;
    for (int i = 0; i < entries_per_iteration; ++i) {
      path = "file_" + std::to_string(file_count++);
      fbl::unique_fd fd(openat(dir_fd, path.c_str(), O_CREAT | O_RDWR));
      ASSERT_TRUE(fd);
    }

    uint64_t current_blocks;
    ASSERT_NO_FATAL_FAILURE(GetFileBlocks(dir_fd, &current_blocks));
    if (current_blocks > max_blocks) {
      ASSERT_EQ(unlinkat(dir_fd, path.c_str(), 0), 0);
      break;
    } else if (current_blocks == max_blocks) {
      // Do just one entry per iteration for the last block.
      entries_per_iteration = 1;
    }
  }
}

TEST_F(MinfsFvmTestWith8MiBSliceSize, FreeSharedPoolBytes) {
  // Get the volume initial conditions for computing what minfs should be returning. There should be
  // at least two free slices for us to test the partition limit.
  zx::status<fuchsia_hardware_block_volume::wire::VolumeManagerInfo> manager_info =
      GetVolumeManagerInfo();
  ASSERT_TRUE(manager_info.is_ok());
  ASSERT_LT(manager_info->assigned_slice_count, manager_info->slice_count);
  uint64_t free_slices = manager_info->slice_count - manager_info->assigned_slice_count;

  // Normal free space size should just report the volume manager's free space.
  fuchsia_io_admin::wire::FilesystemInfo info;
  ASSERT_NO_FATAL_FAILURE(QueryInfo(fs(), &info));
  ASSERT_EQ(kSliceSize * free_slices, info.free_shared_pool_bytes);

  // Lower the partition limit to one more slice than the filesystem currently is using (since
  // there's only our partition in FVM, we know all used slices belong to minfs).
  uint64_t new_limit = manager_info->assigned_slice_count + 1;
  ASSERT_EQ(ZX_OK, SetPartitionLimit(new_limit));
  ASSERT_NO_FATAL_FAILURE(QueryInfo(fs(), &info));
  EXPECT_EQ(kSliceSize, info.free_shared_pool_bytes);  // Set exactly one slice free.

  // Match the limit to the current partition size.
  new_limit = manager_info->assigned_slice_count;
  ASSERT_EQ(ZX_OK, SetPartitionLimit(new_limit));
  ASSERT_NO_FATAL_FAILURE(QueryInfo(fs(), &info));
  EXPECT_EQ(0u, info.free_shared_pool_bytes);  // No slices free.

  // Lower the limit to to below the partition size.
  new_limit = manager_info->assigned_slice_count - 1;
  ASSERT_EQ(ZX_OK, SetPartitionLimit(new_limit));
  ASSERT_NO_FATAL_FAILURE(QueryInfo(fs(), &info));
  EXPECT_EQ(0u, info.free_shared_pool_bytes);  // No slices free.

  // Remove the limit, it should go back to the full free bytes.
  ASSERT_EQ(ZX_OK, SetPartitionLimit(0u));
  ASSERT_NO_FATAL_FAILURE(QueryInfo(fs(), &info));
  ASSERT_EQ(kSliceSize * free_slices, info.free_shared_pool_bytes);
}

// Test various operations when the Minfs partition is near capacity.  This test is sensitive to the
// FVM slice size and was designed with a 8 MiB slice size in mind.
TEST_F(MinfsFvmTestWith8MiBSliceSize, FullOperations) {
  // Define file and directory names we will use upfront.
  const char* big_path = "big_file";
  const char* med_path = "med_file";
  const char* sml_path = "sml_file";
  const char* dir_path = "directory";

  // Open the mount point and create three files.
  fbl::unique_fd mnt_fd = fs().GetRootFd();
  ASSERT_TRUE(mnt_fd);

  fbl::unique_fd big_fd(openat(mnt_fd.get(), big_path, O_CREAT | O_RDWR));
  ASSERT_TRUE(big_fd);

  fbl::unique_fd med_fd(openat(mnt_fd.get(), med_path, O_CREAT | O_RDWR));
  ASSERT_TRUE(med_fd);

  fbl::unique_fd sml_fd(openat(mnt_fd.get(), sml_path, O_CREAT | O_RDWR));
  ASSERT_TRUE(sml_fd);

  // Write to the big file, filling the partition and leaving 2 blocks unused.
  uint32_t free_blocks = 2;
  uint32_t actual_blocks;
  ASSERT_NO_FATAL_FAILURE(FillPartition(fs(), big_fd.get(), free_blocks, &actual_blocks));

  // Delete the big file.
  big_fd.reset();
  ASSERT_EQ(unlinkat(mnt_fd.get(), big_path, 0), 0);

  // Try to write to more than the previously available blocks, which should succeed if the big
  // file's blocks were reclaimed properly.
  auto data = std::vector(static_cast<size_t>(minfs::kMinfsBlockSize) * (actual_blocks + 1), 0xaa);
  ASSERT_EQ(write(med_fd.get(), data.data(), data.size()), static_cast<ssize_t>(data.size()));

  // Remove all of the data in the medium file.
  ASSERT_EQ(ftruncate(med_fd.get(), 0), 0);
  ASSERT_EQ(lseek(med_fd.get(), 0, SEEK_SET), 0);

  // Recreate the big file
  big_fd.reset(openat(mnt_fd.get(), big_path, O_CREAT | O_RDWR));
  ASSERT_TRUE(big_fd);

  // Write to the big file, filling the partition and leaving at most kMinfsDirect + 1 blocks
  // unused.
  free_blocks = minfs::kMinfsDirect + 1;
  ASSERT_NO_FATAL_FAILURE(FillPartition(fs(), big_fd.get(), free_blocks, &actual_blocks));

  // Write enough data to the second file to take up all remaining blocks except for 1.
  // This should strictly be writing to the direct block section of the file.
  data = std::vector(static_cast<size_t>(minfs::kMinfsBlockSize), 0xaa);
  ASSERT_EQ(data.size(), static_cast<size_t>(minfs::kMinfsBlockSize));
  for (unsigned i = 0; i < actual_blocks - 1; i++) {
    ASSERT_EQ(write(med_fd.get(), data.data(), data.size()), static_cast<ssize_t>(data.size()));
  }

  // Make sure we now have only 1 block remaining.
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &free_blocks));
  ASSERT_EQ(free_blocks, 1u);

  // We should now have exactly 1 free block remaining. Attempt to write into the indirect
  // section of the file so we ensure that at least 2 blocks are required.
  // This is expected to fail.
  ASSERT_EQ(lseek(med_fd.get(), minfs::kMinfsBlockSize * minfs::kMinfsDirect, SEEK_SET),
            minfs::kMinfsBlockSize * minfs::kMinfsDirect);
  ASSERT_EQ(data.size(), static_cast<size_t>(minfs::kMinfsBlockSize));
  ASSERT_LT(write(med_fd.get(), data.data(), data.size()), 0);

  // We should still have 1 free block remaining. Writing to the beginning of the second file
  // should only require 1 (direct) block, and therefore pass.
  // Note: This fails without block reservation.
  ASSERT_EQ(data.size(), static_cast<size_t>(minfs::kMinfsBlockSize));
  ASSERT_EQ(write(sml_fd.get(), data.data(), data.size()), static_cast<ssize_t>(data.size()));

  // There are no longer any blocks free.
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &free_blocks));
  ASSERT_EQ(free_blocks, 0u);

  // Making directory should fail now that the file system is completely full.
  ASSERT_LT(mkdirat(mnt_fd.get(), dir_path, 0666), 0);

  // Remove the small file, which should free up blocks.
  sml_fd.reset();
  ASSERT_EQ(unlinkat(mnt_fd.get(), sml_path, 0), 0);
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &free_blocks));
  ASSERT_GT(free_blocks, 0u);

  // Without block reservation, something from the failed write remains allocated. Try editing
  // nearby blocks to force a writeback of partially allocated data.
  // Note: This will fail without block reservation since the previous failed write would leave
  //       the only free block incorrectly allocated and 1 additional block is required for
  //       copy-on-write truncation.
  struct stat s;
  ASSERT_EQ(fstat(big_fd.get(), &s), 0);
  auto truncate_size = static_cast<ssize_t>(
      fbl::round_up(static_cast<uint64_t>(s.st_size / 2), minfs::kMinfsBlockSize));
  ASSERT_EQ(ftruncate(big_fd.get(), truncate_size), 0);

  // Attempt to remount. Without block reservation, an additional block from the previously
  // failed write will still be incorrectly allocated, causing fsck to fail.
  EXPECT_EQ(fs().Unmount().status_value(), ZX_OK);
  EXPECT_EQ(fs().Fsck().status_value(), ZX_OK);
  EXPECT_EQ(fs().Mount().status_value(), ZX_OK);

  mnt_fd = fs().GetRootFd();
  ASSERT_TRUE(mnt_fd);

  // Re-open big file
  big_fd.reset(openat(mnt_fd.get(), big_path, O_RDWR));
  ASSERT_TRUE(big_fd);

  // Re-create the small file
  sml_fd.reset(openat(mnt_fd.get(), sml_path, O_CREAT | O_RDWR));
  ASSERT_TRUE(sml_fd);

  // Make sure we now have at least kMinfsDirect + 1 blocks remaining.
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &free_blocks));
  ASSERT_GE(free_blocks, minfs::kMinfsDirect + 1);

  // We have some room now, so create a new directory.
  ASSERT_EQ(mkdirat(mnt_fd.get(), dir_path, 0666), 0);
  fbl::unique_fd dir_fd(openat(mnt_fd.get(), dir_path, O_RDONLY));
  ASSERT_TRUE(dir_fd);

  // Fill the directory up to kMinfsDirect blocks full of direntries.
  ASSERT_NO_FATAL_FAILURE(FillDirectory(fs(), dir_fd.get(), minfs::kMinfsDirect));

  // Now re-fill the partition by writing as much as possible back to the original file.
  // Attempt to leave 1 block free.
  ASSERT_EQ(lseek(big_fd.get(), truncate_size, SEEK_SET), truncate_size);
  free_blocks = 1;
  ASSERT_NO_FATAL_FAILURE(FillPartition(fs(), big_fd.get(), free_blocks, &actual_blocks));

  if (actual_blocks == 0) {
    // It is possible that, in our previous allocation of big_fd, we ended up leaving less than
    // |free_blocks| free. Since the file has grown potentially large, it is possible that
    // allocating a single block will also allocate additional indirect blocks.
    // For example, in a case where we have 2 free blocks remaining and expect to allocate 1,
    // we may actually end up allocating 2 instead, leaving us with 0 free blocks.
    // Since sml_fd is using less than kMinfsDirect blocks and thus is guaranteed to have a 1:1
    // block usage ratio, we can remedy this situation by removing a single block from sml_fd.
    ASSERT_EQ(ftruncate(sml_fd.get(), 0), 0);
  }

  while (actual_blocks > free_blocks) {
    // Otherwise, if too many blocks remain (if e.g. we needed to allocate 3 blocks but only 2
    // are remaining), write to sml_fd until only 1 remains.
    ASSERT_EQ(data.size(), static_cast<size_t>(minfs::kMinfsBlockSize));
    ASSERT_EQ(write(sml_fd.get(), data.data(), data.size()), static_cast<ssize_t>(data.size()));
    actual_blocks--;
  }

  // Ensure that there is now exactly one block remaining.
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &actual_blocks));
  ASSERT_EQ(free_blocks, actual_blocks);

  // Now, attempt to add one more file to the directory we created. Since it will need to
  // allocate 2 blocks (1 indirect + 1 direct) and there is only 1 remaining, it should fail.
  uint64_t block_count;
  ASSERT_NO_FATAL_FAILURE(GetFileBlocks(dir_fd.get(), &block_count));
  ASSERT_EQ(block_count, minfs::kMinfsDirect);
  fbl::unique_fd tmp_fd(openat(dir_fd.get(), "new_file", O_CREAT | O_RDWR));
  ASSERT_FALSE(tmp_fd);

  // Again, try editing nearby blocks to force bad allocation leftovers to be persisted, and
  // remount the partition. This is expected to fail without block reservation.
  ASSERT_EQ(fstat(big_fd.get(), &s), 0);
  ASSERT_EQ(s.st_size % minfs::kMinfsBlockSize, 0);
  truncate_size = s.st_size - minfs::kMinfsBlockSize;
  ASSERT_EQ(ftruncate(big_fd.get(), truncate_size), 0);
  EXPECT_EQ(fs().Unmount().status_value(), ZX_OK);
  EXPECT_EQ(fs().Fsck().status_value(), ZX_OK);
  EXPECT_EQ(fs().Mount().status_value(), ZX_OK);

  // Re-open files.
  mnt_fd = fs().GetRootFd();
  ASSERT_TRUE(mnt_fd);
  big_fd.reset(openat(mnt_fd.get(), big_path, O_RDWR));
  ASSERT_TRUE(big_fd);
  sml_fd.reset(openat(mnt_fd.get(), sml_path, O_RDWR));
  ASSERT_TRUE(sml_fd);

  // Fill the partition again, writing one block of data to sml_fd
  // in case we need an emergency truncate.
  ASSERT_EQ(data.size(), static_cast<size_t>(minfs::kMinfsBlockSize));
  ASSERT_EQ(write(sml_fd.get(), data.data(), data.size()), static_cast<ssize_t>(data.size()));
  ASSERT_EQ(lseek(big_fd.get(), truncate_size, SEEK_SET), truncate_size);
  free_blocks = 1;
  ASSERT_NO_FATAL_FAILURE(FillPartition(fs(), big_fd.get(), free_blocks, &actual_blocks));

  if (actual_blocks == 0) {
    // If we ended up with fewer blocks than expected, truncate sml_fd to create more space.
    // (See note above for details.)
    ASSERT_EQ(ftruncate(sml_fd.get(), 0), 0);
  }

  ASSERT_EQ(data.size(), static_cast<size_t>(minfs::kMinfsBlockSize));
  while (actual_blocks > free_blocks) {
    // Otherwise, if too many blocks remain (if e.g. we needed to allocate 3 blocks but only 2
    // are remaining), write to sml_fd until only 1 remains.
    ASSERT_EQ(write(sml_fd.get(), data.data(), data.size()), static_cast<ssize_t>(data.size()));
    actual_blocks--;
  }

  // Ensure that there is now exactly one block remaining.
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &actual_blocks));
  ASSERT_EQ(free_blocks, actual_blocks);

  // Now, attempt to rename one of our original files under the new directory.
  // This should also fail.
  ASSERT_NE(renameat(mnt_fd.get(), med_path, dir_fd.get(), med_path), 0);

  // Again, truncate the original file and attempt to remount.
  // Again, this should fail without block reservation.
  ASSERT_EQ(fstat(big_fd.get(), &s), 0);
  ASSERT_EQ(s.st_size % minfs::kMinfsBlockSize, 0);
  truncate_size = s.st_size - minfs::kMinfsBlockSize;
  ASSERT_EQ(ftruncate(big_fd.get(), truncate_size), 0);
  EXPECT_EQ(fs().Unmount().status_value(), ZX_OK);
  EXPECT_EQ(fs().Mount().status_value(), ZX_OK);

  mnt_fd = fs().GetRootFd();
  ASSERT_EQ(unlinkat(mnt_fd.get(), big_path, 0), 0);
  ASSERT_EQ(unlinkat(mnt_fd.get(), med_path, 0), 0);
  ASSERT_EQ(unlinkat(mnt_fd.get(), sml_path, 0), 0);
}

TEST_P(MinfsTest, UnlinkFail) {
  uint32_t original_blocks;
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &original_blocks));

  uint32_t fd_count = 100;
  fbl::unique_fd fds[fd_count];

  char data[minfs::kMinfsBlockSize];
  memset(data, 0xaa, sizeof(data));
  const std::string filename = GetPath("file");

  // Open, write to, and unlink |fd_count| total files without closing them.
  for (unsigned i = 0; i < fd_count; i++) {
    // Since we are unlinking, we can use the same filename for all files.
    fds[i].reset(open(filename.c_str(), O_CREAT | O_RDWR | O_EXCL));
    ASSERT_TRUE(fds[i]);
    ASSERT_EQ(write(fds[i].get(), data, sizeof(data)), static_cast<ssize_t>(sizeof(data)));
    ASSERT_EQ(unlink(filename.c_str()), 0);
  }

  // Close the first, middle, and last files to test behavior when various "links" are removed.
  uint32_t first_fd = 0;
  uint32_t mid_fd = fd_count / 2;
  uint32_t last_fd = fd_count - 1;
  ASSERT_EQ(close(fds[first_fd].release()), 0);
  ASSERT_EQ(close(fds[mid_fd].release()), 0);
  ASSERT_EQ(close(fds[last_fd].release()), 0);

  // Sync Minfs to ensure all unlink operations complete.
  fbl::unique_fd fd(open(filename.c_str(), O_CREAT));
  ASSERT_TRUE(fd);
  ASSERT_EQ(syncfs(fd.get()), 0);

  // Check that the number of Minfs free blocks has decreased.
  uint32_t current_blocks;
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &current_blocks));
  ASSERT_LT(current_blocks, original_blocks);

  // Put the ramdisk to sleep and close all the fds. This will cause file purge to fail,
  // and all unlinked files will be left intact (on disk).
  ASSERT_EQ(fs().GetRamDisk()->SleepAfter(0).status_value(), ZX_OK);

  // The ram-disk is asleep but since no transactions have been processed, the writeback state has
  // not been updated. The first file we close will appear to succeed.
  ASSERT_EQ(close(fds[first_fd + 1].release()), 0);

  // Sync to ensure the writeback state is updated. Since the purge from the previous close will
  // fail, sync will also fail.
  ASSERT_LT(syncfs(fd.get()), 0);

  // Close all open fds.
  for (unsigned i = first_fd + 2; i < last_fd; i++) {
    if (i != mid_fd) {
      ASSERT_EQ(close(fds[i].release()), -1);
    }
  }

  // Sync Minfs to ensure all close operations complete. Since Minfs is in a read-only state and
  // some requests have not been successfully persisted to disk, the sync is expected to fail.
  ASSERT_LT(syncfs(fd.get()), 0);

  // Remount Minfs, which should cause leftover unlinked files to be removed.
  ASSERT_EQ(fs().GetRamDisk()->Wake().status_value(), ZX_OK);
  EXPECT_EQ(fs().Unmount().status_value(), ZX_OK);
  EXPECT_EQ(fs().Mount().status_value(), ZX_OK);

  // Check that the block count has been reverted to the value before any files were added.
  ASSERT_NO_FATAL_FAILURE(GetFreeBlocks(fs(), &current_blocks));
  ASSERT_EQ(current_blocks, original_blocks);
}

// Verifies that the information returned by GetAllocatedRegions FIDL call is correct by
// checking it against the block devices metrics.
TEST_F(MinfsWithoutFvmTest, GetAllocatedRegions) {
  constexpr char kFirstPath[] = "some_file";
  constexpr char kSecondPath[] = "another_file";
  fbl::unique_fd mnt_fd = fs().GetRootFd();
  ASSERT_TRUE(mnt_fd);

  fbl::unique_fd first_fd(openat(mnt_fd.get(), kFirstPath, O_CREAT | O_RDWR));
  ASSERT_TRUE(first_fd);
  fbl::unique_fd second_fd(openat(mnt_fd.get(), kSecondPath, O_CREAT | O_RDWR));
  ASSERT_TRUE(second_fd);

  char data[minfs::kMinfsBlockSize];
  memset(data, 0xb0b, sizeof(data));
  // Interleave writes
  ASSERT_EQ(write(first_fd.get(), data, sizeof(data)), static_cast<ssize_t>(sizeof(data)));
  ASSERT_EQ(fsync(first_fd.get()), 0);
  ASSERT_EQ(write(second_fd.get(), data, sizeof(data)), static_cast<ssize_t>(sizeof(data)));
  ASSERT_EQ(fsync(second_fd.get()), 0);
  ASSERT_EQ(write(first_fd.get(), data, sizeof(data)), static_cast<ssize_t>(sizeof(data)));
  ASSERT_EQ(fsync(first_fd.get()), 0);

  // Ensure that the number of bytes reported via GetAllocatedRegions and QueryInfo is the same
  zx::vmo vmo;
  uint64_t count;
  uint64_t actual_blocks;
  uint64_t total_blocks = 0;
  ASSERT_NO_FATAL_FAILURE(GetAllocations(&vmo, &count));
  ASSERT_NO_FATAL_FAILURE(GetAllocatedBlocks(&actual_blocks));
  fbl::Array<fuchsia_minfs_BlockRegion> buffer(new fuchsia_minfs_BlockRegion[count], count);
  ASSERT_EQ(vmo.read(buffer.data(), 0, sizeof(fuchsia_minfs_BlockRegion) * count), ZX_OK);
  for (size_t i = 0; i < count; i++) {
    total_blocks += buffer[i].length;
  }
  ASSERT_EQ(total_blocks, actual_blocks);

  // Delete second_fd. This allows us test that the FIDL call will still match the metrics
  // from QueryInfo after deletes and with fragmentation.
  ASSERT_EQ(unlinkat(mnt_fd.get(), kSecondPath, 0), 0);
  ASSERT_EQ(close(second_fd.release()), 0);
  ASSERT_EQ(fsync(mnt_fd.get()), 0);
  total_blocks = 0;

  ASSERT_NO_FATAL_FAILURE(GetAllocations(&vmo, &count));
  ASSERT_NO_FATAL_FAILURE(GetAllocatedBlocks(&actual_blocks));
  buffer.reset(new fuchsia_minfs_BlockRegion[count], count);
  ASSERT_EQ(vmo.read(buffer.data(), 0, sizeof(fuchsia_minfs_BlockRegion) * count), ZX_OK);
  for (size_t i = 0; i < count; i++) {
    total_blocks += buffer[i].length;
  }
  ASSERT_EQ(total_blocks, actual_blocks);
}

INSTANTIATE_TEST_SUITE_P(/*no prefix*/, MinfsTest, testing::ValuesIn(AllTestFilesystems()),
                         testing::PrintToStringParamName());

}  // namespace
}  // namespace fs_test
