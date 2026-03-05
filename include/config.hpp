#pragma once
#include <cstdint>

enum class Policy { Latest, Exhaust };

// Easy to edit:
inline constexpr Policy kPolicy = Policy::Latest;

// Runtime:
inline constexpr int kRunSeconds = 60;

// Pool/queue:
inline constexpr int kPoolBlocks = 32;
inline constexpr int kQueueCapacity = 32; // bounded ring capacity
static_assert(kQueueCapacity >= 2, "Queue capacity must be >= 2");

// Groups/frames:
inline constexpr int kMaxFramesPerGroup = 24;
static_assert(kMaxFramesPerGroup >= 1, "MaxFramesPerGroup must be >= 1");

// Frame format:
enum class PixelFormat { GRAY8, RGB24 };

inline constexpr uint32_t kWidth = 2040;
inline constexpr uint32_t kHeight = 1024;
inline constexpr PixelFormat kPixelFormat = PixelFormat::GRAY8;

// Timing:
inline constexpr int kProducerSleepMinMs = 10;
inline constexpr int kProducerSleepMaxMs = 30;
inline constexpr int kAnalyzerSleepMinMs = 200;
inline constexpr int kAnalyzerSleepMaxMs = 300;

constexpr uint32_t BytesPerPixel(PixelFormat f) {
  switch (f) {
    case PixelFormat::GRAY8: return 1;
    case PixelFormat::RGB24: return 3;
  }
  return 1;
}