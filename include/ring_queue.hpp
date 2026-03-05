#pragma once
#include <atomic>
#include <cstdint>

// Bounded ring queue with per-slot sequence numbers.
// - Producer: TryPush()
// - Consumer: TryPop()
// - Producer (Latest policy): TryDropOne() to drop oldest queued item safely.
template <int Capacity>
class RingQueue {
public:
  RingQueue() {
    static_assert(Capacity > 1);
    for (uint64_t i = 0; i < static_cast<uint64_t>(Capacity); ++i) {
      slots_[i].seq.store(i, std::memory_order_relaxed);
      slots_[i].value = -1;
    }
  }

  // Producer thread only.
  bool TryPush(int v) {
    uint64_t t = tail_.load(std::memory_order_relaxed);
    Slot& s = slots_[t % Capacity];
    uint64_t seq = s.seq.load(std::memory_order_acquire);
    if (seq != t) return false; // not free

    s.value = v;                                  // payload first
    s.seq.store(t + 1, std::memory_order_release); // then publish

    tail_.store(t + 1, std::memory_order_release);
    depth_.fetch_add(1, std::memory_order_release);
    return true;
  }

  // Consumer thread only.
  bool TryPop(int& out_v) {
    while (true) {
      const uint64_t h = head_.load(std::memory_order_relaxed);
      const uint64_t t = tail_.load(std::memory_order_acquire);
      if (h == t) return false; // empty

      Slot& s = slots_[h % Capacity];
      const uint64_t seq = s.seq.load(std::memory_order_acquire);

      if (seq == h + 1) {
        uint64_t expected = h + 1;
        if (!s.seq.compare_exchange_strong(expected, h + static_cast<uint64_t>(Capacity),
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
          continue;
        }
        out_v = s.value;
        head_.store(h + 1, std::memory_order_release);
        depth_.fetch_sub(1, std::memory_order_release);
        return true;
      }

      if (seq < h + 1) {
        return false; // transient: not yet published
      }

      // already consumed/dropped -> advance head to catch up
      head_.store(h + 1, std::memory_order_release);
    }
  }

  enum class DropAttempt { Dropped, Skipped, Nothing };

  // Producer thread only. Attempts to drop the oldest queued item.
  DropAttempt TryDropOne(uint64_t& drop_cursor, int& out_v) {
    const uint64_t h = head_.load(std::memory_order_acquire);
    if (drop_cursor < h) drop_cursor = h;

    const uint64_t t = tail_.load(std::memory_order_acquire);
    if (drop_cursor >= t) return DropAttempt::Nothing;

    Slot& s = slots_[drop_cursor % Capacity];
    const uint64_t seq = s.seq.load(std::memory_order_acquire);

    if (seq == drop_cursor + 1) {
      uint64_t expected = drop_cursor + 1;
      if (!s.seq.compare_exchange_strong(expected, drop_cursor + static_cast<uint64_t>(Capacity),
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        return DropAttempt::Nothing;
      }
      out_v = s.value;
      ++drop_cursor;
      depth_.fetch_sub(1, std::memory_order_release);
      return DropAttempt::Dropped;
    }

    if (seq > drop_cursor + 1) {
      ++drop_cursor;
      return DropAttempt::Skipped;
    }

    return DropAttempt::Nothing;
  }

  uint64_t Depth() const { return depth_.load(std::memory_order_acquire); }

private:
  struct Slot {
    std::atomic<uint64_t> seq;
    int value;
  };

  alignas(64) Slot slots_[Capacity];
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) std::atomic<uint64_t> depth_{0};
};