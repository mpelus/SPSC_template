#include "camera_emulator.hpp"

#include <chrono>
#include <cstring>
#include <random>
#include <thread>

#include "globals.hpp"
#include "logging.hpp"

CameraEmulator::CameraEmulator(LockedBlockPool& pool,
                               RingQueue<kQueueCapacity>& q,
                               SharedSync& sync,
                               Stats& stats)
  : pool_(pool), q_(q), sync_(sync), stats_(stats) {}

void CameraEmulator::operator()() {
  std::mt19937 rng(static_cast<uint32_t>(NowNs()));
  std::uniform_int_distribution<int> sleep_ms(kProducerSleepMinMs, kProducerSleepMaxMs);
  std::uniform_int_distribution<int> frame_count_dist(1, kMaxFramesPerGroup);

  const auto& layout = pool_.Layout();
  const uint32_t bpp = BytesPerPixel(kPixelFormat);
  const uint32_t stride = kWidth * bpp;
  const uint32_t frame_bytes = layout.frame_bytes;

  uint64_t group_id = 1;
  uint64_t drop_cursor = 0;

  while (!g_stop.load(std::memory_order_relaxed)) {
    int block_idx = -1;
    if (!pool_.Acquire(block_idx)) break;

    // Fill block with a new ImageGroup.
    BlockView v = pool_.View(block_idx);
    v.header->pixel_used_bytes = 0;
    v.header->last_group_id = group_id;

    ImageGroup* g = v.group;
    g->group_id = group_id;
    g->frame_count = static_cast<uint32_t>(frame_count_dist(rng));
    g->frames = v.frames;

    const uint64_t start_ts = NowNs();
    g->start_ts_ns = start_ts;

    const uint32_t fc = g->frame_count;
    const size_t needed = static_cast<size_t>(fc) * static_cast<size_t>(frame_bytes);
    if (needed > v.pixel_capacity) {
      Log("[Producer] ERROR: payload too large for block. needed=", needed,
          " cap=", v.pixel_capacity, ". Stopping.");
      g_stop.store(true, std::memory_order_relaxed);
      pool_.Release(block_idx);
      break;
    }

    for (uint32_t i = 0; i < fc; ++i) {
      ImageFrame& f = v.frames[i];
      f.timestamp_ns = start_ts + static_cast<uint64_t>(i) * 1'000'000; // +1ms per frame
      f.width = kWidth;
      f.height = kHeight;
      f.fmt = kPixelFormat;
      f.stride_bytes = stride;
      f.size_bytes = frame_bytes;
      f.data = v.pixels + static_cast<size_t>(i) * static_cast<size_t>(frame_bytes);

      std::memset(f.data, static_cast<int>((group_id + i) & 0xFFu), frame_bytes);
    }

    g->end_ts_ns = v.frames[fc - 1].timestamp_ns;
    v.header->pixel_used_bytes = static_cast<uint32_t>(needed);

    Log("[Producer] acquired block=", block_idx,
        " created group=", group_id,
        " frames=", fc,
        " bytes=", needed);

    // Enqueue with policy.
    bool enqueued = false;
    while (!g_stop.load(std::memory_order_relaxed)) {
      if (q_.TryPush(block_idx)) {
        stats_.produced.fetch_add(1, std::memory_order_relaxed);
        sync_.cv_items.notify_one();
        Log("[Producer] queued group=", group_id,
            " block=", block_idx,
            " depth=", q_.Depth());
        enqueued = true;
        break;
      }

      if constexpr (kPolicy == Policy::Latest) {
        int dropped_block = -1;
        auto res = q_.TryDropOne(drop_cursor, dropped_block);

        if (res == RingQueue<kQueueCapacity>::DropAttempt::Dropped) {
          const uint64_t dropped_gid = pool_.PeekGroupId(dropped_block);
          pool_.Release(dropped_block);
          stats_.dropped.fetch_add(1, std::memory_order_relaxed);
          Log("[Producer] Latest: DROPPED oldest group=", dropped_gid,
              " block=", dropped_block,
              " depth=", q_.Depth());
          sync_.cv_space.notify_one();
          continue;
        }

        if (res == RingQueue<kQueueCapacity>::DropAttempt::Skipped) {
          continue;
        }

        std::this_thread::yield();
      } else { // Exhaust
        Log("[Producer] Exhaust: queue full, waiting...");
        std::unique_lock<std::mutex> lk(sync_.mu);
        sync_.cv_space.wait_for(lk, std::chrono::milliseconds(50), [&] {
          return g_stop.load(std::memory_order_relaxed) ||
                 q_.Depth() < static_cast<uint64_t>(kQueueCapacity);
        });
      }
    }

    if (!enqueued) {
      pool_.Release(block_idx);
      break;
    }

    ++group_id;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms(rng)));
  }

  Log("[Producer] exiting");
}