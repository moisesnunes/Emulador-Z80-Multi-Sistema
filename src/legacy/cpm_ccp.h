#pragma once
#include "cpm_bios.h"

// Initialize CPMState for CCP mode: calls CPMInit then sets ccpMode/ccpRunning.
void CCPInit(intel8080* cpu, CPMState& cpm, const std::string& diskDir);

// Load a .COM file into TPA (0x0100), set up FCBs/command-tail, reset CPU state.
// Returns true on success; on failure prints an error and returns false.
bool CCPLoadCom(intel8080* cpu, CPMState& cpm,
                const std::string& name, const std::string& args);

// Called every main-loop iteration while ccpRunning == true.
// Accumulates typed characters; on CR processes the command.
// Returns true when a .COM has been loaded (caller should set ccpRunning=false).
bool CCPTick(intel8080* cpu, CPMState& cpm);
