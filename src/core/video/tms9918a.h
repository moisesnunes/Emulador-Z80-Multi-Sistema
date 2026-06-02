#pragma once

#include "core/video_frame.h"

#include <array>
#include <cstdint>

namespace core {

class Tms9918a {
public:
  static constexpr int Width = 256;
  static constexpr int Height = 192;

  Tms9918a();

  void reset();
  uint8_t readData();
  uint8_t readStatus();
  void writeData(uint8_t value);
  void writeControl(uint8_t value);
  void setVBlank(bool active);
  bool interruptLine() const;
  void render();
  VideoFrame frame() const;

private:
  void putPixel(int x, int y, uint8_t colorIndex);
  void renderBackdrop();
  void renderGraphics1();

  std::array<uint8_t, 0x4000> vram_;
  std::array<uint8_t, 8> regs_;
  std::array<uint8_t, Width * Height * 4> pixels_;
  uint8_t status_;
  uint16_t address_;
  uint8_t latch_;
  bool latched_;
  uint8_t readBuffer_;
  bool writeMode_;
};

} // namespace core
