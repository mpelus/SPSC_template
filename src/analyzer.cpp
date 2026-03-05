#include "analyzer.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <thread>

#include "globals.hpp"
#include "logging.hpp"

Analyzer::Analyzer(LockedBlockPool& pool,
                   RingQueue<kQueueCapacity>& q,
                   SharedSync& sync,
                   Stats& stats)
  : pool_(pool), q_(q), sync_(sync), stats_(stats) {}

void Analyzer::operator()() {
  std::mt19937 rng(static_cast<uint32_t>(NowNs() ^ 0xA5A5A5A5u));
  std::uniform_int_distribution<int> sleep_ms(kAnalyzerSleepMinMs, kAnalyzerSleepMaxMs);

  const auto& layout = pool_.Layout();
  const size_t frame_bytes = layout.frame_bytes;
  const size_t analysis_capacity = static_cast<size_t>(kMaxFramesPerGroup) * frame_bytes;

  // Reusable analysis buffer (no per-group heap allocations).
  std::unique_ptr<uint8_t[]> analysis_buf(new uint8_t[analysis_capacity]);

  while (true) {
    int block_idx = -1;
    if (q_.TryPop(block_idx)) {
      stats_.consumed.fetch_add(1, std::memory_order_relaxed);
      sync_.cv_space.notify_one();

      BlockView v = pool_.View(block_idx);
      ImageGroup* g = v.group;
      const uint32_t fc = g->frame_count;
      const size_t bytes_to_copy = static_cast<size_t>(fc) * frame_bytes;

      Log("[Consumer] dequeued group=", g->group_id,
          " frames=", fc,
          " block=", block_idx,
          " copy_bytes=", bytes_to_copy,
          " depth=", q_.Depth());

      auto t0 = std::chrono::steady_clock::now();

      for (uint32_t i = 0; i < fc; ++i) {
        const ImageFrame& f = g->frames[i];
        std::memcpy(analysis_buf.get() + static_cast<size_t>(i) * frame_bytes,
                    f.data,
                    f.size_bytes);
      }

      Log("[Consumer] analysis start group=", g->group_id);

      if (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms(rng)));
      }

      auto t1 = std::chrono::steady_clock::now();
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

      Log("[Consumer] analysis end   group=", g->group_id,
          " elapsed_ms=", ms,
          " returning block=", block_idx);

      pool_.Release(block_idx);
      continue;
    }

    // If stopping, exit after draining queue.
    if (g_stop.load(std::memory_order_relaxed)) {
      if (q_.Depth() == 0) break;
    }

    std::unique_lock<std::mutex> lk(sync_.mu);
    sync_.cv_items.wait_for(lk, std::chrono::milliseconds(50), [&] {
      return g_stop.load(std::memory_order_relaxed) || q_.Depth() > 0;
    });
  }

  Log("[Consumer] exiting");
}