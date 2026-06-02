#include "core/machine_factory.h"

#include "systems/computers/cpm/cpm_machine.h"
#include "systems/computers/msx/msx_ares_machine.h"
#include "systems/consoles/mastersystem/master_system_machine.h"
#include "systems/consoles/sg1000/sg1000_machine.h"
#include "systems/arcade/invaders/invaders_machine.h"

namespace core {

std::unique_ptr<Machine> CreateMachine(const std::string &systemId) {
  if (systemId == "msx")
    return std::make_unique<systems::computers::msx::AresStyleMsxMachine>();
  if (systemId == "cpm")
    return std::make_unique<systems::computers::cpm::CpmMachine>();
  if (systemId == "sg1000")
    return std::make_unique<systems::consoles::sg1000::SG1000Machine>();
  if (systemId == "mastersystem")
    return std::make_unique<
        systems::consoles::mastersystem::MasterSystemMachine>();
  if (systemId == "invaders")
    return std::make_unique<systems::arcade::invaders::SpaceInvadersMachine>();

  return nullptr;
}

} // namespace core
