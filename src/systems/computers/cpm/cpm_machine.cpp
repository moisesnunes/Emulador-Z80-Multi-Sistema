#include "systems/computers/cpm/cpm_machine.h"

#include "core/media/binary_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace systems::computers::cpm {
namespace {

const core::MachineInfo kInfo{core::MachineFamily::Computers, "cpm", "CP/M",
                              true};
constexpr uint32_t kZ80CyclesPerFrame = 4000000 / 60;

uint8_t glyph_row(char ch, int row) {
  if (ch >= 'a' && ch <= 'z')
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

  switch (ch) {
  case '0': { static const uint8_t r[7] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}; return r[row]; }
  case '1': { static const uint8_t r[7] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}; return r[row]; }
  case '2': { static const uint8_t r[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}; return r[row]; }
  case '3': { static const uint8_t r[7] = {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}; return r[row]; }
  case '4': { static const uint8_t r[7] = {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}; return r[row]; }
  case '5': { static const uint8_t r[7] = {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}; return r[row]; }
  case '6': { static const uint8_t r[7] = {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}; return r[row]; }
  case '7': { static const uint8_t r[7] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}; return r[row]; }
  case '8': { static const uint8_t r[7] = {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}; return r[row]; }
  case '9': { static const uint8_t r[7] = {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}; return r[row]; }
  case 'A': { static const uint8_t r[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}; return r[row]; }
  case 'B': { static const uint8_t r[7] = {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}; return r[row]; }
  case 'C': { static const uint8_t r[7] = {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}; return r[row]; }
  case 'D': { static const uint8_t r[7] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}; return r[row]; }
  case 'E': { static const uint8_t r[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}; return r[row]; }
  case 'F': { static const uint8_t r[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}; return r[row]; }
  case 'G': { static const uint8_t r[7] = {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}; return r[row]; }
  case 'H': { static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}; return r[row]; }
  case 'I': { static const uint8_t r[7] = {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}; return r[row]; }
  case 'J': { static const uint8_t r[7] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c}; return r[row]; }
  case 'K': { static const uint8_t r[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}; return r[row]; }
  case 'L': { static const uint8_t r[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}; return r[row]; }
  case 'M': { static const uint8_t r[7] = {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}; return r[row]; }
  case 'N': { static const uint8_t r[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}; return r[row]; }
  case 'O': { static const uint8_t r[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}; return r[row]; }
  case 'P': { static const uint8_t r[7] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}; return r[row]; }
  case 'Q': { static const uint8_t r[7] = {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}; return r[row]; }
  case 'R': { static const uint8_t r[7] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}; return r[row]; }
  case 'S': { static const uint8_t r[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}; return r[row]; }
  case 'T': { static const uint8_t r[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}; return r[row]; }
  case 'U': { static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}; return r[row]; }
  case 'V': { static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}; return r[row]; }
  case 'W': { static const uint8_t r[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}; return r[row]; }
  case 'X': { static const uint8_t r[7] = {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}; return r[row]; }
  case 'Y': { static const uint8_t r[7] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}; return r[row]; }
  case 'Z': { static const uint8_t r[7] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}; return r[row]; }
  case '.': return row == 6 ? 0x04 : 0x00;
  case ',': return row >= 5 ? (row == 5 ? 0x04 : 0x08) : 0x00;
  case ':': return (row == 1 || row == 5) ? 0x04 : 0x00;
  case '-': return row == 3 ? 0x1f : 0x00;
  case '/': return 0x01 << (6 - row);
  case '!': return row < 5 ? 0x04 : (row == 6 ? 0x04 : 0x00);
  default:
    return 0x00;
  }
}

} // namespace

CpmMachine::CpmMachine() {
  cpu_.AttachBus(this);
  reset();
}

const core::MachineInfo &CpmMachine::info() const { return kInfo; }

bool CpmMachine::loadMedia(const std::filesystem::path &path,
                           std::string &error) {
  std::vector<uint8_t> data;
  if (!core::LoadBinaryFile(path, data, error))
    return false;

  if (data.size() > 0xff00) {
    error = "Programa CP/M maior que a area TPA suportada.";
    return false;
  }

  program_ = std::move(data);
  return true;
}

void CpmMachine::reset() {
  ram_.fill(0x00);
  cells_.fill(' ');
  inputQueue_.clear();
  cursorX_ = 0;
  cursorY_ = 0;

  ram_[0x0000] = 0x76; // HALT on warm boot/return to zero.
  ram_[0x0005] = 0xc9; // RET fallback; normal BDOS is intercepted.
  installTransientProgram();

  cpu_.ResetZ80();
  cpu_.AttachBus(this);
  cpu_.PC = 0x0100;
  cpu_.SP = 0xf000;
  renderTerminal();
}

void CpmMachine::installTransientProgram() {
  if (program_.empty())
    return;

  std::copy(program_.begin(), program_.end(), ram_.begin() + 0x0100);
}

void CpmMachine::runFrame() {
  uint32_t elapsed = 0;
  while (elapsed < kZ80CyclesPerFrame && !cpu_.halted) {
    if (cpu_.PC == 0x0000) {
      cpu_.halted = true;
      break;
    }

    if (cpu_.PC == 0x0005) {
      uint32_t before = cpu_.cycles;
      if (!handleBdos())
        break;
      cpu_.cycles += 17;
      cpu_.cyclesInterrupt += 17;
      elapsed += cpu_.cycles - before;
      continue;
    }

    elapsed += ExecuteZ80Next(&cpu_);
  }
}

core::VideoFrame CpmMachine::videoFrame() const {
  renderTerminal();
  return {pixels_.data(), Width, Height, Width * 4, core::PixelFormat::RGBA8888};
}

void CpmMachine::keyDown(int nativeKey) {
  if (nativeKey >= 0x20 && nativeKey <= 0x7e) {
    queueConsoleChar(static_cast<uint8_t>(nativeKey));
    return;
  }

  core::InputKey key = static_cast<core::InputKey>(nativeKey);
  if (key == core::InputKey::Start)
    queueConsoleChar('\r');
  else if (key == core::InputKey::Button1)
    queueConsoleChar(' ');
  else if (key == core::InputKey::Select)
    queueConsoleChar('\b');
}

void CpmMachine::keyUp(int) {}

uint8_t CpmMachine::read(uint16_t address) { return ram_[address]; }

void CpmMachine::write(uint16_t address, uint8_t value) { ram_[address] = value; }

uint8_t CpmMachine::in(uint16_t) { return 0xff; }

void CpmMachine::out(uint16_t, uint8_t) {}

bool CpmMachine::handleBdos() {
  uint16_t returnAddress = static_cast<uint16_t>(ram_[cpu_.SP]) |
                           (static_cast<uint16_t>(ram_[cpu_.SP + 1]) << 8);

  switch (cpu_.C) {
  case 0x00:
    finishBdosCall(returnAddress);
    cpu_.PC = returnAddress;
    cpu_.halted = true;
    return false;
  case 0x01: {
    if (inputQueue_.empty())
      return false;
    uint8_t ch = popConsoleChar();
    cpu_.A = ch;
    cpu_.L = ch;
    putChar(static_cast<char>(ch));
    break;
  }
  case 0x02:
    putChar(static_cast<char>(cpu_.E));
    break;
  case 0x03: {
    if (inputQueue_.empty())
      return false;
    uint8_t ch = popConsoleChar();
    cpu_.A = ch;
    cpu_.L = ch;
    break;
  }
  case 0x04:
  case 0x05:
    putChar(static_cast<char>(cpu_.E));
    break;
  case 0x06:
    if (cpu_.E == 0xff) {
      if (inputQueue_.empty()) {
        cpu_.A = 0x00;
        cpu_.L = 0x00;
      } else {
        uint8_t ch = popConsoleChar();
        cpu_.A = ch;
        cpu_.L = ch;
      }
    } else if (cpu_.E == 0xfe) {
      cpu_.A = inputQueue_.empty() ? 0x00 : 0xff;
      cpu_.L = cpu_.A;
    } else {
      putChar(static_cast<char>(cpu_.E));
    }
    break;
  case 0x09: {
    uint16_t address = static_cast<uint16_t>((cpu_.D << 8) | cpu_.E);
    for (int i = 0; i < 4096; i++) {
      char ch = static_cast<char>(ram_[address++]);
      if (ch == '$')
        break;
      putChar(ch);
    }
    break;
  }
  case 0x0a: {
    if (!hasCompleteLine())
      return false;

    uint16_t address = static_cast<uint16_t>((cpu_.D << 8) | cpu_.E);
    uint8_t maxLen = ram_[address];
    uint8_t count = 0;
    while (!inputQueue_.empty()) {
      uint8_t ch = popConsoleChar();
      putChar(static_cast<char>(ch));
      if (ch == '\r' || ch == '\n')
        break;
      if (count < maxLen)
        ram_[static_cast<uint16_t>(address + 2 + count++)] = ch;
    }
    ram_[static_cast<uint16_t>(address + 1)] = count;
    break;
  }
  case 0x0b:
    cpu_.A = inputQueue_.empty() ? 0x00 : 0xff;
    cpu_.L = cpu_.A;
    break;
  case 0x0c:
    cpu_.H = 0x00;
    cpu_.L = 0x22;
    break;
  default:
    break;
  }

  finishBdosCall(returnAddress);
  return true;
}

bool CpmMachine::hasCompleteLine() const {
  for (uint8_t ch : inputQueue_) {
    if (ch == '\r' || ch == '\n')
      return true;
  }
  return false;
}

uint8_t CpmMachine::popConsoleChar() {
  uint8_t ch = inputQueue_.front();
  inputQueue_.pop_front();
  return ch;
}

void CpmMachine::queueConsoleChar(uint8_t ch) {
  if (ch == '\n')
    ch = '\r';
  inputQueue_.push_back(ch);
}

void CpmMachine::finishBdosCall(uint16_t returnAddress) {
  cpu_.SP = static_cast<uint16_t>(cpu_.SP + 2);
  cpu_.PC = returnAddress;
}

void CpmMachine::putChar(char ch) {
  if (ch == '\r') {
    cursorX_ = 0;
    return;
  }

  if (ch == '\n') {
    newLine();
    return;
  }

  if (ch == '\b') {
    if (cursorX_ > 0)
      cursorX_--;
    cells_[cursorY_ * Columns + cursorX_] = ' ';
    return;
  }

  if (static_cast<unsigned char>(ch) < 0x20)
    return;

  cells_[cursorY_ * Columns + cursorX_] = ch;
  cursorX_++;
  if (cursorX_ >= Columns)
    newLine();
}

void CpmMachine::newLine() {
  cursorX_ = 0;
  cursorY_++;
  if (cursorY_ < Rows)
    return;

  for (int row = 1; row < Rows; row++) {
    std::copy_n(cells_.begin() + row * Columns, Columns,
                cells_.begin() + (row - 1) * Columns);
  }
  std::fill_n(cells_.begin() + (Rows - 1) * Columns, Columns, ' ');
  cursorY_ = Rows - 1;
}

void CpmMachine::renderTerminal() const {
  pixels_.fill(0);

  for (int row = 0; row < Rows; row++) {
    for (int col = 0; col < Columns; col++) {
      char ch = cells_[row * Columns + col];
      for (int y = 0; y < CellHeight; y++) {
        int glyphY = (y - 1) / 2;
        uint8_t bits = (glyphY >= 0 && glyphY < 7) ? glyph_row(ch, glyphY) : 0;
        for (int x = 0; x < CellWidth; x++) {
          bool on = x >= 1 && x <= 5 && (bits & (0x10 >> (x - 1)));
          std::size_t pixel =
              static_cast<std::size_t>((row * CellHeight + y) * Width +
                                       (col * CellWidth + x)) *
              4;
          pixels_[pixel + 0] = on ? 80 : 4;
          pixels_[pixel + 1] = on ? 255 : 16;
          pixels_[pixel + 2] = on ? 120 : 8;
          pixels_[pixel + 3] = 255;
        }
      }
    }
  }
}

std::string CpmMachine::terminalText() const {
  std::string text;
  for (int row = 0; row < Rows; row++) {
    int end = Columns;
    while (end > 0 && cells_[row * Columns + end - 1] == ' ')
      end--;
    text.append(cells_.data() + row * Columns, end);
    if (row + 1 < Rows)
      text.push_back('\n');
  }
  return text;
}

} // namespace systems::computers::cpm
