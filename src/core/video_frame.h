#pragma once

#include <cstdint>

namespace core {

enum class PixelFormat {
  RGB888,
  RGBA8888,
};

struct VideoFrame {
  const uint8_t *pixels = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  PixelFormat format = PixelFormat::RGBA8888;

  bool valid() const { return pixels && width > 0 && height > 0 && stride > 0; }
};

} // namespace core
