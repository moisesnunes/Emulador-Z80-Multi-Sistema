#pragma once

#include <cstdint>

namespace core {

class Z80Bus {
public:
  virtual ~Z80Bus() = default;

  virtual uint8_t read(uint16_t address) = 0;
  virtual void write(uint16_t address, uint8_t value) = 0;
  virtual uint8_t in(uint16_t port) = 0;
  virtual void out(uint16_t port, uint8_t value) = 0;
};

} // namespace core
