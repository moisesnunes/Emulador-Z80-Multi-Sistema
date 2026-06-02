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

namespace systems::consoles::mastersystem {

class MasterSystemMachine final : public core::Machine, public core::Z80Bus {
public:
  MasterSystemMachine();

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

private:
  uint8_t readRomBank(uint8_t bank, uint16_t offset) const;
  void writeMapper(uint16_t address, uint8_t value);
  uint8_t readControllerA() const;
  uint8_t readControllerB() const;

  std::vector<uint8_t> rom_;
  std::array<uint8_t, 0x2000> ram_;
  std::array<uint8_t, 0x8000> cartridgeRam_;
  std::array<uint8_t, 3> mapperBanks_;
  std::array<uint8_t, 2> controller_;
  zilogZ80 cpu_;
  bool cartridgeRamEnabled_;
  uint8_t cartridgeRamBank_;
  uint8_t memoryControl_;
  uint8_t ioControl_;
  uint8_t psgLatch_;
  core::Tms9918a vdpCompat_;
};

} // namespace systems::consoles::mastersystem
