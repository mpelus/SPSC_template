#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

struct SharedSync {
  std::mutex mu;
  std::condition_variable cv_items;
  std::condition_variable cv_space;

  void WakeAll() {
    cv_items.notify_all();
    cv_space.notify_all();
  }
};

struct Stats {
  std::atomic<uint64_t> produced{0};
  std::atomic<uint64_t> consumed{0};
  std::atomic<uint64_t> dropped{0};
};