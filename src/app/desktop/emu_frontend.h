#pragma once

#include "core/system_catalog.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace desktop {

using SystemCategory = core::MachineFamily;
using SystemInfo = core::SystemDefinition;

const char *CategoryName(SystemCategory category);

class EmuFrontend {
public:
  EmuFrontend();

  const std::vector<SystemInfo> &systems() const;
  const SystemInfo &selectedSystem() const;
  void selectSystem(const std::string &id);

  bool loadRom(const std::string &path);
  void reset();
  void runFrame();
  void setPaused(bool paused);
  void keyDown(int key);
  void keyUp(int key);

  bool paused() const;
  const std::string &loadedRom() const;
  const std::string &status() const;
  bool hasMachine() const;
  core::VideoFrame videoFrame() const;

  std::filesystem::path mediaRootForSelectedSystem() const;
  std::vector<std::string> mediaExtensionsForSelectedSystem() const;
  bool isMediaCompatibleWithSelectedSystem(const std::filesystem::path &path) const;
  std::string recommendedMediaHint() const;
  std::vector<std::filesystem::path>
  discoverMedia(const std::filesystem::path &root) const;
  std::vector<std::filesystem::path> discoverMediaForSelectedSystem() const;

private:
  std::string systemForMedia(const std::filesystem::path &path) const;
  bool selectSystemByIndex(std::size_t index);

  std::vector<SystemInfo> systems_;
  std::size_t selectedSystemIndex_;
  std::unique_ptr<core::Machine> machine_;
  bool paused_;
  std::string loadedRom_;
  std::string status_;
};

} // namespace desktop
