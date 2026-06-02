#include "emu_frontend.h"

#include "core/machine_factory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

namespace desktop {

const char *CategoryName(SystemCategory category) {
  return core::MachineFamilyName(category);
}

namespace {

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool contains_path_part(const std::filesystem::path &path,
                        const char *needle) {
  std::string text = lowercase(path.generic_string());
  return text.find(needle) != std::string::npos;
}

} // namespace

EmuFrontend::EmuFrontend()
    : systems_(core::SystemCatalog()), selectedSystemIndex_(0), paused_(true),
      machine_(core::CreateMachine(systems_[0].id)),
      status_("Nenhuma ROM carregada.") {}

const std::vector<SystemInfo> &EmuFrontend::systems() const { return systems_; }

const SystemInfo &EmuFrontend::selectedSystem() const {
  return systems_[selectedSystemIndex_];
}

void EmuFrontend::selectSystem(const std::string &id) {
  for (std::size_t i = 0; i < systems_.size(); i++) {
    if (id == systems_[i].id && selectSystemByIndex(i))
      return;
  }
}

bool EmuFrontend::loadRom(const std::string &path) {
  if (path.empty()) {
    status_ = "Informe ou selecione uma ROM.";
    return false;
  }

  std::error_code ec;
  std::filesystem::path media(path);
  if (!media.is_absolute())
    media = std::filesystem::current_path(ec) / media;

  media = media.lexically_normal();
  if (!std::filesystem::exists(media, ec) ||
      !std::filesystem::is_regular_file(media, ec)) {
    status_ = "Arquivo de ROM nao encontrado.";
    return false;
  }

  std::string targetSystem = systemForMedia(media);
  if (!targetSystem.empty() && targetSystem != selectedSystem().id)
    selectSystem(targetSystem);

  if (!machine_) {
    status_ = std::string("Sistema ainda nao implementado no novo core: ") +
              selectedSystem().name;
    return false;
  }

  std::string error;
  if (!machine_->loadMedia(media, error)) {
    status_ = error;
    return false;
  }

  machine_->reset();
  loadedRom_ = media.string();
  paused_ = false;
  status_ = std::string("Rodando ") + selectedSystem().name +
            ": " + media.filename().string();
  return true;
}

void EmuFrontend::reset() {
  if (machine_)
    machine_->reset();
  status_ = loadedRom_.empty() ? "Reset solicitado sem ROM carregada."
                               : "Reset solicitado.";
}

void EmuFrontend::runFrame() {
  if (!paused_ && machine_)
    machine_->runFrame();
}

void EmuFrontend::setPaused(bool paused) {
  paused_ = paused;
  status_ = paused_ ? "Emulacao pausada." : "Emulacao marcada para execucao.";
}

void EmuFrontend::keyDown(int key) {
  if (machine_)
    machine_->keyDown(key);
}

void EmuFrontend::keyUp(int key) {
  if (machine_)
    machine_->keyUp(key);
}

bool EmuFrontend::paused() const { return paused_; }

const std::string &EmuFrontend::loadedRom() const { return loadedRom_; }

const std::string &EmuFrontend::status() const { return status_; }

bool EmuFrontend::hasMachine() const { return machine_ != nullptr; }

core::VideoFrame EmuFrontend::videoFrame() const {
  return machine_ ? machine_->videoFrame() : core::VideoFrame{};
}

std::filesystem::path EmuFrontend::mediaRootForSelectedSystem() const {
  std::string id = selectedSystem().id;
  if (id == "msx")
    return "roms/msx";
  if (id == "cpm")
    return "roms/cpm";
  if (id == "sg1000")
    return "roms/sg1000";
  if (id == "mastersystem")
    return "roms/mastersystem";
  if (id == "invaders")
    return "roms/invaders";
  return "roms";
}

std::vector<std::string> EmuFrontend::mediaExtensionsForSelectedSystem() const {
  std::string id = selectedSystem().id;
  if (id == "msx")
    return {".rom", ".mx1", ".mx2", ".bin"};
  if (id == "cpm")
    return {".com", ".img"};
  if (id == "sg1000")
    return {".sg", ".sc", ".bin", ".rom"};
  if (id == "mastersystem")
    return {".sms", ".bin", ".rom"};
  if (id == "invaders")
    return {".cfg", ".bin", ".h", ".g", ".f", ".e"};
  return {".rom", ".bin", ".cfg"};
}

bool EmuFrontend::isMediaCompatibleWithSelectedSystem(
    const std::filesystem::path &path) const {
  std::string ext = lowercase(path.extension().string());
  std::vector<std::string> extensions = mediaExtensionsForSelectedSystem();
  return std::find(extensions.begin(), extensions.end(), ext) !=
         extensions.end();
}

std::string EmuFrontend::recommendedMediaHint() const {
  std::string id = selectedSystem().id;
  if (id == "msx")
    return "Use ROMs MSX simples em roms/msx, por exemplo .rom.";
  if (id == "cpm")
    return "Use programas CP/M .COM em roms/cpm.";
  if (id == "sg1000")
    return "Use ROMs SG-1000 .sg/.sc/.bin.";
  if (id == "mastersystem")
    return "Use ROMs Master System .sms/.bin.";
  if (id == "invaders")
    return "Use roms/invaders/game.cfg para carregar os quatro blocos do jogo.";
  return "Selecione uma midia compativel com a maquina atual.";
}

std::vector<std::filesystem::path>
EmuFrontend::discoverMedia(const std::filesystem::path &root) const {
  std::vector<std::string> extensions = mediaExtensionsForSelectedSystem();
  std::vector<std::filesystem::path> result;
  std::error_code ec;
  if (!std::filesystem::exists(root, ec))
    return result;

  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  std::filesystem::recursive_directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec))
      continue;

    std::string ext = it->path().extension().string();
    ext = lowercase(ext);

    if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end())
      result.push_back(it->path());

    if (result.size() >= 400)
      break;
  }

  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::filesystem::path>
EmuFrontend::discoverMediaForSelectedSystem() const {
  std::vector<std::filesystem::path> result =
      discoverMedia(mediaRootForSelectedSystem());
  if (!result.empty())
    return result;
  return discoverMedia("roms");
}

std::string EmuFrontend::systemForMedia(const std::filesystem::path &path) const {
  std::string ext = lowercase(path.extension().string());
  if (contains_path_part(path, "roms/invaders") || ext == ".h" || ext == ".g" ||
      ext == ".f" || ext == ".e")
    return "invaders";
  if (contains_path_part(path, "roms/msx"))
    return "msx";
  if (contains_path_part(path, "roms/cpm") || ext == ".com")
    return "cpm";
  if (contains_path_part(path, "roms/sg1000") || ext == ".sg" || ext == ".sc")
    return "sg1000";
  if (contains_path_part(path, "roms/mastersystem") || ext == ".sms")
    return "mastersystem";
  return "";
}

bool EmuFrontend::selectSystemByIndex(std::size_t index) {
  selectedSystemIndex_ = index;
  machine_ = core::CreateMachine(systems_[index].id);
  loadedRom_.clear();
  paused_ = true;
  status_ = std::string("Sistema selecionado: ") + systems_[index].name;
  if (!machine_)
    status_ += " (ainda sem maquina conectada ao novo core)";
  return true;
}

} // namespace desktop
