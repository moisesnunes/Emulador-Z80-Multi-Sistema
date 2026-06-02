#include "gui.h"

#include "glad/glad.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>

namespace desktop {
namespace {

void copy_path(char *buffer, std::size_t bufferSize,
               const std::filesystem::path &path) {
  std::snprintf(buffer, bufferSize, "%s", path.string().c_str());
}

std::string relative_label(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::path rel =
      std::filesystem::relative(path, std::filesystem::current_path(ec), ec);
  return ec ? path.string() : rel.string();
}

void refresh_media(GuiState &state, const EmuFrontend &emu) {
  state.mediaFiles = emu.discoverMediaForSelectedSystem();
  state.mediaScanned = true;
  state.scannedSystemId = emu.selectedSystem().id;
  state.selectedMediaIndex = state.mediaFiles.empty() ? -1 : 0;
}

void ensure_media_current(GuiState &state, const EmuFrontend &emu) {
  if (!state.mediaScanned || state.scannedSystemId != emu.selectedSystem().id)
    refresh_media(state, emu);
}

std::string join_extensions(const std::vector<std::string> &extensions) {
  std::string text;
  for (std::size_t i = 0; i < extensions.size(); i++) {
    if (i)
      text += ", ";
    text += extensions[i];
  }
  return text;
}

void select_system(GuiState &state, EmuFrontend &emu, const char *id) {
  emu.selectSystem(id);
  refresh_media(state, emu);
  state.romPath[0] = '\0';
}

bool load_selected_media(GuiState &state, EmuFrontend &emu) {
  if (state.selectedMediaIndex < 0 ||
      state.selectedMediaIndex >= static_cast<int>(state.mediaFiles.size()))
    return false;

  copy_path(state.romPath, sizeof(state.romPath),
            state.mediaFiles[state.selectedMediaIndex]);
  return emu.loadRom(state.romPath);
}

void ensure_video_texture(GuiState &state) {
  if (state.videoTexture != 0)
    return;

  glGenTextures(1, &state.videoTexture);
  glBindTexture(GL_TEXTURE_2D, state.videoTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool update_video_texture(GuiState &state, const core::VideoFrame &frame) {
  if (!frame.valid())
    return false;

  ensure_video_texture(state);

  int bytesPerPixel = frame.format == core::PixelFormat::RGB888 ? 3 : 4;
  GLenum format = frame.format == core::PixelFormat::RGB888 ? GL_RGB : GL_RGBA;
  glBindTexture(GL_TEXTURE_2D, state.videoTexture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride / bytesPerPixel);

  if (state.videoTextureWidth != frame.width ||
      state.videoTextureHeight != frame.height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0,
                 format, GL_UNSIGNED_BYTE, frame.pixels);
    state.videoTextureWidth = frame.width;
    state.videoTextureHeight = frame.height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, format,
                    GL_UNSIGNED_BYTE, frame.pixels);
  }

  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  return true;
}

ImVec2 video_size_for_viewport(const GuiState &state,
                               const core::VideoFrame &frame,
                               const ImVec2 &available) {
  if (!state.keepAspect)
    return available;

  float width = static_cast<float>(frame.width * state.videoScale);
  float height = static_cast<float>(frame.height * state.videoScale);
  float fitScale = std::min(available.x / static_cast<float>(frame.width),
                            available.y / static_cast<float>(frame.height));

  if (width > available.x || height > available.y) {
    if (state.integerScale)
      fitScale = std::max(1.0f, static_cast<float>(static_cast<int>(fitScale)));
    width = static_cast<float>(frame.width) * fitScale;
    height = static_cast<float>(frame.height) * fitScale;
  }

  return ImVec2(std::max(1.0f, width), std::max(1.0f, height));
}

void render_system_picker(GuiState &state, EmuFrontend &emu,
                          SystemCategory category) {
  ImGui::TextUnformatted(CategoryName(category));
  for (const SystemInfo &system : emu.systems()) {
    if (system.family != category)
      continue;

    bool selected = std::strcmp(system.id, emu.selectedSystem().id) == 0;
    if (ImGui::RadioButton(system.name, selected))
      select_system(state, emu, system.id);
  }
}

void render_system_combo(GuiState &state, EmuFrontend &emu) {
  const SystemInfo &selected = emu.selectedSystem();
  if (!ImGui::BeginCombo("Sistema", selected.name))
    return;

  for (const SystemInfo &system : emu.systems()) {
    bool isSelected = std::strcmp(system.id, selected.id) == 0;
    std::string label = std::string(CategoryName(system.family)) + " / " +
                        system.name;
    if (ImGui::Selectable(label.c_str(), isSelected))
      select_system(state, emu, system.id);
    if (isSelected)
      ImGui::SetItemDefaultFocus();
  }

  ImGui::EndCombo();
}

void render_toolbar(GuiState &state, EmuFrontend &emu) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

  render_system_combo(state, emu);
  ImGui::SameLine();

  if (ImGui::Button(emu.paused() ? "Rodar" : "Pausar"))
    emu.setPaused(!emu.paused());
  ImGui::SameLine();
  if (ImGui::Button("Reset"))
    emu.reset();
  ImGui::SameLine();
  if (ImGui::Button("Carregar selecionada"))
    load_selected_media(state, emu);
  ImGui::SameLine();
  if (ImGui::Button("Atualizar midias"))
    refresh_media(state, emu);

  ImGui::PopStyleVar();

  ImGui::Text("Status: %s", emu.status().c_str());
}

void render_main_menu(GuiState &state, EmuFrontend &emu, bool &running) {
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("Arquivo")) {
    if (ImGui::MenuItem("Carregar caminho"))
      emu.loadRom(state.romPath);
    if (ImGui::MenuItem("Atualizar lista"))
      refresh_media(state, emu);
    ImGui::Separator();
    if (ImGui::MenuItem("Sair"))
      running = false;
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Emulador")) {
    if (ImGui::MenuItem("Reset"))
      emu.reset();
    bool paused = emu.paused();
    if (ImGui::MenuItem("Pausado", nullptr, paused))
      emu.setPaused(!paused);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Sistema")) {
    if (ImGui::BeginMenu("Computadores")) {
      for (const SystemInfo &system : emu.systems()) {
        if (system.family == SystemCategory::Computers &&
            ImGui::MenuItem(system.name))
          select_system(state, emu, system.id);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Consoles")) {
      for (const SystemInfo &system : emu.systems()) {
        if (system.family == SystemCategory::Consoles &&
            ImGui::MenuItem(system.name))
          select_system(state, emu, system.id);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Arcade")) {
      for (const SystemInfo &system : emu.systems()) {
        if (system.family == SystemCategory::Arcade &&
            ImGui::MenuItem(system.name))
          select_system(state, emu, system.id);
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}

void render_rom_tab(GuiState &state, EmuFrontend &emu) {
  ensure_media_current(state, emu);

  ImGui::Text("Maquina: %s", emu.selectedSystem().name);
  ImGui::Text("Pasta: %s", emu.mediaRootForSelectedSystem().string().c_str());
  std::string extensions = join_extensions(emu.mediaExtensionsForSelectedSystem());
  ImGui::Text("Extensoes: %s", extensions.c_str());
  ImGui::TextWrapped("%s", emu.recommendedMediaHint().c_str());

  ImGui::Separator();

  float listWidth = std::max(260.0f, ImGui::GetContentRegionAvail().x * 0.44f);
  ImGui::BeginChild("media_list_panel", ImVec2(listWidth, 340), true);
  ImGui::Text("Midias compativeis: %d",
              static_cast<int>(state.mediaFiles.size()));
  ImGui::Separator();
  for (int i = 0; i < static_cast<int>(state.mediaFiles.size()); i++) {
    const std::filesystem::path &media = state.mediaFiles[i];
    std::string label = relative_label(media);
    bool selected = state.selectedMediaIndex == i;
    if (ImGui::Selectable(label.c_str(), selected)) {
      state.selectedMediaIndex = i;
      copy_path(state.romPath, sizeof(state.romPath), media);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
      state.selectedMediaIndex = i;
      load_selected_media(state, emu);
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("media_actions_panel", ImVec2(0, 340), true);
  ImGui::TextUnformatted("Arquivo");
  ImGui::InputText("##caminho_midia", state.romPath, sizeof(state.romPath));
  if (ImGui::Button("Carregar caminho", ImVec2(160, 0)))
    emu.loadRom(state.romPath);
  ImGui::SameLine();
  if (ImGui::Button("Carregar lista", ImVec2(160, 0)))
    load_selected_media(state, emu);

  ImGui::Separator();
  ImGui::Text("Carregado: %s", emu.loadedRom().empty() ? "(nenhum)"
                                                       : emu.loadedRom().c_str());
  ImGui::Text("Execucao: %s", emu.paused() ? "pausada" : "rodando");
  ImGui::Spacing();
  ImGui::TextWrapped("Dica: ao trocar de sistema, a lista ja muda para a pasta e"
                     " extensoes esperadas por aquela maquina.");
  ImGui::EndChild();
}

void render_system_tab(GuiState &state, EmuFrontend &emu) {
  render_system_picker(state, emu, SystemCategory::Computers);
  ImGui::Separator();
  render_system_picker(state, emu, SystemCategory::Consoles);
  ImGui::Separator();
  render_system_picker(state, emu, SystemCategory::Arcade);
}

void render_video_tab(GuiState &state, const EmuFrontend &emu) {
  ImGui::SliderInt("Escala", &state.videoScale, 1, 6);
  ImGui::Checkbox("Escala inteira", &state.integerScale);
  ImGui::Checkbox("Manter aspecto", &state.keepAspect);

  ImGui::Separator();
  ImGui::TextUnformatted("Viewport");
  ImGui::BeginChild("viewport_preview", ImVec2(0, 430), true);
  ImVec2 size = ImGui::GetContentRegionAvail();
  core::VideoFrame frame = emu.videoFrame();
  if (update_video_texture(state, frame)) {
    ImVec2 imageSize = video_size_for_viewport(state, frame, size);
    float offsetX = std::max(0.0f, (size.x - imageSize.x) * 0.5f);
    if (offsetX > 0.0f)
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
    ImGui::Image(reinterpret_cast<ImTextureID>(
                     static_cast<intptr_t>(state.videoTexture)),
                 imageSize);
  } else {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                        IM_COL32(12, 13, 16, 255));
    draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                  IM_COL32(70, 76, 90, 255));
    ImGui::TextUnformatted("Sem frame de video.");
  }
  ImGui::EndChild();
}

void render_input_tab() {
  ImGui::TextUnformatted("Teclado");
  ImGui::Separator();
  ImGui::TextUnformatted("Setas: direcional");
  ImGui::TextUnformatted("Espaco/Z/X: botoes principais");
  ImGui::TextUnformatted("Enter: Start");
  ImGui::TextUnformatted("C/Backspace: Coin/Select");
}

void render_debug_tab(const EmuFrontend &emu) {
  ImGui::Text("Sistema: %s", emu.selectedSystem().name);
  ImGui::Text("Pausado: %s", emu.paused() ? "sim" : "nao");
  ImGui::Text("ROM: %s", emu.loadedRom().empty() ? "(nenhuma)"
                                                 : emu.loadedRom().c_str());
}

} // namespace

void gui_init(GuiState &state) {
  state.romPath[0] = '\0';
  state.mediaScanned = false;
  state.selectedMediaIndex = -1;
  state.scannedSystemId.clear();
  state.videoScale = 3;
  state.integerScale = true;
  state.keepAspect = true;
  state.videoTexture = 0;
  state.videoTextureWidth = 0;
  state.videoTextureHeight = 0;
}

void gui_render(GuiState &state, EmuFrontend &emu, bool &running) {
  render_main_menu(state, emu, running);

  ImGuiIO &io = ImGui::GetIO();
  const float menuHeight = 22.0f;
  ImGui::SetNextWindowPos(ImVec2(0.0f, menuHeight), ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(io.DisplaySize.x, io.DisplaySize.y - menuHeight),
      ImGuiCond_Always);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoCollapse;
  ImGui::Begin("Emulador", nullptr, flags);

  render_toolbar(state, emu);
  ImGui::Separator();

  if (ImGui::BeginTabBar("main_tabs")) {
    if (ImGui::BeginTabItem("ROM")) {
      render_rom_tab(state, emu);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Sistema")) {
      render_system_tab(state, emu);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Video")) {
      render_video_tab(state, emu);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Input")) {
      render_input_tab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Debug")) {
      render_debug_tab(emu);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::End();
}

void gui_shutdown(GuiState &state) {
  if (state.videoTexture != 0) {
    glDeleteTextures(1, &state.videoTexture);
    state.videoTexture = 0;
    state.videoTextureWidth = 0;
    state.videoTextureHeight = 0;
  }
}

} // namespace desktop
