#pragma once

#include "emu_frontend.h"
#include "gui.h"

#include <SDL3/SDL.h>

namespace desktop {

class Application {
public:
  Application();
  int run(int argc, char **argv);

private:
  bool init(const char *initialRom);
  void mainLoop();
  void processEvents();
  void beginFrame();
  void endFrame();
  void shutdown();

  SDL_Window *window_;
  SDL_GLContext glContext_;
  bool running_;
  bool sdlStarted_;
  bool imguiContextReady_;
  bool imguiRendererReady_;
  bool textInputStarted_;
  Uint64 lastFrameTicks_;
  EmuFrontend emu_;
  GuiState gui_;
};

} // namespace desktop
