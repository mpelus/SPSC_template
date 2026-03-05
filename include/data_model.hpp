#pragma once
#include <cstdint>
#include "config.hpp"

struct ImageFrame {
  uint64_t timestamp_ns;
  uint32_t width, height;
  PixelFormat fmt;
  uint32_t stride_bytes;
  uint32_t size_bytes;
  uint8_t* data; // points into a locked memory block
};

struct ImageGroup {
  uint64_t group_id;
  uint32_t frame_count; // variable length per group
  uint64_t start_ts_ns, end_ts_ns;
  ImageFrame* frames; // points into same locked block
  uint32_t reserved0 = 0; // optional small metadata
};