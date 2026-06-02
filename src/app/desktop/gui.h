#pragma once

#include "emu_frontend.h"

#include <filesystem>
#include <string>
#include <vector>

namespace desktop {

struct GuiState {
  char romPath[1024];
  std::vector<std::filesystem::path> mediaFiles;
  bool mediaScanned;
  int selectedMediaIndex;
  std::string scannedSystemId;
  int videoScale;
  bool integerScale;
  bool keepAspect;
  unsigned int videoTexture;
  int videoTextureWidth;
  int videoTextureHeight;
};

void gui_init(GuiState &state);
void gui_render(GuiState &state, EmuFrontend &emu, bool &running);
void gui_shutdown(GuiState &state);

} // namespace desktop
