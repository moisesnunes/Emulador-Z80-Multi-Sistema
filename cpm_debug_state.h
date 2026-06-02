// ═══════════════════════════════════════════════════════════════════════════════
// CPMDebugState.h - Estruturas de debug para CP/M
// ═══════════════════════════════════════════════════════════════════════════════

#ifndef CPM_DEBUG_STATE_H
#define CPM_DEBUG_STATE_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════════════════
// Histórico de Instruções Executadas
// ═══════════════════════════════════════════════════════════════════════════════

struct InstructionLog
{
    uint16_t pc;        // Program Counter quando instrução foi executada
    uint8_t opcode;     // Opcode (1º byte)
    uint8_t param1;     // 2º byte (se houver)
    uint8_t param2;     // 3º byte (se houver)
    uint64_t timestamp; // Quando foi executado (em ciclos)

    // Estado da CPU naquele momento
    uint8_t A, B, C, D, E, H, L;
    uint16_t SP, PC;
    bool carry, zero, sign, parity;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Estado do Debugger CP/M
// ═══════════════════════════════════════════════════════════════════════════════

struct CPMDebugState
{
    static constexpr int MAXINSTR = 1000; // Máximo de instruções a guardar

    // ─── Controle de Execução ──────────────────────────────────────────────
    bool notHalted = false;        // Executar continuamente?
    bool runOnce = false;          // Executar uma instrução?
    bool runFrame = true;          // Executar um frame?
    bool breakpointActive = false; // Tem breakpoint ativo?

    // ─── Breakpoints ────────────────────────────────────────────────────────
    char pauseLine[5] = {};      // Endereço do breakpoint em hex string
    uint16_t breakpointAddr = 0; // Endereço do breakpoint

    // ─── Histórico de Instruções ───────────────────────────────────────────
    InstructionLog previousInstructions[MAXINSTR];
    int currentInstruction = 0; // Índice atual no histórico
    int totalInstructions = 0;  // Total de instruções executadas

    // ─── Timing ─────────────────────────────────────────────────────────────
    long long cycleTime = 0;  // Tempo por instrução (em unidades)
    uint64_t totalCycles = 0; // Total de ciclos executados

    // ─── Estado do CP/M ─────────────────────────────────────────────────────
    uint8_t currentDrive = 0;     // Drive atual (0=A, 1=B, etc)
    uint8_t currentUser = 0;      // User ID atual (0-15)
    uint16_t dmaAddress = 0x0080; // DMA address

    // ─── BDOS State ─────────────────────────────────────────────────────────
    uint8_t lastBdosCall = 0xFF; // Última função BDOS chamada
    uint16_t lastBdosAddr = 0;   // Endereço onde BDOS foi chamado
    bool inBdos = false;         // Atualmente dentro de BDOS?

    // ─── BDOS History ────────────────────────────────────────────────────────
    struct BdosLogEntry
    {
        uint8_t fn = 0xFF;
        bool valid = false;
    };
    static constexpr int BDOS_LOG_MAX = 32;
    BdosLogEntry bdosLog[BDOS_LOG_MAX] = {};
    int bdosLogHead = 0;
    int bdosLogCount = 0;
    void logBdosCall(uint8_t fn);

    // ─── Terminal State ─────────────────────────────────────────────────────
    std::string lastCommand = ""; // Último comando CCP digitado
    std::string lastOutput = "";  // Última linha do terminal
    bool ccpActive = false;       // CCP está rodando?

    // ─── Flags de Debug ─────────────────────────────────────────────────────
    bool showMemory = false;          // Mostrar editor de memória?
    bool showRegisters = true;        // Mostrar registradores?
    bool showInstructions = true;     // Mostrar histórico de instruções?
    bool showBdosInfo = true;         // Mostrar informações BDOS?
    bool showStackInfo = true;        // Mostrar info da stack?
    uint16_t memoryViewAddr = 0x0100; // Endereço inicial da memória a visualizar

    // ─── Performance ────────────────────────────────────────────────────────
    int instructionsPerSecond = 0;
    double avgCyclesPerInstruction = 0.0;

    // ─── NVRAM ───────────────────────────────────────────────────────────────
    bool nvramAutoSave = false; // salvar estado ao fechar a janela
    char nvramPath[512] = {};   // caminho derivado do diskDir pelo emulador

    // ─── Speed throttle ─────────────────────────────────────────────────────
    double targetMHz = 0.0; // 0 = unlimited; otherwise throttle CPU to this speed

    // ─── Detecção de Loop Infinito ───────────────────────────────────────────
    // Pausa a execução se o PC ficar dentro de uma janela de 64 bytes por mais
    // de STUCK_CYCLE_LIMIT ciclos consecutivos sem passar por BDOS/BIOS.
    static constexpr uint64_t STUCK_CYCLE_LIMIT = 20'000'000; // ~10s a 2 MHz
    static constexpr uint16_t STUCK_WINDOW_BYTES = 64;

    bool stuckDetectionEnabled = true;
    bool stuckDetected = false;
    uint16_t stuckDetectedPC = 0;
    uint64_t stuckCycleCount = 0;
    uint16_t stuckWindowMin = 0;
    uint16_t stuckWindowMax = 0;

    void tickStuck(uint16_t newPC, uint8_t opCycles);
    void resetStuckCounter();
    void clearStuckDetected();

    // ─── Tabela de Símbolos ──────────────────────────────────────────────────
    std::unordered_map<uint16_t, std::string> symbols;
    char symbolFilePath[512] = "symbols.sym";

    // Contagem de execução por endereço de opcode (PC no fetch) — heatmap no debugger
    uint32_t execHits[0x10000]{};
    void clearExecHeatmap();

    // ═══════════════════════════════════════════════════════════════════════
    // Métodos
    // ═══════════════════════════════════════════════════════════════════════

    void logInstruction(class intel8080 *cpu);
    void updateBdosInfo(uint8_t function);
    void updateCcpInfo(const std::string &command, const std::string &output);
    std::string getInstructionDisplay(int index);
    std::string getRegisterDisplay(class intel8080 *cpu);
    std::string getMemoryDisplay(class intel8080 *cpu, uint16_t addr, int lines);
    std::string getBdosDisplay();
    std::string getStackDisplay(class intel8080 *cpu);

    void loadSymbols(const char *path);
    const char *resolveSymbol(uint16_t addr) const;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Tabela de Funções BDOS
// ═══════════════════════════════════════════════════════════════════════════════

static const char *BDOS_FUNCTION_NAMES[] = {
    "System Reset",           // 0x00
    "Console Input",          // 0x01
    "Console Output",         // 0x02
    "Aux Input",              // 0x03
    "Aux Output",             // 0x04
    "List Output",            // 0x05
    "Direct I/O",             // 0x06
    "Get I/O Byte",           // 0x07
    "Set I/O Byte",           // 0x08
    "Print String",           // 0x09
    "Read Buffer",            // 0x0A
    "Get Console Status",     // 0x0B
    "Return Version",         // 0x0C
    "Reset Disk System",      // 0x0D
    "Select Drive",           // 0x0E
    "Open File",              // 0x0F
    "Close File",             // 0x10
    "Search For First",       // 0x11
    "Search For Next",        // 0x12
    "Delete File",            // 0x13
    "Read Sequential",        // 0x14
    "Write Sequential",       // 0x15
    "Make File",              // 0x16
    "Rename File",            // 0x17
    "Return Login Vector",    // 0x18
    "Return Current Drive",   // 0x19
    "Set DMA Address",        // 0x1A
    "Get Alloc Vector",       // 0x1B
    "Write Protect Disk",     // 0x1C
    "Get Read Only Vector",   // 0x1D
    "Set File Attributes",    // 0x1E
    "Get Disk Parameters",    // 0x1F
    "Set/Get User Code",      // 0x20
    "Read Random",            // 0x21
    "Write Random",           // 0x22
    "Compute File Size",      // 0x23
    "Set Random Record",      // 0x24
    "Reset Drive",            // 0x25
    "Access Drive",           // 0x26
    "Free Drive",             // 0x27
    "Write Random with Fill", // 0x28
    "Parse Filename",         // 0x29
};

static const int BDOS_FUNCTION_COUNT = 0x2A;

// ═══════════════════════════════════════════════════════════════════════════════
// Implementação Inline
// ═══════════════════════════════════════════════════════════════════════════════

inline void CPMDebugState::updateBdosInfo(uint8_t function)
{
    lastBdosCall = function;
    inBdos = true;
    logBdosCall(function);
}

inline void CPMDebugState::logBdosCall(uint8_t fn)
{
    int idx = bdosLogHead % BDOS_LOG_MAX;
    bdosLog[idx].fn = fn;
    bdosLog[idx].valid = true;
    bdosLogHead++;
    if (bdosLogCount < BDOS_LOG_MAX)
        bdosLogCount++;
}

inline void CPMDebugState::updateCcpInfo(const std::string &command, const std::string &output)
{
    lastCommand = command;
    lastOutput = output;
    ccpActive = true;
}

inline std::string CPMDebugState::getBdosDisplay()
{
    if (lastBdosCall == 0xFF)
        return "No BDOS call";

    std::string result = "BDOS Call: 0x";
    result += (char)('0' + (lastBdosCall >> 4));
    result += (char)((lastBdosCall & 0xF) > 9 ? 'A' + (lastBdosCall & 0xF) - 10 : '0' + (lastBdosCall & 0xF));

    if (lastBdosCall < BDOS_FUNCTION_COUNT)
    {
        result += " (";
        result += BDOS_FUNCTION_NAMES[lastBdosCall];
        result += ")";
    }

    return result;
}

// ─── Stuck / infinite-loop detection ─────────────────────────────────────────

inline void CPMDebugState::tickStuck(uint16_t newPC, uint8_t opCycles)
{
    if (!stuckDetectionEnabled || stuckDetected)
        return;

    if (stuckCycleCount == 0)
    {
        stuckWindowMin = stuckWindowMax = newPC;
    }
    if (newPC < stuckWindowMin)
        stuckWindowMin = newPC;
    if (newPC > stuckWindowMax)
        stuckWindowMax = newPC;

    if ((uint16_t)(stuckWindowMax - stuckWindowMin) > STUCK_WINDOW_BYTES)
    {
        stuckWindowMin = stuckWindowMax = newPC;
        stuckCycleCount = opCycles;
        return;
    }

    stuckCycleCount += opCycles;
    if (stuckCycleCount >= STUCK_CYCLE_LIMIT)
    {
        stuckDetected = true;
        stuckDetectedPC = newPC;
    }
}

inline void CPMDebugState::resetStuckCounter()
{
    stuckCycleCount = 0;
    stuckWindowMin = 0;
    stuckWindowMax = 0;
}

inline void CPMDebugState::clearStuckDetected()
{
    stuckDetected = false;
    stuckDetectedPC = 0;
    resetStuckCounter();
}

#endif // CPM_DEBUG_STATE_H
