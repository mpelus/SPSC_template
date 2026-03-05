#pragma once
#include <cstddef>
#include <cstdint>
#include <unistd.h>
#include "config.hpp"
#include "data_model.hpp"
#include "logging.hpp"

struct alignas(64) BlockHeader {
  uint32_t magic = 0xB10C0B10u;
  uint32_t block_index = 0;
  uint32_t max_frames = 0;
  uint32_t frame_bytes = 0;
  uint32_t pixel_capacity_bytes = 0;
  uint32_t pixel_used_bytes = 0;
  uint64_t last_group_id = 0;
};

struct BlockLayout {
  size_t page_size = 4096;
  uint32_t frame_bytes = 0;

  size_t off_header = 0;
  size_t off_group = 0;
  size_t off_frames = 0;
  size_t off_pixels = 0;

  size_t pixel_capacity_bytes = 0;
  size_t block_bytes = 0;

  BlockLayout() {
    page_size = static_cast<size_t>(::sysconf(_SC_PAGESIZE));

    const uint32_t bpp = BytesPerPixel(kPixelFormat);
    const uint32_t stride = kWidth * bpp;
    frame_bytes = stride * kHeight;

    off_header = 0;
    off_group = AlignUp(sizeof(BlockHeader), alignof(ImageGroup));
    off_frames = AlignUp(off_group + sizeof(ImageGroup), alignof(ImageFrame));
    off_pixels = AlignUp(off_frames + static_cast<size_t>(kMaxFramesPerGroup) * sizeof(ImageFrame), 64);

    pixel_capacity_bytes = static_cast<size_t>(kMaxFramesPerGroup) * static_cast<size_t>(frame_bytes);
    block_bytes = AlignUp(off_pixels + pixel_capacity_bytes, page_size);
  }
};

struct BlockView {
  uint8_t* base = nullptr;
  BlockHeader* header = nullptr;
  ImageGroup* group = nullptr;
  ImageFrame* frames = nullptr;
  uint8_t* pixels = nullptr;
  size_t pixel_capacity = 0;
};