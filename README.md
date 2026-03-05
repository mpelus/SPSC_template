# SPSC Camera Producer + Analyzer Consumer Project (Component-by-Component)

This project emulates:
- A **fast producer** that generates variable-length **ImageGroup** batches of frames (camera-like).
- A **slow consumer** that copies pixels into a local buffer and sleeps to emulate CV work.
- A **fixed-size locked-memory pool** of blocks (mlock/munlock) with explicit ownership transfer.
- A **bounded SPSC ring queue** with correct acquire/release ordering.
- A **policy switch** between:
  - **Latest**: producer never blocks; it drops the oldest queued items to make room.
  - **Exhaust**: producer blocks when queue is full; no drops.

---

# Build System

## `CMakeLists.txt`
**Purpose:** Defines how to build the project with CMake on Ubuntu (g++), enabling C++20 and pthreads.

**Key responsibilities:**
- Sets `CMAKE_CXX_STANDARD 20`
- Builds one executable (`app`) from the `.cpp` sources
- Adds `include/` to the include path
- Links `Threads::Threads` (required for `std::thread` on Linux)
- Adds common warnings and `-O2`

---

# Configuration + Data Types

## `include/config.hpp`
**Purpose:** Central place for compile-time configuration constants and small enums.

**What it defines:**
- `enum class Policy { Latest, Exhaust };`
- `inline constexpr Policy kPolicy`: edit to switch behavior.
- Runtime constants:
  - `kRunSeconds` (fixed runtime)
- Pool/queue constants:
  - `kPoolBlocks`, `kQueueCapacity`
- Frame/group constants:
  - `kMaxFramesPerGroup`
- Frame format and size:
  - `PixelFormat { GRAY8, RGB24 }`
  - `kWidth`, `kHeight`, `kPixelFormat`
- Timing:
  - Producer sleep range (fast)
  - Analyzer sleep range (slow)
- `BytesPerPixel(PixelFormat)`: utility to compute bytes per pixel.

**Why it exists:**
- Keeps the rest of the code clean and makes tuning easy.
- Ensures no CLI parsing is needed; everything is in source constants.

---

## `include/data_model.hpp`
**Purpose:** Holds the required data structures: `ImageFrame` and `ImageGroup`.

### `struct ImageFrame`
Represents one frame descriptor:
- `timestamp_ns`: frame timestamp (steady clock ns in this demo)
- `width`, `height`
- `fmt`: pixel format
- `stride_bytes`: bytes per row (width * bytes-per-pixel)
- `size_bytes`: total bytes in this frame payload
- `data`: pointer into the block’s pixel payload area

**Important:** `data` is not owned by the frame; it points into the currently owned pooled block.

### `struct ImageGroup`
Represents a batch of frames:
- `group_id`: unique ID for logging/tracking
- `frame_count`: variable length [1..kMaxFramesPerGroup]
- `start_ts_ns`, `end_ts_ns`: timing metadata
- `frames`: pointer to the block’s internal array of `ImageFrame` descriptors
- `reserved0`: optional placeholder metadata

**Important:** `frames` does not allocate; it points inside the same contiguous block.

---

# Globals + Logging

## `include/globals.hpp` and `src/globals.cpp`
**Purpose:** Hosts process-wide global objects shared by threads.

### `std::atomic<bool> g_stop`
- Global shutdown flag.
- Set to `true` by:
  - the fixed runtime logic in `main`
  - Ctrl+C (SIGINT) handler
- Read by producer and consumer loops to exit cleanly.

### `std::mutex g_log_mu`
- Used only for logging.
- Ensures `std::cout` lines do not interleave between threads.

---

## `include/logging.hpp`
**Purpose:** Small utilities used across the project.

### `Log(...)`
- Thread-safe logging wrapper around `std::cout`.
- Locks `g_log_mu`, prints a full line, then flushes by `std::endl`.

### `NowNs()`
- Returns steady-clock time in nanoseconds.
- Used for timestamps and for RNG seeding.

### `AlignUp(x, a)`
- Aligns a size to the next multiple of `a`.
- Used to build correct memory layout inside each block.

---

# Block Memory Layout (Single Allocation per Block)

## `include/block_layout.hpp`
**Purpose:** Defines the exact memory layout inside one pooled block.

### `struct BlockHeader`
Metadata stored at the beginning of each block:
- `magic`: debug marker (helps detect corruption/misuse)
- `block_index`: which block this is
- `max_frames`: fixed capacity
- `frame_bytes`: bytes per frame payload
- `pixel_capacity_bytes`: total payload capacity in this block
- `pixel_used_bytes`: how much payload current group used
- `last_group_id`: last group written into this block

**Why it exists:**
- Helps debugging and makes it easy to sanity-check memory usage.

### `struct BlockLayout`
Computes offsets and sizes (once at startup):
- Reads system page size via `sysconf(_SC_PAGESIZE)`
- Computes `frame_bytes` from width/height/format
- Computes offsets:
  - `off_group` aligned after header
  - `off_frames` aligned after group
  - `off_pixels` aligned after frame descriptors
- Computes:
  - `pixel_capacity_bytes = kMaxFramesPerGroup * frame_bytes`
  - `block_bytes` aligned up to page size

**Why alignment/page size matters:**
- `posix_memalign()` is used to page-align the allocation.
- `mlock()` works on memory pages; aligning to pages is correct and efficient.
- Aligning internal fields avoids misaligned accesses (important for performance and correctness).

### `struct BlockView`
Convenience “typed pointers” into a block:
- `base`: raw pointer
- `header`, `group`, `frames`, `pixels`: pointers into the block at known offsets
- `pixel_capacity`: for bounds checks

**Why it exists:**
- Prevents repeated pointer arithmetic scattered across the code.

---

# Locked Block Pool (Fixed N Blocks, mlock + reuse)

## `include/locked_block_pool.hpp` and `src/locked_block_pool.cpp`
**Purpose:** Manage a fixed number of blocks with optional mlock().

### What “pool” means here
- There are exactly `kPoolBlocks` blocks.
- Each block is one contiguous allocation containing:
  - header + group + frame descriptors + pixels
- No per-group heap allocations are done in the hot path.

### Constructor `LockedBlockPool::LockedBlockPool(layout)`
What it does:
1. Allocates each block with `posix_memalign(page_size, block_bytes)` so the base is page-aligned.
2. Zeroes the block.
3. Writes initial header fields (block index, capacities).
4. Attempts to `mlock()` each block:
   - If all succeed: locked-memory enabled.
   - If any fail:
     - logs an error explaining RLIMIT_MEMLOCK
     - munlocks any already locked blocks
     - continues with unlocked memory (still works, just not locked).

**Important behavior:**
- It is “all or nothing” for locking: either all blocks are locked or none are, to keep behavior consistent.

### `Acquire(int& out_index)`
- Blocks until:
  - a free block is available, OR
  - shutdown/stop occurs
- Returns `false` if shutting down.
- Returns a block index otherwise.

**Threading:**
- Uses a mutex + condition_variable internally.
- This is allowed because it’s not part of the ring queue hot path.

### `Release(int index)`
- Returns a block index to the free list.
- Notifies `cv_` so a blocked producer can continue.

### `View(int index)`
- Converts the raw pointer into a `BlockView` with typed pointers.

### `PeekGroupId(int index)`
- Used only for logging drops (Latest policy).
- Reads the group id from the block’s stored group struct.

### Destructor / `Shutdown()`
- If locking is enabled: munlocks all blocks.
- Frees all allocations.

**Leak prevention:**
- Blocks are always tracked by index.
- Ownership is explicit: pool → producer → queue → consumer → pool.

---

# Synchronization Helpers (only for waiting, not ring ops)

## `include/shared_sync.hpp`
**Purpose:** Condition variables used only for blocking waits / wake-ups.

### `struct SharedSync`
Contains:
- `mutex mu`
- `condition_variable cv_items`
  - Used when consumer wants to wait for items.
- `condition_variable cv_space`
  - Used when producer wants to wait for space (Exhaust policy).

### `WakeAll()`
- Notifies both CVs to ensure waiters wake during shutdown.

### `struct Stats`
Atomic counters:
- `produced`: incremented when producer successfully enqueues
- `consumed`: incremented when consumer successfully dequeues
- `dropped`: incremented when producer drops an old group (Latest)

These are used by the 1Hz summary printing in main.

---

# Ring Queue (Bounded SPSC + Safe Producer-Side Dropping)

## `include/ring_queue.hpp`
**Purpose:** Lock-free bounded queue for block indices with SPSC semantics and a safe drop mechanism for Latest policy.

### Core idea
The queue is a ring of slots.
Each slot has:
- `value` (the block index)
- `seq` (a sequence number describing the slot’s state)

The sequence number lets you tell whether a slot is:
- free for producer to write
- full for consumer (or producer-drop) to claim
- already consumed/dropped and available in a later “cycle”

This pattern avoids mutexes for ring operations.

### Internal state
- `head_`: consumer-owned position (monotonic counter)
- `tail_`: producer-owned position (monotonic counter)
- `depth_`: approximate depth counter for logging and waiting predicates

### `TryPush(int v)` (Producer only)
1. Reads current `tail` (relaxed).
2. Looks at slot `tail % Capacity`.
3. Loads slot `seq` with acquire:
   - If `seq != tail`, slot is not free → queue full at that position → returns false.
4. Writes `slot.value = v` (payload write).
5. Publishes by storing `slot.seq = tail+1` with release.
6. Advances `tail_` and increments `depth_`.

**Why release here matters:**
- It guarantees the value write is visible before the “full” marker is visible.

### `TryPop(int& out_v)` (Consumer only)
1. Reads `head` and `tail` (tail with acquire).
2. If `head == tail`, queue empty.
3. Loads slot `seq` with acquire.
4. If `seq == head+1`, the item is ready:
   - Consumer claims slot by CAS changing seq to `head+Capacity` (acq_rel).
   - Reads `value`.
   - Advances head and decrements depth.
5. If `seq > head+1`, it means this slot belongs to a later cycle (already consumed/dropped):
   - consumer advances head to catch up.

**Why CAS is used:**
- It prevents a race with producer-side dropping (Latest policy).
- Only one of them can “claim” a given queued item.

### Producer-side dropping: `TryDropOne(drop_cursor, out_v)` (Producer only)
Used only in Latest policy when queue is full.

- The producer keeps a `drop_cursor` which it moves forward over time.
- It never rewinds behind `head`.
- It uses the same sequence logic as consumer:
  - If slot is ready (`seq == cursor+1`), it claims it using CAS and returns `Dropped`.
  - If slot is already advanced (`seq > cursor+1`), it returns `Skipped` and moves cursor.
  - If not ready, returns `Nothing`.

**Why this is safe:**
- Dropping is done by atomically claiming the slot, exactly like consuming.
- The consumer will not also consume the same slot because CAS ensures exclusive claim.

### `Depth()`
Returns `depth_` (acquire), used for:
- logging
- wait predicates (Exhaust policy)

---

# Producer Thread: CameraEmulator

## `include/camera_emulator.hpp` and `src/camera_emulator.cpp`
**Purpose:** Simulate a camera producing image groups faster than analysis.

### Inputs (constructor)
- `LockedBlockPool& pool_`: where blocks come from and return to
- `RingQueue& q_`: where produced blocks are enqueued
- `SharedSync& sync_`: CVs for waking consumer / waiting in Exhaust
- `Stats& stats_`: counters

### What the producer loop does
1. **Acquire a free block** from the pool (`pool_.Acquire`).
2. Create a new group:
   - Choose random `frame_count` in [1..kMaxFramesPerGroup].
   - Fill `ImageGroup` in the block.
   - Fill `ImageFrame` descriptors array in the block:
     - timestamps, width/height, stride, size
     - `data` pointers point into the block’s payload area
3. Fill pixel payload:
   - For demo: `memset()` with a pattern `(group_id + frame_index) & 0xFF`.
4. Enqueue the block index into the ring according to policy:

#### Policy: Latest
- Producer never blocks.
- If `TryPush()` fails:
  - calls `TryDropOne()` repeatedly to drop oldest queued items until it can push.
  - for each dropped block index:
    - logs the dropped group ID
    - returns block to the pool immediately
    - increments dropped counter
- Then enqueues the newest block.

#### Policy: Exhaust
- If `TryPush()` fails:
  - logs “waiting”
  - waits on `cv_space` for up to 50ms, rechecking conditions
  - consumer notifies `cv_space` whenever it pops an item

5. Sleeps 10–30ms to simulate camera interval.

### Ownership transitions (producer perspective)
- Before enqueue: producer owns block exclusively.
- After successful enqueue: consumer (eventually) owns it.
- If dropped: producer reclaims it by CAS (drop) and returns it to pool.

---

# Consumer Thread: Analyzer

## `include/analyzer.hpp` and `src/analyzer.cpp`
**Purpose:** Simulate slow CV analysis that copies image data locally.

### Inputs (constructor)
- same shared objects as producer

### What the consumer loop does
1. Tries to pop a block index (`TryPop`).
2. If it got one:
   - increments consumed counter
   - notifies `cv_space` (helps Exhaust policy)
   - reads group metadata and frame descriptors from the block
3. Copies pixels into a reusable local buffer:
   - allocates `analysis_buf` once sized for max possible payload
   - `memcpy` each frame’s payload into this buffer
   - this models “analysis takes ownership by copying”
4. Sleeps 80–150ms to emulate slow compute.
5. Returns the block to the pool (`pool_.Release`).
6. If no item is available:
   - waits on `cv_items` (up to 50ms) to reduce CPU spinning

### Shutdown behavior
- When `g_stop` becomes true:
  - consumer keeps running until the queue drains (`Depth() == 0`)
  - then exits

---

# Program Orchestration

## `src/main.cpp`
**Purpose:** Starts threads, prints summaries, handles shutdown.

### What main does
1. Installs signal handlers for SIGINT/SIGTERM:
   - Ctrl+C sets `g_stop = true`
2. Constructs `BlockLayout` and prints layout details:
   - `frame_bytes`, offsets, total block bytes
3. Creates:
   - `LockedBlockPool pool(layout)`
   - `RingQueue queue`
   - `SharedSync sync`
   - `Stats stats`
4. Starts exactly two threads:
   - `CameraEmulator` (producer)
   - `Analyzer` (consumer)
5. Runs a loop in main:
   - stops after `kRunSeconds` OR Ctrl+C
   - prints once-per-second summary:
     - produced/s, consumed/s, dropped/s
     - queue depth
     - free blocks
6. On shutdown:
   - sets stop flag
   - wakes condition variables (`sync.WakeAll()` and `pool.WakeAll()`)
   - joins producer and consumer threads
   - prints totals and free-block count

### Invariants enforced at end
- `free_blocks` should equal `kPoolBlocks`
- queue depth should be 0 (or be drained by consumer shutdown path)

---

# How the Whole System Works Together (End-to-End)

1. Pool owns all blocks initially (free list contains all indices).
2. Producer:
   - takes a free block from pool
   - writes group+frames+pixels into it
   - transfers ownership by pushing index into ring
3. Consumer:
   - pops index from ring
   - reads the group in-place
   - copies pixels into local buffer
   - sleeps (analysis)
   - returns block to pool
4. Policy behavior:
   - Latest: producer may claim+drop old queued slots to keep newest data flowing.
   - Exhaust: producer waits for consumer to free queue space; no drops.
5. Shutdown:
   - stop flag stops producer quickly
   - consumer drains remaining queued items (if any)
   - all blocks return to pool

---

# Why This Meets the Requirements

- **Two worker threads only:** Producer + Consumer; main just orchestrates.
- **No hot-path heap allocations:** frames and pixels are inside blocks; consumer allocates analysis buffer once.
- **Locked memory is explicit:** page-aligned blocks; mlock at init; munlock at shutdown; graceful fallback if mlock fails.
- **SPSC bounded ring:** lock-free ring ops using atomics + acquire/release; condition variables only for waiting.
- **Latest semantics:** drop oldest queued items (safe CAS claim) until enqueue succeeds; producer never blocks.
- **Exhaust semantics:** producer blocks on `cv_space` until there is room.
- **Concise logs + 1Hz summary:** producer/consumer logs and summary from main.
- **Clean shutdown:** atomic stop, wake waiters, join threads, blocks accounted for.