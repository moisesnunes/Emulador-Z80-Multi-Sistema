#pragma once

#include "core/bus/z80_bus.h"
#include "core/machine.h"
#include "core/video/tms9918a.h"
#include "zilogZ80.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace systems::computers::msx {

class AresStyleMsxMachine final : public core::Machine, public core::Z80Bus {
public:
  AresStyleMsxMachine();

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

  bool loadBios(const std::filesystem::path &path, std::string &error);

private:
  static constexpr std::size_t SlotSize = 0x10000;

  uint8_t readPrimarySlot() const;
  void writePrimarySlot(uint8_t value);
  uint8_t readKeyboard() const;
  uint8_t readPsg() const;
  bool loadCartridge(const std::vector<uint8_t> &rom, std::string &error);
  uint16_t detectCartridgeAddress(const std::vector<uint8_t> &rom) const;

  std::array<uint8_t, SlotSize> bios_;
  std::array<uint8_t, SlotSize> cartridge_;
  std::array<uint8_t, SlotSize> expansion_;
  std::array<uint8_t, SlotSize> ram_;
  std::array<uint8_t, 11> keyboardRows_;
  std::array<uint8_t, 16> psg_;
  std::array<uint8_t, 2> joystick_;
  std::array<uint8_t, 4> primarySlot_;
  zilogZ80 cpu_;
  uint8_t keyboardRow_;
  uint8_t psgLatch_;
  core::Tms9918a vdp_;
};

} // namespace systems::computers::msx
