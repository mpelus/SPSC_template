#pragma once
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include "globals.hpp"

template <class... Ts>
inline void Log(Ts&&... xs) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  (std::cout << ... << xs) << std::endl;
}

inline uint64_t NowNs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

inline size_t AlignUp(size_t x, size_t a) {
  return (x + (a - 1)) & ~(a - 1);
}