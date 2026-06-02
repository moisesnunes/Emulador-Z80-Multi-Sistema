#pragma once

#include "core/machine.h"

#include <vector>

namespace core {

using SystemDefinition = MachineInfo;

const char *MachineFamilyName(MachineFamily family);
const std::vector<SystemDefinition> &SystemCatalog();

} // namespace core
