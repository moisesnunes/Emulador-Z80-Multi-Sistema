#include "systems/computers/msx/msx_ares_machine.h"

#include "core/media/binary_loader.h"

#include <algorithm>
#include <cstring>

namespace systems::computers::msx {
namespace {

const core::MachineInfo kInfo{core::MachineFamily::Computers, "msx",
                              "MSX (slots estilo ares)", true};
constexpr uint32_t kZ80CyclesPerFrame = 3579545 / 60;

template <typename Array>
void load_to_bank(Array &bank, uint16_t address, const std::vector<uint8_t> &data) {
  std::size_t count = std::min<std::size_t>(data.size(), bank.size() - address);
  std::copy_n(data.begin(), count, bank.begin() + address);
}

uint8_t joystick_bit_for_key(core::InputKey key) {
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

AresStyleMsxMachine::AresStyleMsxMachine() {
  bios_.fill(0xff);
  cartridge_.fill(0xff);
  expansion_.fill(0xff);
  cpu_.AttachBus(this);
  reset();
}

const core::MachineInfo &AresStyleMsxMachine::info() const { return kInfo; }

void AresStyleMsxMachine::reset() {
  ram_.fill(0x00);
  keyboardRows_.fill(0xff);
  psg_.fill(0x00);
  joystick_[0] = 0x3f;
  joystick_[1] = 0x3f;
  primarySlot_ = {0, 0, 3, 3};
  keyboardRow_ = 0;
  psgLatch_ = 0;
  psg_[15] = 0x8f;
  cpu_.ResetZ80();
  cpu_.AttachBus(this);
  vdp_.reset();
}

bool AresStyleMsxMachine::loadBios(const std::filesystem::path &path,
                                   std::string &error) {
  std::vector<uint8_t> data;
  if (!core::LoadBinaryFile(path, data, error))
    return false;

  bios_.fill(0xff);
  load_to_bank(bios_, 0x0000, data);
  return true;
}

bool AresStyleMsxMachine::loadMedia(const std::filesystem::path &path,
                                    std::string &error) {
  std::vector<uint8_t> data;
  if (!core::LoadBinaryFile(path, data, error))
    return false;

  return loadCartridge(data, error);
}

bool AresStyleMsxMachine::loadCartridge(const std::vector<uint8_t> &rom,
                                        std::string &error) {
  uint16_t address = detectCartridgeAddress(rom);
  if (rom.size() > cartridge_.size() - address) {
    error = "Cartucho MSX maior que o espaco linear suportado.";
    return false;
  }

  cartridge_.fill(0xff);
  load_to_bank(cartridge_, address, rom);
  return true;
}

uint16_t AresStyleMsxMachine::detectCartridgeAddress(
    const std::vector<uint8_t> &rom) const {
  if (rom.size() <= 0x4000 && rom.size() >= 4 && rom[0] == 'A' &&
      rom[1] == 'B') {
    uint16_t init = static_cast<uint16_t>(rom[2]) |
                    (static_cast<uint16_t>(rom[3]) << 8);
    if (init >= 0x8000 && init < 0xc000)
      return 0x8000;
  }
  return 0x4000;
}

void AresStyleMsxMachine::runFrame() {
  ExecuteZ80ForCycles(&cpu_, kZ80CyclesPerFrame);

  vdp_.setVBlank(true);
  vdp_.render();
  if (vdp_.interruptLine())
    cpu_.MaskableInterrupt();
}

core::VideoFrame AresStyleMsxMachine::videoFrame() const { return vdp_.frame(); }

void AresStyleMsxMachine::keyDown(int nativeKey) {
  uint8_t bit = joystick_bit_for_key(static_cast<core::InputKey>(nativeKey));
  if (bit)
    joystick_[0] &= static_cast<uint8_t>(~bit);
}

void AresStyleMsxMachine::keyUp(int nativeKey) {
  uint8_t bit = joystick_bit_for_key(static_cast<core::InputKey>(nativeKey));
  if (bit)
    joystick_[0] |= bit;
}

uint8_t AresStyleMsxMachine::read(uint16_t address) {
  uint8_t page = address >> 14;
  switch (primarySlot_[page]) {
  case 0:
    return bios_[address];
  case 1:
    return cartridge_[address];
  case 2:
    return expansion_[address];
  case 3:
    return ram_[address];
  }
  return 0xff;
}

void AresStyleMsxMachine::write(uint16_t address, uint8_t value) {
  uint8_t page = address >> 14;
  switch (primarySlot_[page]) {
  case 0:
  case 1:
  case 2:
    return;
  case 3:
    ram_[address] = value;
    return;
  }
}

uint8_t AresStyleMsxMachine::in(uint16_t port) {
  switch (port & 0xff) {
  case 0x98:
    return vdp_.readData();
  case 0x99:
    return vdp_.readStatus();
  case 0xa2:
    return readPsg();
  case 0xa8:
    return readPrimarySlot();
  case 0xa9:
    return readKeyboard();
  case 0xaa:
    return keyboardRow_;
  default:
    return 0xff;
  }
}

void AresStyleMsxMachine::out(uint16_t port, uint8_t value) {
  switch (port & 0xff) {
  case 0x98:
    vdp_.writeData(value);
    return;
  case 0x99:
    vdp_.writeControl(value);
    return;
  case 0xa0:
    psgLatch_ = value & 0x0f;
    return;
  case 0xa1:
    if ((psgLatch_ & 0x0f) != 14)
      psg_[psgLatch_ & 0x0f] = value;
    return;
  case 0xa8:
    writePrimarySlot(value);
    return;
  case 0xaa:
    keyboardRow_ = value & 0x0f;
    return;
  default:
    return;
  }
}

uint8_t AresStyleMsxMachine::readPrimarySlot() const {
  return static_cast<uint8_t>((primarySlot_[0] & 3) |
                              ((primarySlot_[1] & 3) << 2) |
                              ((primarySlot_[2] & 3) << 4) |
                              ((primarySlot_[3] & 3) << 6));
}

void AresStyleMsxMachine::writePrimarySlot(uint8_t value) {
  primarySlot_[0] = value & 0x03;
  primarySlot_[1] = (value >> 2) & 0x03;
  primarySlot_[2] = (value >> 4) & 0x03;
  primarySlot_[3] = (value >> 6) & 0x03;
}

uint8_t AresStyleMsxMachine::readKeyboard() const {
  return keyboardRow_ < keyboardRows_.size() ? keyboardRows_[keyboardRow_]
                                             : 0xff;
}

uint8_t AresStyleMsxMachine::readPsg() const {
  uint8_t reg = psgLatch_ & 0x0f;
  if (reg != 14)
    return psg_[reg];

  bool joystick1 = (psg_[15] & 0x40) == 0;
  uint8_t value = joystick1 ? joystick_[0] : joystick_[1];
  return static_cast<uint8_t>(0xc0 | (value & 0x3f));
}

} // namespace systems::computers::msx
