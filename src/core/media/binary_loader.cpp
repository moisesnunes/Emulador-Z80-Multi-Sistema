#include "core/media/binary_loader.h"

#include <fstream>
#include <sstream>

namespace core {

bool LoadBinaryFile(const std::filesystem::path &path,
                    std::vector<uint8_t> &bytes, std::string &error) {
  bytes.clear();

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    error = "Nao foi possivel abrir: " + path.string();
    return false;
  }

  std::streamsize size = file.tellg();
  if (size <= 0) {
    error = "Arquivo vazio: " + path.string();
    return false;
  }

  file.seekg(0, std::ios::beg);
  bytes.resize(static_cast<std::size_t>(size));
  if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
    error = "Leitura incompleta: " + path.string();
    bytes.clear();
    return false;
  }

  return true;
}

} // namespace core
