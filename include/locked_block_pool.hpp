#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>
#include "block_layout.hpp"

class LockedBlockPool {
public:
  explicit LockedBlockPool(const BlockLayout& layout);
  ~LockedBlockPool();

  LockedBlockPool(const LockedBlockPool&) = delete;
  LockedBlockPool& operator=(const LockedBlockPool&) = delete;

  void Shutdown();

  // Blocks until a free block exists OR stop/shutdown triggers.
  bool Acquire(int& out_index);

  // Returns block to pool.
  void Release(int index);

  int FreeCount() const;

  BlockView View(int index);
  uint64_t PeekGroupId(int index);

  const BlockLayout& Layout() const { return layout_; }
  void WakeAll();

private:
  BlockLayout layout_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<void*> blocks_;
  std::vector<int> free_;

  bool locked_ = false;
  bool shutdown_ = false;
};