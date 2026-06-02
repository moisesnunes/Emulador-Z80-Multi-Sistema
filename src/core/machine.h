#pragma once

#include "core/video_frame.h"

#include <filesystem>
#include <string>

namespace core {

enum class MachineFamily {
  Computers,
  Consoles,
  Arcade,
};

enum class InputKey : int {
  Up = 1,
  Down,
  Left,
  Right,
  Button1,
  Button2,
  Start,
  Select,
};

struct MachineInfo {
  MachineFamily family;
  const char *id;
  const char *name;
  bool z80Based;
};

class Machine {
public:
  virtual ~Machine() = default;

  virtual const MachineInfo &info() const = 0;
  virtual bool loadMedia(const std::filesystem::path &path,
                         std::string &error) = 0;
  virtual void reset() = 0;
  virtual void runFrame() = 0;
  virtual VideoFrame videoFrame() const = 0;
  virtual void keyDown(int nativeKey) = 0;
  virtual void keyUp(int nativeKey) = 0;
};

} // namespace core
