#include <chrono>
#include <csignal>
#include <thread>

#include "analyzer.hpp"
#include "block_layout.hpp"
#include "camera_emulator.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "locked_block_pool.hpp"
#include "logging.hpp"
#include "ring_queue.hpp"
#include "shared_sync.hpp"

extern "C" void HandleSignal(int signo) {
  (void)signo;
  g_stop.store(true, std::memory_order_relaxed);
}

int main() {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  BlockLayout layout;
  Log("[Main] frame_bytes=", layout.frame_bytes,
      " (", kWidth, "x", kHeight, " ",
      (kPixelFormat == PixelFormat::GRAY8 ? "GRAY8" : "RGB24"), ")");
  Log("[Main] block_bytes=", layout.block_bytes,
      " off_group=", layout.off_group,
      " off_frames=", layout.off_frames,
      " off_pixels=", layout.off_pixels);
  Log("[Main] policy=", (kPolicy == Policy::Latest ? "Latest" : "Exhaust"),
      " runtime=", kRunSeconds, "s");

  LockedBlockPool pool(layout);
  RingQueue<kQueueCapacity> queue;
  SharedSync sync;
  Stats stats;

  CameraEmulator producer(pool, queue, sync, stats);
  Analyzer consumer(pool, queue, sync, stats);

  std::thread t_prod(std::ref(producer));
  std::thread t_cons(std::ref(consumer));

  uint64_t last_p = 0, last_c = 0, last_d = 0;
  auto start = std::chrono::steady_clock::now();
  auto last_tick = start;

  while (!g_stop.load(std::memory_order_relaxed)) {
    auto now = std::chrono::steady_clock::now();

    auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
    if (elapsed_s >= kRunSeconds) {
      g_stop.store(true, std::memory_order_relaxed);
      break;
    }

    if (now - last_tick >= std::chrono::seconds(1)) {
      last_tick += std::chrono::seconds(1);

      uint64_t p = stats.produced.load(std::memory_order_relaxed);
      uint64_t c = stats.consumed.load(std::memory_order_relaxed);
      uint64_t d = stats.dropped.load(std::memory_order_relaxed);

      uint64_t dp = p - last_p;
      uint64_t dc = c - last_c;
      uint64_t dd = d - last_d;
      last_p = p; last_c = c; last_d = d;

      Log("[Summary] produced/s=", dp,
          " consumed/s=", dc,
          " dropped/s=", dd,
          " depth=", queue.Depth(),
          " free_blocks=", pool.FreeCount());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // shutdown + wake any waiters
  sync.WakeAll();
  pool.WakeAll();

  t_prod.join();
  t_cons.join();

  Log("[Main] totals: produced=", stats.produced.load(),
      " consumed=", stats.consumed.load(),
      " dropped=", stats.dropped.load(),
      " depth=", queue.Depth(),
      " free_blocks=", pool.FreeCount(),
      " (expected ", kPoolBlocks, ")");

  return 0;
}