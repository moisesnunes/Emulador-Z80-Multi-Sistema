#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace core {

bool LoadBinaryFile(const std::filesystem::path &path,
                    std::vector<uint8_t> &bytes, std::string &error);

} // namespace core
