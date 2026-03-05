#pragma once
#include "config.hpp"
#include "locked_block_pool.hpp"
#include "ring_queue.hpp"
#include "shared_sync.hpp"

class CameraEmulator {
public:
  CameraEmulator(LockedBlockPool& pool,
                 RingQueue<kQueueCapacity>& q,
                 SharedSync& sync,
                 Stats& stats);

  void operator()();

private:
  LockedBlockPool& pool_;
  RingQueue<kQueueCapacity>& q_;
  SharedSync& sync_;
  Stats& stats_;
};