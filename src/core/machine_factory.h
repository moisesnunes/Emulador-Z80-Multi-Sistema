#pragma once

#include "core/machine.h"

#include <memory>
#include <string>

namespace core {

std::unique_ptr<Machine> CreateMachine(const std::string &systemId);

} // namespace core
