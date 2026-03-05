#include "locked_block_pool.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include <sys/mman.h>
#include <sys/resource.h>

#include "globals.hpp"
#include "logging.hpp"

LockedBlockPool::LockedBlockPool(const BlockLayout& layout) : layout_(layout) {
  blocks_.reserve(kPoolBlocks);
  free_.reserve(kPoolBlocks);

  bool locking_enabled = true;
  std::vector<void*> locked_ptrs;
  locked_ptrs.reserve(kPoolBlocks);

  for (int i = 0; i < kPoolBlocks; ++i) {
    void* p = nullptr;
    int rc = ::posix_memalign(&p, layout_.page_size, layout_.block_bytes);
    if (rc != 0 || !p) throw std::runtime_error("posix_memalign failed");

    std::memset(p, 0, layout_.block_bytes);

    // Initialize header.
    auto* hdr = reinterpret_cast<BlockHeader*>(p);
    hdr->magic = 0xB10C0B10u;
    hdr->block_index = static_cast<uint32_t>(i);
    hdr->max_frames = static_cast<uint32_t>(kMaxFramesPerGroup);
    hdr->frame_bytes = layout_.frame_bytes;
    hdr->pixel_capacity_bytes = static_cast<uint32_t>(layout_.pixel_capacity_bytes);

    if (locking_enabled) {
      if (::mlock(p, layout_.block_bytes) == 0) {
        locked_ptrs.push_back(p);
      } else {
        int e = errno;
        struct rlimit lim {};
        std::string lims;
        if (::getrlimit(RLIMIT_MEMLOCK, &lim) == 0) {
          lims = " (RLIMIT_MEMLOCK cur=" + std::to_string((unsigned long long)lim.rlim_cur) +
                 ", max=" + std::to_string((unsigned long long)lim.rlim_max) + ")";
        }
        Log("[Pool] mlock failed for block ", i, ": ", std::strerror(e), lims,
            ". Continuing WITHOUT locked memory.");

        for (void* lp : locked_ptrs) ::munlock(lp, layout_.block_bytes);
        locked_ptrs.clear();
        locking_enabled = false;
      }
    }

    blocks_.push_back(p);
    free_.push_back(i);
  }

  locked_ = locking_enabled && (locked_ptrs.size() == static_cast<size_t>(kPoolBlocks));
  if (locked_) {
    Log("[Pool] Locked-memory enabled: mlock() succeeded for all blocks (", kPoolBlocks,
        " blocks, ", layout_.block_bytes, " bytes each).");
  } else {
    Log("[Pool] Locked-memory disabled.");
  }
}

LockedBlockPool::~LockedBlockPool() {
  Shutdown();
}

void LockedBlockPool::Shutdown() {
  std::lock_guard<std::mutex> lk(mu_);
  if (shutdown_) return;
  shutdown_ = true;

  if (locked_) {
    for (void* p : blocks_) ::munlock(p, layout_.block_bytes);
  }
  for (void* p : blocks_) std::free(p);

  blocks_.clear();
  free_.clear();
}

bool LockedBlockPool::Acquire(int& out_index) {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [&] {
    return shutdown_ || g_stop.load(std::memory_order_relaxed) || !free_.empty();
  });
  if (shutdown_ || g_stop.load(std::memory_order_relaxed)) return false;
  out_index = free_.back();
  free_.pop_back();
  return true;
}

void LockedBlockPool::Release(int index) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    free_.push_back(index);
  }
  cv_.notify_one();
}

int LockedBlockPool::FreeCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return static_cast<int>(free_.size());
}

BlockView LockedBlockPool::View(int index) {
  BlockView v;
  v.base = reinterpret_cast<uint8_t*>(blocks_[index]);
  v.header = reinterpret_cast<BlockHeader*>(v.base + layout_.off_header);
  v.group = reinterpret_cast<ImageGroup*>(v.base + layout_.off_group);
  v.frames = reinterpret_cast<ImageFrame*>(v.base + layout_.off_frames);
  v.pixels = v.base + layout_.off_pixels;
  v.pixel_capacity = layout_.pixel_capacity_bytes;
  return v;
}

uint64_t LockedBlockPool::PeekGroupId(int index) {
  BlockView v = View(index);
  return v.group->group_id;
}

void LockedBlockPool::WakeAll() {
  cv_.notify_all();
}