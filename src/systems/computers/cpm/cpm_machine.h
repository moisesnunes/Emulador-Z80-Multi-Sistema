#pragma once

#include "core/bus/z80_bus.h"
#include "core/machine.h"
#include "zilogZ80.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace systems::computers::cpm {

class CpmMachine final : public core::Machine, public core::Z80Bus {
public:
  CpmMachine();

  const core::MachineInfo &info() const override;
  bool loadMedia(const std::filesystem::path &path, std::string &error) override;
  void reset() override;
  void runFrame() override;
  core::VideoFrame videoFrame() const override;
  void keyDown(int nativeKey) override;
  void keyUp(int nativeKey) override;

  uint8_t read(uint16_t address) override;
  void write(uint16_t address, uint8_t value) override;
  uint8_t in(uint16_t port) override;
  void out(uint16_t port, uint8_t value) override;

  std::string terminalText() const;

private:
  static constexpr int Columns = 80;
  static constexpr int Rows = 25;
  static constexpr int CellWidth = 8;
  static constexpr int CellHeight = 16;
  static constexpr int Width = Columns * CellWidth;
  static constexpr int Height = Rows * CellHeight;

  void installTransientProgram();
  bool handleBdos();
  bool hasCompleteLine() const;
  uint8_t popConsoleChar();
  void queueConsoleChar(uint8_t ch);
  void finishBdosCall(uint16_t returnAddress);
  void putChar(char ch);
  void newLine();
  void renderTerminal() const;

  std::array<uint8_t, 0x10000> ram_;
  std::vector<uint8_t> program_;
  std::array<char, Columns * Rows> cells_;
  mutable std::array<uint8_t, Width * Height * 4> pixels_;
  std::deque<uint8_t> inputQueue_;
  zilogZ80 cpu_;
  int cursorX_;
  int cursorY_;
};

} // namespace systems::computers::cpm
