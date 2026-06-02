#include "systems/arcade/invaders/invaders_machine.h"

#include "core/media/binary_loader.h"
#include "alu.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace systems::arcade::invaders {
namespace {

const core::MachineInfo kInfo{core::MachineFamily::Arcade, "invaders",
                              "Space Invaders", false};
constexpr uint32_t kCpuCyclesPerFrame = 1996800 / 60;
constexpr uint8_t kCoinBit = 0;
constexpr uint8_t kPlayer2StartBit = 1;
constexpr uint8_t kPlayer1StartBit = 2;
constexpr uint8_t kPlayer1ShootBit = 4;
constexpr uint8_t kPlayer1LeftBit = 5;
constexpr uint8_t kPlayer1RightBit = 6;

std::string trim(std::string value) {
  auto isSpace = [](unsigned char ch) { return std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !isSpace(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !isSpace(ch); })
                  .base(),
              value.end());
  return value;
}

bool parse_address(const std::string &text, uint16_t &address) {
  try {
    std::size_t consumed = 0;
    unsigned long value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() || value > 0xffff)
      return false;
    address = static_cast<uint16_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace

SpaceInvadersCpu::SpaceInvadersCpu(SpaceInvadersMachine *machine)
    : machine_(machine) {}

uint8_t SpaceInvadersCpu::InPort8(uint16_t port) {
  return machine_->in(static_cast<uint8_t>(port));
}

void SpaceInvadersCpu::OutPort8(uint16_t port, uint8_t value) {
  machine_->out(static_cast<uint8_t>(port), value);
}

SpaceInvadersMachine::SpaceInvadersMachine() : cpu_(this) {
  initialMemory_.fill(0x00);
  reset();
}

const core::MachineInfo &SpaceInvadersMachine::info() const { return kInfo; }

bool SpaceInvadersMachine::loadMedia(const std::filesystem::path &path,
                                     std::string &error) {
  initialMemory_.fill(0x00);
  if (path.extension() == ".cfg")
    return loadConfig(path, error);

  return loadRomFile(path, 0x0000, error);
}

bool SpaceInvadersMachine::loadConfig(const std::filesystem::path &path,
                                      std::string &error) {
  std::ifstream file(path);
  if (!file) {
    error = "Nao foi possivel abrir config arcade: " + path.string();
    return false;
  }

  std::filesystem::path root = path.parent_path();
  std::string line;
  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line.find("title") == 0)
      continue;

    std::size_t at = line.find('@');
    if (at == std::string::npos)
      continue;

    uint16_t address = 0;
    if (!parse_address(trim(line.substr(at + 1)), address)) {
      error = "Endereco invalido em config arcade: " + line;
      return false;
    }

    if (!loadRomFile(root / trim(line.substr(0, at)), address, error))
      return false;
  }

  return true;
}

bool SpaceInvadersMachine::loadRomFile(const std::filesystem::path &path,
                                       uint16_t address, std::string &error) {
  std::vector<uint8_t> data;
  if (!core::LoadBinaryFile(path, data, error))
    return false;

  if (data.size() > initialMemory_.size() - address) {
    error = "ROM arcade maior que o espaco de memoria.";
    return false;
  }

  std::copy(data.begin(), data.end(), initialMemory_.begin() + address);
  return true;
}

void SpaceInvadersMachine::reset() {
  std::copy(initialMemory_.begin(), initialMemory_.end(), cpu_.memory);
  std::fill(std::begin(cpu_.IOPorts), std::end(cpu_.IOPorts), 0);
  cpu_.PC = 0;
  cpu_.SP = 0;
  cpu_.A = cpu_.B = cpu_.C = cpu_.D = cpu_.E = cpu_.H = cpu_.L = 0;
  cpu_.sf = cpu_.zf = cpu_.acf = cpu_.pf = cpu_.cf = false;
  cpu_.halted = false;
  cpu_.interrupts = false;
  cpu_.cycles = 0;
  cpu_.cyclesInterrupt = 0;
  cpu_.arcadeMode = false;
  std::fill(std::begin(cpu_.memWritable), std::end(cpu_.memWritable), 0xff);
  cpu_.SetRomRegion(0x0000, RomEnd);

  port1_ = 0x00;
  port2_ = 0x00;
  shiftRegister_ = 0;
  shiftOffset_ = 0;
  pixels_.fill(0);
}

void SpaceInvadersMachine::runFrame() {
  uint32_t target = cpu_.cycles + kCpuCyclesPerFrame;
  bool midInterrupt = false;
  while (!cpu_.halted && cpu_.cycles < target) {
    uint32_t elapsed = cpu_.cycles - (target - kCpuCyclesPerFrame);
    if (!midInterrupt && elapsed >= kCpuCyclesPerFrame / 2) {
      triggerInterrupt(0x0008);
      midInterrupt = true;
    }

    uint8_t opcode = cpu_.ReadMem8(cpu_.PC);
    ExecuteOpCode(opcode, &cpu_);
  }

  triggerInterrupt(0x0010);
}

void SpaceInvadersMachine::triggerInterrupt(uint16_t vector) {
  if (!cpu_.interrupts)
    return;

  cpu_.halted = false;
  PushRegisterPair(&cpu_, (cpu_.PC >> 8) & 0xff, cpu_.PC & 0xff);
  cpu_.PC = vector;
  cpu_.interrupts = false;
}

core::VideoFrame SpaceInvadersMachine::videoFrame() const {
  renderBitmap();
  return {pixels_.data(), Width, Height, Width * 4, core::PixelFormat::RGBA8888};
}

void SpaceInvadersMachine::keyDown(int nativeKey) {
  core::InputKey key = static_cast<core::InputKey>(nativeKey);
  switch (key) {
  case core::InputKey::Left:
    setInputBit(1, kPlayer1LeftBit, true);
    break;
  case core::InputKey::Right:
    setInputBit(1, kPlayer1RightBit, true);
    break;
  case core::InputKey::Button1:
    setInputBit(1, kPlayer1ShootBit, true);
    break;
  case core::InputKey::Start:
    setInputBit(1, kPlayer1StartBit, true);
    break;
  case core::InputKey::Select:
    setInputBit(1, kCoinBit, true);
    break;
  case core::InputKey::Button2:
    setInputBit(2, kPlayer2StartBit, true);
    break;
  default:
    break;
  }
}

void SpaceInvadersMachine::keyUp(int nativeKey) {
  core::InputKey key = static_cast<core::InputKey>(nativeKey);
  switch (key) {
  case core::InputKey::Left:
    setInputBit(1, kPlayer1LeftBit, false);
    break;
  case core::InputKey::Right:
    setInputBit(1, kPlayer1RightBit, false);
    break;
  case core::InputKey::Button1:
    setInputBit(1, kPlayer1ShootBit, false);
    break;
  case core::InputKey::Start:
    setInputBit(1, kPlayer1StartBit, false);
    break;
  case core::InputKey::Select:
    setInputBit(1, kCoinBit, false);
    break;
  case core::InputKey::Button2:
    setInputBit(2, kPlayer2StartBit, false);
    break;
  default:
    break;
  }
}

void SpaceInvadersMachine::setInputBit(uint8_t port, uint8_t bit,
                                       bool pressed) {
  uint8_t &value = port == 1 ? port1_ : port2_;
  if (pressed)
    value |= static_cast<uint8_t>(1 << bit);
  else
    value &= static_cast<uint8_t>(~(1 << bit));
}

uint8_t SpaceInvadersMachine::read(uint16_t address) const {
  return cpu_.memory[address];
}

uint8_t SpaceInvadersMachine::in(uint8_t port) const {
  switch (port) {
  case 1:
    return port1_;
  case 2:
    return port2_;
  case 3:
    return static_cast<uint8_t>((shiftRegister_ >> (8 - shiftOffset_)) & 0xff);
  default:
    return 0x00;
  }
}

void SpaceInvadersMachine::out(uint8_t port, uint8_t value) {
  switch (port) {
  case 2:
    shiftOffset_ = value & 0x07;
    break;
  case 4:
    shiftRegister_ =
        static_cast<uint16_t>((static_cast<uint16_t>(value) << 8) |
                              (shiftRegister_ >> 8));
    break;
  default:
    break;
  }
}

void SpaceInvadersMachine::renderBitmap() const {
  pixels_.fill(0);
  for (uint16_t address = VramStart; address < VramEnd; address++) {
    uint8_t byte = cpu_.memory[address];
    int offset = static_cast<int>(address - VramStart) * 8;
    for (int bit = 0; bit < 8; bit++) {
      if (((byte >> bit) & 0x01) == 0)
        continue;

      int rawX = (offset + bit) % 256;
      int rawY = (offset + bit) / 256;
      int x = rawY;
      int y = 255 - rawX;
      std::size_t pixel = static_cast<std::size_t>(y * Width + x) * 4;
      pixels_[pixel + 0] = 0x00;
      pixels_[pixel + 1] = 0xc0;
      pixels_[pixel + 2] = 0xc0;
      pixels_[pixel + 3] = 0xff;
    }
  }
}

} // namespace systems::arcade::invaders
