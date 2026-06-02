#include "systems/consoles/sg1000/sg1000_machine.h"

#include "core/media/binary_loader.h"

#include <algorithm>

namespace systems::consoles::sg1000 {
namespace {

const core::MachineInfo kInfo{core::MachineFamily::Consoles, "sg1000",
                              "SG-1000", true};
constexpr uint32_t kZ80CyclesPerFrame = 3579545 / 60;

uint8_t controller_bit_for_key(core::InputKey key) {
  switch (key) {
  case core::InputKey::Up:
    return 0x01;
  case core::InputKey::Down:
    return 0x02;
  case core::InputKey::Left:
    return 0x04;
  case core::InputKey::Right:
    return 0x08;
  case core::InputKey::Button1:
    return 0x10;
  case core::InputKey::Button2:
  case core::InputKey::Start:
    return 0x20;
  default:
    return 0;
  }
}

} // namespace

SG1000Machine::SG1000Machine() {
  cpu_.AttachBus(this);
  reset();
}

const core::MachineInfo &SG1000Machine::info() const { return kInfo; }

void SG1000Machine::reset() {
  expansion_.fill(0xff);
  ram_.fill(0x00);
  controller_[0] = 0x3f;
  controller_[1] = 0x3f;
  psgLatch_ = 0;
  cpu_.ResetZ80();
  cpu_.AttachBus(this);
  vdp_.reset();
}

bool SG1000Machine::loadMedia(const std::filesystem::path &path,
                              std::string &error) {
  return core::LoadBinaryFile(path, rom_, error);
}

void SG1000Machine::runFrame() {
  ExecuteZ80ForCycles(&cpu_, kZ80CyclesPerFrame);

  vdp_.setVBlank(true);
  vdp_.render();
  if (vdp_.interruptLine())
    cpu_.MaskableInterrupt();
}

core::VideoFrame SG1000Machine::videoFrame() const { return vdp_.frame(); }

void SG1000Machine::keyDown(int nativeKey) {
  uint8_t bit = controller_bit_for_key(static_cast<core::InputKey>(nativeKey));
  if (bit)
    controller_[0] &= static_cast<uint8_t>(~bit);
}

void SG1000Machine::keyUp(int nativeKey) {
  uint8_t bit = controller_bit_for_key(static_cast<core::InputKey>(nativeKey));
  if (bit)
    controller_[0] |= bit;
}

uint8_t SG1000Machine::read(uint16_t address) {
  if (address < 0x8000) {
    if (rom_.empty())
      return 0xff;
    return rom_[address % std::min<std::size_t>(rom_.size(), 0x8000)];
  }

  if (address < 0xc000) {
    std::size_t offset = address - 0x8000;
    if (rom_.size() > 0x8000 && offset < rom_.size() - 0x8000)
      return rom_[0x8000 + offset];
    return expansion_[offset];
  }

  return ram_[address & 0x03ff];
}

void SG1000Machine::write(uint16_t address, uint8_t value) {
  if (address >= 0xc000) {
    ram_[address & 0x03ff] = value;
    return;
  }

  if (address >= 0x8000 && address < 0xc000)
    expansion_[address - 0x8000] = value;
}

uint8_t SG1000Machine::in(uint16_t port) {
  switch ((port >> 6) & 0x03) {
  case 2:
    return (port & 1) ? vdp_.readStatus() : vdp_.readData();
  case 3:
    return (port & 1) ? readControllerB() : readControllerA();
  default:
    return 0xff;
  }
}

void SG1000Machine::out(uint16_t port, uint8_t value) {
  switch ((port >> 6) & 0x03) {
  case 1:
    psgLatch_ = value;
    return;
  case 2:
    (port & 1) ? vdp_.writeControl(value) : vdp_.writeData(value);
    return;
  default:
    return;
  }
}

uint8_t SG1000Machine::readControllerA() const {
  return static_cast<uint8_t>((controller_[0] & 0x3f) |
                              ((controller_[1] & 0x03) << 6));
}

uint8_t SG1000Machine::readControllerB() const {
  return static_cast<uint8_t>(0xf0 | ((controller_[1] >> 2) & 0x0f));
}

} // namespace systems::consoles::sg1000
