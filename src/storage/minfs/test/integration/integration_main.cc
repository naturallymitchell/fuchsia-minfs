// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/memfs/memfs.h>

#include <memory>

#include <fs/test_support/environment.h>
#include <zxtest/zxtest.h>

#include "minfs_fixtures.h"

// The test can operate over either a ramdisk, or a real device. Initialization
// of that device happens at the test environment level, but the test fixtures
// must be able to see it.
fs::Environment* fs::g_environment;

int main(int argc, char** argv) {
  const char kHelp[] = "minfs integration tests";
  fs::Environment::TestConfig config = {};
  if (!config.GetOptions(argc, argv)) {
    printf("%s\n%s\n", kHelp, config.HelpMessage());
  }
  if (config.show_help) {
    printf("%s\n%s\n", kHelp, config.HelpMessage());
  }

  config.is_packaged = false;  // This test has not been converted to packages yet.
  config.mount_path = kMountPath;
  config.ramdisk_block_count = 1 << 20;  // 1M blocks.
  config.format_type = DISK_FORMAT_MINFS;

  auto parent = std::make_unique<fs::Environment>(config);
  fs::g_environment = parent.get();

  // Initialize a tmpfs instance to "hold" the mounted blobfs.
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  if (loop.StartThread() != ZX_OK) {
    printf("Unable to initialize local tmpfs loop\n");
    return -1;
  }
  memfs_filesystem_t* fs;
  if (memfs_install_at(loop.dispatcher(), "/minfs-tmp", &fs) != ZX_OK) {
    printf("Unable to install local tmpfs\n");
    return -1;
  }

  zxtest::Runner::GetInstance()->AddGlobalTestEnvironment(std::move(parent));

  int result = RUN_ALL_TESTS(argc, argv);
  loop.Shutdown();
  memfs_uninstall_unsafe(fs, "/minfs-tmp");
  return result;
}
