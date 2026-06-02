#pragma once

#include "core/machine.h"
#include "intel8080.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace systems::arcade::invaders {

class SpaceInvadersMachine;

class SpaceInvadersCpu final : public intel8080 {
public:
  explicit SpaceInvadersCpu(SpaceInvadersMachine *machine);

  uint8_t InPort8(uint16_t port) override;
  void OutPort8(uint16_t port, uint8_t value) override;

private:
  SpaceInvadersMachine *machine_;
};

class SpaceInvadersMachine final : public core::Machine {
public:
  SpaceInvadersMachine();

  const core::MachineInfo &info() const override;
  bool loadMedia(const std::filesystem::path &path, std::string &error) override;
  void reset() override;
  void runFrame() override;
  core::VideoFrame videoFrame() const override;
  void keyDown(int nativeKey) override;
  void keyUp(int nativeKey) override;

  uint8_t read(uint16_t address) const;
  uint8_t in(uint8_t port) const;
  void out(uint8_t port, uint8_t value);

private:
  static constexpr int Width = 224;
  static constexpr int Height = 256;
  static constexpr uint16_t RomEnd = 0x2000;
  static constexpr uint16_t VramStart = 0x2400;
  static constexpr uint16_t VramEnd = 0x4000;

  bool loadConfig(const std::filesystem::path &path, std::string &error);
  bool loadRomFile(const std::filesystem::path &path, uint16_t address,
                   std::string &error);
  void triggerInterrupt(uint16_t vector);
  void renderBitmap() const;
  void setInputBit(uint8_t port, uint8_t bit, bool pressed);

  SpaceInvadersCpu cpu_;
  std::array<uint8_t, 0x10000> initialMemory_;
  mutable std::array<uint8_t, Width * Height * 4> pixels_;
  uint8_t port1_;
  uint8_t port2_;
  uint16_t shiftRegister_;
  uint8_t shiftOffset_;
};

} // namespace systems::arcade::invaders
