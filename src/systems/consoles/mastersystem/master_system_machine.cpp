#include "systems/consoles/mastersystem/master_system_machine.h"

#include "core/media/binary_loader.h"

#include <algorithm>

namespace systems::consoles::mastersystem {
namespace {

const core::MachineInfo kInfo{core::MachineFamily::Consoles, "mastersystem",
                              "Master System", true};
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

MasterSystemMachine::MasterSystemMachine() {
  cpu_.AttachBus(this);
  reset();
}

const core::MachineInfo &MasterSystemMachine::info() const { return kInfo; }

void MasterSystemMachine::reset() {
  ram_.fill(0x00);
  cartridgeRam_.fill(0xff);
  mapperBanks_ = {0, 1, 2};
  controller_[0] = 0x3f;
  controller_[1] = 0x3f;
  cartridgeRamEnabled_ = false;
  cartridgeRamBank_ = 0;
  memoryControl_ = 0x00;
  ioControl_ = 0xff;
  psgLatch_ = 0;
  cpu_.ResetZ80();
  cpu_.AttachBus(this);
  vdpCompat_.reset();
}

bool MasterSystemMachine::loadMedia(const std::filesystem::path &path,
                                    std::string &error) {
  if (!core::LoadBinaryFile(path, rom_, error))
    return false;

  if (rom_.size() % 0x4000 != 0) {
    std::size_t rounded = ((rom_.size() + 0x3fff) / 0x4000) * 0x4000;
    rom_.resize(rounded, 0xff);
  }
  return true;
}

void MasterSystemMachine::runFrame() {
  ExecuteZ80ForCycles(&cpu_, kZ80CyclesPerFrame);

  vdpCompat_.setVBlank(true);
  vdpCompat_.render();
  if (vdpCompat_.interruptLine())
    cpu_.MaskableInterrupt();
}

core::VideoFrame MasterSystemMachine::videoFrame() const {
  return vdpCompat_.frame();
}

void MasterSystemMachine::keyDown(int nativeKey) {
  uint8_t bit = controller_bit_for_key(static_cast<core::InputKey>(nativeKey));
  if (bit)
    controller_[0] &= static_cast<uint8_t>(~bit);
}

void MasterSystemMachine::keyUp(int nativeKey) {
  uint8_t bit = controller_bit_for_key(static_cast<core::InputKey>(nativeKey));
  if (bit)
    controller_[0] |= bit;
}

uint8_t MasterSystemMachine::read(uint16_t address) {
  if (address < 0x0400)
    return readRomBank(0, address);

  if (address < 0x4000)
    return readRomBank(mapperBanks_[0], address);

  if (address < 0x8000)
    return readRomBank(mapperBanks_[1], address - 0x4000);

  if (address < 0xc000) {
    if (cartridgeRamEnabled_) {
      uint16_t base = cartridgeRamBank_ ? 0x4000 : 0x0000;
      return cartridgeRam_[base + (address - 0x8000)];
    }
    return readRomBank(mapperBanks_[2], address - 0x8000);
  }

  return ram_[address & 0x1fff];
}

void MasterSystemMachine::write(uint16_t address, uint8_t value) {
  if (address >= 0x8000 && address < 0xc000 && cartridgeRamEnabled_) {
    uint16_t base = cartridgeRamBank_ ? 0x4000 : 0x0000;
    cartridgeRam_[base + (address - 0x8000)] = value;
    return;
  }

  if (address >= 0xc000) {
    ram_[address & 0x1fff] = value;
    if (address >= 0xfffc)
      writeMapper(address, value);
  }
}

uint8_t MasterSystemMachine::in(uint16_t port) {
  port &= 0xff;

  if ((port & 0xc1) == 0x40)
    return 0x00;
  if ((port & 0xc1) == 0x41)
    return 0x00;
  if ((port & 0xc1) == 0x80)
    return vdpCompat_.readData();
  if ((port & 0xc1) == 0x81)
    return vdpCompat_.readStatus();
  if ((port & 0xc1) == 0xc0)
    return readControllerA();
  if ((port & 0xc1) == 0xc1)
    return readControllerB();

  return 0xff;
}

void MasterSystemMachine::out(uint16_t port, uint8_t value) {
  port &= 0xff;

  if ((port & 0xc1) == 0x00) {
    memoryControl_ = value;
    return;
  }
  if ((port & 0xc1) == 0x01) {
    ioControl_ = value;
    return;
  }
  if ((port & 0xc0) == 0x40) {
    psgLatch_ = value;
    return;
  }
  if ((port & 0xc1) == 0x80) {
    vdpCompat_.writeData(value);
    return;
  }
  if ((port & 0xc1) == 0x81) {
    vdpCompat_.writeControl(value);
    return;
  }
}

uint8_t MasterSystemMachine::readRomBank(uint8_t bank, uint16_t offset) const {
  if (rom_.empty())
    return 0xff;

  std::size_t bankCount = std::max<std::size_t>(1, rom_.size() / 0x4000);
  std::size_t base = (bank % bankCount) * 0x4000;
  return rom_[(base + (offset & 0x3fff)) % rom_.size()];
}

void MasterSystemMachine::writeMapper(uint16_t address, uint8_t value) {
  switch (address) {
  case 0xfffc:
    cartridgeRamBank_ = (value & 0x04) ? 1 : 0;
    cartridgeRamEnabled_ = (value & 0x08) != 0;
    break;
  case 0xfffd:
    mapperBanks_[0] = value;
    break;
  case 0xfffe:
    mapperBanks_[1] = value;
    break;
  case 0xffff:
    mapperBanks_[2] = value;
    break;
  }
}

uint8_t MasterSystemMachine::readControllerA() const {
  return static_cast<uint8_t>((controller_[0] & 0x3f) |
                              ((controller_[1] & 0x03) << 6));
}

uint8_t MasterSystemMachine::readControllerB() const {
  return static_cast<uint8_t>(0x30 | ((controller_[1] >> 2) & 0x0f) | 0xc0);
}

} // namespace systems::consoles::mastersystem
