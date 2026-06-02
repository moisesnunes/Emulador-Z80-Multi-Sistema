#include "core/system_catalog.h"

namespace core {

const char *MachineFamilyName(MachineFamily family) {
  switch (family) {
  case MachineFamily::Computers:
    return "Computadores";
  case MachineFamily::Consoles:
    return "Consoles";
  case MachineFamily::Arcade:
    return "Arcade";
  }
  return "Desconhecido";
}

const std::vector<SystemDefinition> &SystemCatalog() {
  static const std::vector<SystemDefinition> systems = {
      {MachineFamily::Computers, "msx", "MSX (bus estilo ares)", true},
      {MachineFamily::Computers, "cpm", "CP/M", true},
      {MachineFamily::Consoles, "sg1000", "SG-1000", true},
      {MachineFamily::Consoles, "mastersystem", "Master System", true},
      {MachineFamily::Arcade, "invaders", "Space Invaders", false},
  };

  return systems;
}

} // namespace core
