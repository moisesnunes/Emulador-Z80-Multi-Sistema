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

namespace systems::consoles::sg1000 {

class SG1000Machine final : public core::Machine, public core::Z80Bus {
public:
  SG1000Machine();

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
  uint8_t readControllerA() const;
  uint8_t readControllerB() const;

  std::vector<uint8_t> rom_;
  std::array<uint8_t, 0x4000> expansion_;
  std::array<uint8_t, 0x0400> ram_;
  std::array<uint8_t, 2> controller_;
  zilogZ80 cpu_;
  core::Tms9918a vdp_;
  uint8_t psgLatch_;
};

} // namespace systems::consoles::sg1000
