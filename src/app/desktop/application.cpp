#include "application.h"

#include "core/machine.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <glad/glad.h>

#include <cstdio>
#include <cstring>
#include <iostream>

namespace desktop
{
  namespace
  {

    void setup_imgui_keymap()
    {
      ImGuiIO &io = ImGui::GetIO();
      io.KeyMap[ImGuiKey_Tab] = SDL_SCANCODE_TAB;
      io.KeyMap[ImGuiKey_LeftArrow] = SDL_SCANCODE_LEFT;
      io.KeyMap[ImGuiKey_RightArrow] = SDL_SCANCODE_RIGHT;
      io.KeyMap[ImGuiKey_UpArrow] = SDL_SCANCODE_UP;
      io.KeyMap[ImGuiKey_DownArrow] = SDL_SCANCODE_DOWN;
      io.KeyMap[ImGuiKey_PageUp] = SDL_SCANCODE_PAGEUP;
      io.KeyMap[ImGuiKey_PageDown] = SDL_SCANCODE_PAGEDOWN;
      io.KeyMap[ImGuiKey_Home] = SDL_SCANCODE_HOME;
      io.KeyMap[ImGuiKey_End] = SDL_SCANCODE_END;
      io.KeyMap[ImGuiKey_Insert] = SDL_SCANCODE_INSERT;
      io.KeyMap[ImGuiKey_Delete] = SDL_SCANCODE_DELETE;
      io.KeyMap[ImGuiKey_Backspace] = SDL_SCANCODE_BACKSPACE;
      io.KeyMap[ImGuiKey_Space] = SDL_SCANCODE_SPACE;
      io.KeyMap[ImGuiKey_Enter] = SDL_SCANCODE_RETURN;
      io.KeyMap[ImGuiKey_Escape] = SDL_SCANCODE_ESCAPE;
      io.KeyMap[ImGuiKey_KeyPadEnter] = SDL_SCANCODE_KP_ENTER;
      io.KeyMap[ImGuiKey_A] = SDL_SCANCODE_A;
      io.KeyMap[ImGuiKey_C] = SDL_SCANCODE_C;
      io.KeyMap[ImGuiKey_V] = SDL_SCANCODE_V;
      io.KeyMap[ImGuiKey_X] = SDL_SCANCODE_X;
      io.KeyMap[ImGuiKey_Y] = SDL_SCANCODE_Y;
      io.KeyMap[ImGuiKey_Z] = SDL_SCANCODE_Z;
    }

    int mouse_button_index(Uint8 button)
    {
      if (button == SDL_BUTTON_LEFT)
        return 0;
      if (button == SDL_BUTTON_RIGHT)
        return 1;
      if (button == SDL_BUTTON_MIDDLE)
        return 2;
      return -1;
    }

    void update_key_modifiers(SDL_Keymod mod)
    {
      ImGuiIO &io = ImGui::GetIO();
      io.KeyCtrl = (mod & SDL_KMOD_CTRL) != 0;
      io.KeyShift = (mod & SDL_KMOD_SHIFT) != 0;
      io.KeyAlt = (mod & SDL_KMOD_ALT) != 0;
      io.KeySuper = (mod & SDL_KMOD_GUI) != 0;
    }

    const char *find_initial_rom(int argc, char **argv)
    {
      for (int i = 1; i < argc; i++)
      {
        if (argv[i] && argv[i][0] != '-')
          return argv[i];
      }
      return nullptr;
    }

    int map_input_key(SDL_Scancode scancode)
    {
      switch (scancode)
      {
      case SDL_SCANCODE_UP:
        return static_cast<int>(core::InputKey::Up);
      case SDL_SCANCODE_DOWN:
        return static_cast<int>(core::InputKey::Down);
      case SDL_SCANCODE_LEFT:
        return static_cast<int>(core::InputKey::Left);
      case SDL_SCANCODE_RIGHT:
        return static_cast<int>(core::InputKey::Right);
      case SDL_SCANCODE_Z:
      case SDL_SCANCODE_SPACE:
        return static_cast<int>(core::InputKey::Button1);
      case SDL_SCANCODE_X:
      case SDL_SCANCODE_LSHIFT:
        return static_cast<int>(core::InputKey::Button2);
      case SDL_SCANCODE_RETURN:
        return static_cast<int>(core::InputKey::Start);
      case SDL_SCANCODE_C:
      case SDL_SCANCODE_BACKSPACE:
        return static_cast<int>(core::InputKey::Select);
      default:
        return 0;
      }
    }

  } // namespace

  Application::Application()
      : window_(nullptr), glContext_(nullptr), running_(true),
        sdlStarted_(false), imguiContextReady_(false),
        imguiRendererReady_(false), textInputStarted_(false),
        lastFrameTicks_(0)
  {
    gui_init(gui_);
  }

  int Application::run(int argc, char **argv)
  {
    const char *initialRom = find_initial_rom(argc, argv);
    if (!init(initialRom))
    {
      shutdown();
      return 1;
    }

    mainLoop();
    shutdown();
    return 0;
  }

  bool Application::init(const char *initialRom)
  {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
      std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
      return false;
    }
    sdlStarted_ = true;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags flags =
        static_cast<SDL_WindowFlags>(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    window_ = SDL_CreateWindow("Emulador Z80/8080", 1100, 720, flags);
    if (!window_)
    {
      std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
      return false;
    }

    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_)
    {
      std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError()
                << std::endl;
      return false;
    }

    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)))
    {
      std::cerr << "Failed to initialize GLAD for SDL OpenGL." << std::endl;
      return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiContextReady_ = true;
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendPlatformName = "desktop_sdl3_minimal";
    setup_imgui_keymap();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplOpenGL3_Init("#version 330 core"))
    {
      std::cerr << "ImGui OpenGL backend init failed." << std::endl;
      return false;
    }
    imguiRendererReady_ = true;

    textInputStarted_ = SDL_StartTextInput(window_);
    lastFrameTicks_ = SDL_GetTicksNS();

    if (initialRom)
    {
      std::snprintf(gui_.romPath, sizeof(gui_.romPath), "%s", initialRom);
      emu_.loadRom(gui_.romPath);
    }

    return true;
  }

  void Application::mainLoop()
  {
    while (running_)
    {
      processEvents();
      emu_.runFrame();
      beginFrame();
      gui_render(gui_, emu_, running_);
      endFrame();
    }
  }

  void Application::processEvents()
  {
    ImGuiIO &io = ImGui::GetIO();
    io.MouseWheel = 0.0f;
    io.MouseWheelH = 0.0f;

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      switch (event.type)
      {
      case SDL_EVENT_QUIT:
        running_ = false;
        break;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        running_ = false;
        break;
      case SDL_EVENT_TEXT_INPUT:
        io.AddInputCharactersUTF8(event.text.text);
        if (!io.WantCaptureKeyboard)
        {
          const char *text = event.text.text;
          while (text && *text)
          {
            unsigned char ch = static_cast<unsigned char>(*text++);
            if (ch >= 0x20 && ch <= 0x7e)
              emu_.keyDown(static_cast<int>(ch));
          }
        }
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        io.MouseWheelH += event.wheel.x;
        io.MouseWheel += event.wheel.y;
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
      {
        int index = mouse_button_index(event.button.button);
        if (index >= 0)
          io.MouseDown[index] = event.button.down;
        break;
      }
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
      {
        SDL_Scancode scancode = event.key.scancode;
        if (scancode >= 0 && scancode < 512)
          io.KeysDown[scancode] = event.key.down;
        update_key_modifiers(event.key.mod);
        int inputKey = map_input_key(scancode);
        if (inputKey != 0 && (!io.WantCaptureKeyboard || !event.key.down))
        {
          if (event.key.down)
            emu_.keyDown(inputKey);
          else
            emu_.keyUp(inputKey);
        }
        break;
      }
      default:
        break;
      }
    }

    float mouseX = 0.0f;
    float mouseY = 0.0f;
    SDL_MouseButtonFlags mouseButtons = SDL_GetMouseState(&mouseX, &mouseY);
    io.MousePos = ImVec2(mouseX, mouseY);
    io.MouseDown[0] = (mouseButtons & SDL_BUTTON_LMASK) != 0;
    io.MouseDown[1] = (mouseButtons & SDL_BUTTON_RMASK) != 0;
    io.MouseDown[2] = (mouseButtons & SDL_BUTTON_MMASK) != 0;
    update_key_modifiers(SDL_GetModState());
  }

  void Application::beginFrame()
  {
    int windowW = 0;
    int windowH = 0;
    int drawableW = 0;
    int drawableH = 0;
    SDL_GetWindowSize(window_, &windowW, &windowH);
    SDL_GetWindowSizeInPixels(window_, &drawableW, &drawableH);

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(windowW),
                            static_cast<float>(windowH));
    if (windowW > 0 && windowH > 0)
    {
      io.DisplayFramebufferScale =
          ImVec2(static_cast<float>(drawableW) / static_cast<float>(windowW),
                 static_cast<float>(drawableH) / static_cast<float>(windowH));
    }

    Uint64 now = SDL_GetTicksNS();
    io.DeltaTime =
        lastFrameTicks_ == 0
            ? 1.0f / 60.0f
            : static_cast<float>(static_cast<double>(now - lastFrameTicks_) /
                                 1000000000.0);
    lastFrameTicks_ = now;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
  }

  void Application::endFrame()
  {
    ImGui::Render();

    int drawableW = 0;
    int drawableH = 0;
    SDL_GetWindowSizeInPixels(window_, &drawableW, &drawableH);
    glViewport(0, 0, drawableW, drawableH);
    glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);
  }

  void Application::shutdown()
  {
    gui_shutdown(gui_);
    if (textInputStarted_ && window_)
      SDL_StopTextInput(window_);
    if (imguiRendererReady_)
      ImGui_ImplOpenGL3_Shutdown();
    if (imguiContextReady_ && ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    if (glContext_)
      SDL_GL_DestroyContext(glContext_);
    if (window_)
      SDL_DestroyWindow(window_);
    if (sdlStarted_)
      SDL_Quit();

    glContext_ = nullptr;
    window_ = nullptr;
    sdlStarted_ = false;
    imguiContextReady_ = false;
    imguiRendererReady_ = false;
    textInputStarted_ = false;
  }

} // namespace desktop
