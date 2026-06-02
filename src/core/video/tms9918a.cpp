#include "core/video/tms9918a.h"

#include <algorithm>

namespace core {
namespace {

constexpr uint8_t kPalette[16][3] = {
    {0, 0, 0},       {0, 0, 0},       {62, 184, 73},   {116, 208, 125},
    {89, 85, 224},   {128, 118, 241}, {185, 94, 81},   {101, 219, 239},
    {219, 101, 89},  {255, 137, 125}, {204, 195, 94},  {222, 208, 135},
    {58, 162, 65},   {183, 102, 181}, {204, 204, 204}, {255, 255, 255},
};

} // namespace

Tms9918a::Tms9918a() { reset(); }

void Tms9918a::reset() {
  vram_.fill(0);
  regs_.fill(0);
  pixels_.fill(0);
  status_ = 0;
  address_ = 0;
  latch_ = 0;
  latched_ = false;
  readBuffer_ = 0;
  writeMode_ = true;
  render();
}

uint8_t Tms9918a::readData() {
  latched_ = false;
  uint8_t value = readBuffer_;
  readBuffer_ = vram_[address_ & 0x3fff];
  address_ = (address_ + 1) & 0x3fff;
  return value;
}

uint8_t Tms9918a::readStatus() {
  latched_ = false;
  uint8_t value = status_;
  status_ &= ~0xe0;
  return value;
}

void Tms9918a::writeData(uint8_t value) {
  latched_ = false;
  vram_[address_ & 0x3fff] = value;
  address_ = (address_ + 1) & 0x3fff;
}

void Tms9918a::writeControl(uint8_t value) {
  if (!latched_) {
    latch_ = value;
    latched_ = true;
    return;
  }

  latched_ = false;
  if (value & 0x80) {
    regs_[value & 0x07] = latch_;
    return;
  }

  address_ = (static_cast<uint16_t>(latch_) |
              (static_cast<uint16_t>(value & 0x3f) << 8)) &
             0x3fff;
  writeMode_ = (value & 0x40) != 0;
  if (!writeMode_) {
    readBuffer_ = vram_[address_ & 0x3fff];
    address_ = (address_ + 1) & 0x3fff;
  }
}

void Tms9918a::setVBlank(bool active) {
  if (active)
    status_ |= 0x80;
  else
    status_ &= ~0x80;
}

bool Tms9918a::interruptLine() const {
  return (status_ & 0x80) && (regs_[1] & 0x20);
}

void Tms9918a::putPixel(int x, int y, uint8_t colorIndex) {
  if (x < 0 || x >= Width || y < 0 || y >= Height)
    return;

  const uint8_t *rgb = kPalette[colorIndex & 0x0f];
  std::size_t offset = static_cast<std::size_t>(y * Width + x) * 4;
  pixels_[offset + 0] = rgb[0];
  pixels_[offset + 1] = rgb[1];
  pixels_[offset + 2] = rgb[2];
  pixels_[offset + 3] = 255;
}

void Tms9918a::renderBackdrop() {
  uint8_t bg = regs_[7] & 0x0f;
  if (bg == 0)
    bg = 1;

  const uint8_t *rgb = kPalette[bg];
  for (int i = 0; i < Width * Height; i++) {
    pixels_[i * 4 + 0] = rgb[0];
    pixels_[i * 4 + 1] = rgb[1];
    pixels_[i * 4 + 2] = rgb[2];
    pixels_[i * 4 + 3] = 255;
  }
}

void Tms9918a::renderGraphics1() {
  uint16_t nameBase = static_cast<uint16_t>(regs_[2] & 0x0f) << 10;
  uint16_t colorBase = static_cast<uint16_t>(regs_[3]) << 6;
  uint16_t patternBase = static_cast<uint16_t>(regs_[4] & 0x07) << 11;
  uint8_t backdrop = regs_[7] & 0x0f;
  if (backdrop == 0)
    backdrop = 1;

  for (int row = 0; row < 24; row++) {
    for (int col = 0; col < 32; col++) {
      uint8_t tile = vram_[(nameBase + row * 32 + col) & 0x3fff];
      uint8_t colorByte = vram_[(colorBase + tile / 8) & 0x3fff];
      uint8_t fg = (colorByte >> 4) & 0x0f;
      uint8_t bg = colorByte & 0x0f;
      if (fg == 0)
        fg = backdrop;
      if (bg == 0)
        bg = backdrop;

      for (int py = 0; py < 8; py++) {
        uint8_t bits = vram_[(patternBase + tile * 8 + py) & 0x3fff];
        for (int px = 0; px < 8; px++) {
          uint8_t color = (bits & (0x80 >> px)) ? fg : bg;
          putPixel(col * 8 + px, row * 8 + py, color);
        }
      }
    }
  }
}

void Tms9918a::render() {
  renderBackdrop();

  bool m1 = (regs_[1] & 0x10) != 0;
  bool m2 = (regs_[1] & 0x08) != 0;
  bool m3 = (regs_[0] & 0x02) != 0;
  if (!m1 && !m2 && !m3)
    renderGraphics1();
}

VideoFrame Tms9918a::frame() const {
  return {pixels_.data(), Width, Height, Width * 4, PixelFormat::RGBA8888};
}

} // namespace core
