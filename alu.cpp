#include <cstdio>
#include "alu.h"
#include "intel8080.h"
#include "zilogZ80.h"
#include "cpm_bios.h"

// Global pointer set by the CP/M emulator so MachineIn/Out can access serial state.
// Null when running in arcade mode.
CPMState *g_serialState = nullptr;

uint16_t ReadWord(intel8080 *cpu, uint16_t pc)
{
    return cpu->ReadMem16(pc);
}

uint16_t AdvanceWord(intel8080 *const cpu)
{
    uint16_t result = ReadWord(cpu, cpu->PC);
    cpu->PC += 2;
    return result;
}

uint8_t ReadByte(intel8080 *cpu, uint16_t pc)
{
    return cpu->ReadMem8(pc);
}

uint8_t AdvanceByte(intel8080 *const cpu)
{
    uint8_t result = ReadByte(cpu, cpu->PC);
    cpu->PC += 1;
    return result;
}

uint8_t ReadMemoryHL(intel8080 *const cpu)
{
    uint16_t address = (cpu->H << 8) | (cpu->L & 0xFF);
    return cpu->ReadMem8(address);
}

void WriteMemoryHL(intel8080 *const cpu, uint8_t val)
{
    uint16_t address = (cpu->H << 8) | (cpu->L & 0xFF);
    // cpu->memory[address] = val;
    cpu->WriteMem(address, val);
}

uint16_t ReadRegisterMemory(intel8080 *const cpu, uint8_t a, uint8_t b)
{
    uint16_t registerAddress = (a << 8) | (b & 0xFF);
    return cpu->ReadMem8(registerAddress);
}

void WriteRegisterMemory(intel8080 *const cpu, uint16_t memoryAddress, uint8_t val)
{
    // cpu->memory[memoryAddress] = val;
    cpu->WriteMem(memoryAddress, val);
}

uint16_t GetBC(intel8080 *const cpu)
{
    return (cpu->B << 8) | cpu->C;
}

uint16_t GetDE(intel8080 *const cpu)
{
    return (cpu->D << 8) | cpu->E;
}

void SetBC(intel8080 *const cpu, uint16_t value)
{
    cpu->B = value >> 8;
    cpu->C = value & 0xFF;
}

void SetDE(intel8080 *const cpu, uint16_t value)
{
    cpu->D = value >> 8;
    cpu->E = value & 0xFF;
}

void SetHL(intel8080 *const cpu, uint16_t value)
{
    cpu->H = value >> 8;
    cpu->L = value & 0xFF;
}

bool FullCarry(uint8_t a, uint8_t b, bool cf)
{
    uint16_t result = a + b + cf;
    return (result >> 8) & 1;
}

bool HalfCarry(uint8_t a, uint8_t b, bool cf)
{
    uint16_t result = a + b + cf;
    uint16_t carry = result ^ a ^ b;
    return (carry >> 4) & 1;
}

static void Z80SetUndocXY(intel8080 *cpu, uint8_t r)
{
    zilogZ80 *z = static_cast<zilogZ80 *>(cpu);
    z->xf = (r >> 3) & 1;
    z->yf = (r >> 5) & 1;
}

void AddReg(intel8080 *cpu, uint8_t *reg, uint8_t val, bool cf)
{
    uint8_t a = *reg;
    uint8_t result = a + val + cf;
    cpu->cf = FullCarry(a, val, cf);
    if (cpu->z80F)
    {
        cpu->acf = ((a ^ val ^ result) >> 4) & 1;
        cpu->sf = (result >> 7) & 1;
        cpu->zf = (result == 0);
        cpu->pf = (((~(a ^ val)) & (a ^ result)) & 0x80) != 0;
        Z80SetUndocXY(cpu, result);
        static_cast<zilogZ80 *>(cpu)->nf = false;
    }
    else
    {
        cpu->acf = HalfCarry(a, val, cf);
        cpu->Set_SZP_Flags(result);
    }
    *reg = result;
}

void SubReg(intel8080 *cpu, uint8_t *reg, uint8_t val, bool cf)
{
    if (!cpu->z80F)
    {
        AddReg(cpu, reg, ~val, !cf);
        cpu->cf = !cpu->cf;
        return;
    }

    uint8_t a = *reg;
    uint16_t sub = (uint16_t)val + (uint16_t)cf;
    uint16_t diff = (uint16_t)a - sub;
    uint8_t result = (uint8_t)diff;
    cpu->cf = (a < sub) ? 1 : 0;
    cpu->acf = ((a ^ val ^ result) >> 4) & 1;
    cpu->sf = (result >> 7) & 1;
    cpu->zf = (result == 0);
    cpu->pf = (((a ^ val) & (a ^ result)) & 0x80) != 0;
    Z80SetUndocXY(cpu, result);
    static_cast<zilogZ80 *>(cpu)->nf = true;
    *reg = result;
}

void INX(intel8080 *cpu, uint8_t *a, uint8_t *b)
{
    uint16_t result = ((*a << 8) | (*b & 0xFF)) + 1;
    *a = (result >> 8) & 0xFF;
    *b = result & 0xFF;
}

void DCX(intel8080 *cpu, uint8_t *rega, uint8_t *regb)
{
    uint16_t result = ((*rega << 8) | (*regb & 0xFF)) - 1;
    *rega = (result >> 8) & 0xFF;
    *regb = result & 0xFF;
}

uint8_t INR(intel8080 *cpu, uint8_t registerVal)
{
    uint8_t a = registerVal;
    uint8_t result = a + 1;
    if (cpu->z80F)
    {
        cpu->acf = (((a & 0xFU) + 1U) > 0x0FU);
        cpu->sf = (result >> 7) & 1;
        cpu->zf = (result == 0);
        cpu->pf = (a == 0x7FU);
        Z80SetUndocXY(cpu, result);
        static_cast<zilogZ80 *>(cpu)->nf = false;
    }
    else
    {
        cpu->acf = (result & 0xF) == 0;
        cpu->Set_SZP_Flags(result);
    }
    return result;
}

uint8_t DCR(intel8080 *cpu, uint8_t registerVal)
{
    uint8_t a = registerVal;
    uint8_t result = a - 1;
    if (cpu->z80F)
    {
        cpu->acf = ((a & 0xFU) < 1U);
        cpu->sf = (result >> 7) & 1;
        cpu->zf = (result == 0);
        cpu->pf = (a == 0x80U);
        Z80SetUndocXY(cpu, result);
        static_cast<zilogZ80 *>(cpu)->nf = true;
    }
    else
    {
        cpu->acf = !((result & 0xF) == 0xF);
        cpu->Set_SZP_Flags(result);
    }
    return result;
}

void ANA(intel8080 *cpu, uint8_t *reg, uint8_t val)
{
    uint8_t result = *reg & val;
    cpu->cf = 0;
    if (cpu->z80F)
    {
        cpu->acf = true;
        cpu->sf = (result >> 7) & 1;
        cpu->zf = (result == 0);
        cpu->pf = !__builtin_parity(result);
        Z80SetUndocXY(cpu, result);
        static_cast<zilogZ80 *>(cpu)->nf = false;
    }
    else
    {
        cpu->acf = ((*reg | val) >> 3) & 1;
        cpu->Set_SZP_Flags(result);
    }
    *reg = result;
}

void XRA(intel8080 *cpu, uint8_t *reg, uint8_t val)
{
    uint8_t result = *reg ^ val;
    cpu->acf = 0;
    cpu->cf = 0;
    if (cpu->z80F)
    {
        cpu->sf = (result >> 7) & 1;
        cpu->zf = (result == 0);
        cpu->pf = !__builtin_parity(result);
        Z80SetUndocXY(cpu, result);
        static_cast<zilogZ80 *>(cpu)->nf = false;
    }
    else
        cpu->Set_SZP_Flags(result);
    *reg = result;
}

void ORA(intel8080 *cpu, uint8_t *reg, uint8_t val)
{
    uint8_t result = *reg | val;
    cpu->acf = 0;
    cpu->cf = 0;
    if (cpu->z80F)
    {
        cpu->sf = (result >> 7) & 1;
        cpu->zf = (result == 0);
        cpu->pf = !__builtin_parity(result);
        Z80SetUndocXY(cpu, result);
        static_cast<zilogZ80 *>(cpu)->nf = false;
    }
    else
        cpu->Set_SZP_Flags(result);
    *reg = result;
}

void CMP(intel8080 *cpu, uint8_t reg, uint8_t val)
{
    uint8_t tmp = reg;
    SubReg(cpu, &tmp, val, 0);
    if (cpu->z80F)
        Z80SetUndocXY(cpu, val);
}

void SHLD(intel8080 *cpu)
{
    uint16_t address = AdvanceWord(cpu);
    // cpu->memory[address] = cpu->L;
    // cpu->memory[address+1] = cpu->H;
    cpu->WriteMem(address, cpu->L);
    cpu->WriteMem(address + 1, cpu->H);
}

void STA(intel8080 *cpu)
{
    uint16_t address = AdvanceWord(cpu);
    // cpu->memory[address] = cpu->A;
    cpu->WriteMem(address, cpu->A);
}

void DAD(intel8080 *cpu, uint8_t rega, uint8_t regb)
{
    uint16_t reagab = (rega << 8) | (regb & 0xFF);
    uint16_t reaghl = (cpu->H << 8) | (cpu->L & 0xFF);
    uint32_t result = reagab + reaghl;
    cpu->cf = (result >> 16) & 0x01;
    cpu->H = (result >> 8) & 0xFF;
    cpu->L = result & 0xFF;
}

void DAD(intel8080 *cpu, uint16_t SP)
{
    uint16_t reaghl = (cpu->H << 8) | (cpu->L & 0xFF);
    uint32_t result = SP + reaghl;
    cpu->cf = (result >> 16) & 0x01;
    cpu->H = (result >> 8) & 0xFF;
    cpu->L = result & 0xFF;
    // std::cout << "added to SP in DAD" << std::endl;
}

void RLC(intel8080 *cpu)
{
    uint16_t result = (cpu->A) << 1;
    result |= ((cpu->A & 0xFF) >> 7);
    cpu->cf = result & 0x01;
    cpu->A = result & 0xFF;
}

void RAL(intel8080 *cpu)
{
    uint16_t result = (cpu->A) << 1;
    result |= cpu->cf;
    cpu->cf = ((cpu->A & 0xFF) >> 7) & 0x01;
    cpu->A = result;
}

void RRC(intel8080 *cpu)
{
    uint16_t result = (cpu->A) >> 1;
    result |= ((cpu->A << 7) & 0x80);
    cpu->cf = (result >> 7) & 0x01;
    cpu->A = result & 0xFF;
}

void RAR(intel8080 *cpu)
{
    uint16_t result = (cpu->A) >> 1;
    result |= (cpu->cf << 7);
    cpu->cf = (cpu->A & 0x01);
    cpu->A = result;
}

void DAA(intel8080 *cpu)
{
    bool cy = cpu->cf;
    uint8_t correction = 0;
    uint8_t lsb = cpu->A & 0x0F;
    uint8_t msb = cpu->A >> 4;

    if (cpu->acf || lsb > 9)
        correction += 0x06;

    if (cpu->cf || msb > 9 || (msb >= 9 && lsb > 9))
    {
        correction += 0x60;
        cy = 1;
    }

    AddReg(cpu, &cpu->A, correction, 0);
    cpu->cf = cy;
}

void LHLD(intel8080 *cpu)
{
    uint16_t address = AdvanceWord(cpu);
    cpu->L = cpu->ReadMem8(address);
    cpu->H = cpu->ReadMem8((uint16_t)(address + 1));
}

void JMP(intel8080 *cpu)
{
    uint16_t address = AdvanceWord(cpu);
    cpu->PC = address;
}

void JNZ(intel8080 *cpu)
{
    if (!cpu->zf)
    {
        JMP(cpu);
    }
    else
    {
        AdvanceWord(cpu);
    }
}

void JNC(intel8080 *cpu)
{
    if (!cpu->cf)
    {
        JMP(cpu);
    }
    else
    {
        AdvanceWord(cpu);
    }
}

void JPO(intel8080 *cpu)
{
    if (!cpu->pf)
    {
        JMP(cpu);
    }
    else
    {
        AdvanceWord(cpu);
    }
}

void JP(intel8080 *cpu)
{
    if (!cpu->sf)
    {
        JMP(cpu);
    }
    else
    {
        AdvanceWord(cpu);
    }
}

void JZ(intel8080 *cpu)
{
    if (cpu->zf)
    {
        JMP(cpu);
    }
    else
    {
        AdvanceWord(cpu);
    }
}

void JC(intel8080 *cpu)
{
    if (cpu->cf)
    {
        JMP(cpu);
    }
    else
    {
        AdvanceWord(cpu);
    }
}

void JPE(intel8080 *cpu)
{
    if (cpu->pf)
    {
        JMP(cpu);
    }
    else
    {
        AdvanceWord(cpu);
    }
}

void JM(intel8080 *cpu)
{
    if (cpu->sf)
    {
        JMP(cpu);
    }
    else
    {
        AdvanceWord(cpu);
    }
}

void RET(intel8080 *cpu)
{
    uint16_t result = cpu->ReadMem16(cpu->SP);
    cpu->PC = result;
    cpu->SP = cpu->SP + 2;
    // std::cout << "added to SP + 2 in RET" << std::endl;
}

// Conditional RET: base cost is 5 cycles (table); add 6 when taken → 11 total.
#define COND_RET(cpu, cond)              \
    do                                   \
    {                                    \
        if (cond)                        \
        {                                \
            RET(cpu);                    \
            (cpu)->cycles += 6;          \
            (cpu)->cyclesInterrupt += 6; \
        }                                \
    } while (0)

void RNZ(intel8080 *cpu) { COND_RET(cpu, !cpu->zf); }
void RNC(intel8080 *cpu) { COND_RET(cpu, !cpu->cf); }
void RPO(intel8080 *cpu) { COND_RET(cpu, !cpu->pf); }
void RP(intel8080 *cpu) { COND_RET(cpu, !cpu->sf); }
void RZ(intel8080 *cpu) { COND_RET(cpu, cpu->zf); }
void RC(intel8080 *cpu) { COND_RET(cpu, cpu->cf); }
void RPE(intel8080 *cpu) { COND_RET(cpu, cpu->pf); }
void RM(intel8080 *cpu) { COND_RET(cpu, cpu->sf); }

#undef COND_RET

void PushRegisterPair(intel8080 *cpu, uint8_t HReg, uint8_t LReg)
{
    // cpu->memory[cpu->SP-1] = HReg;
    // cpu->memory[cpu->SP-2] = LReg;

    cpu->WriteMem(cpu->SP - 1, HReg);
    cpu->WriteMem(cpu->SP - 2, LReg);

    cpu->SP = cpu->SP - 2;
    // std::cout << "subtracted 2 from SP in PushReg" << std::endl;
}

void PopRegisterPair(intel8080 *cpu, uint8_t *HReg, uint8_t *LReg)
{
    *LReg = cpu->ReadMem8(cpu->SP);
    *HReg = cpu->ReadMem8((uint16_t)(cpu->SP + 1));
    cpu->SP = cpu->SP + 2;
    // std::cout << "added to SP +2 in PopReg" << std::endl;
}

void PushPSW(intel8080 *cpu)
{
    uint8_t statusReg = (cpu->sf << 7) | (cpu->zf << 6) | (0 << 5) | (cpu->acf << 4) | (0 << 3) | (cpu->pf << 2) | (1 << 1) | (cpu->cf << 0);
    // cpu->memory[cpu->SP - 1] = cpu->A;
    // cpu->memory[cpu->SP - 2] = statusReg;

    cpu->WriteMem(cpu->SP - 1, cpu->A);
    cpu->WriteMem(cpu->SP - 2, statusReg);

    cpu->SP = cpu->SP - 2;
    // std::cout << "subtracted 2 from SP in PushPSW" << std::endl;
}

void PopPSW(intel8080 *cpu)
{
    uint8_t statusReg = cpu->ReadMem8(cpu->SP);
    cpu->A = cpu->ReadMem8((uint16_t)(cpu->SP + 1));
    cpu->sf = (statusReg >> 7) & 0x01;
    cpu->zf = (statusReg >> 6) & 0x01;
    cpu->acf = (statusReg >> 4) & 0x01;
    cpu->pf = (statusReg >> 2) & 0x01;
    cpu->cf = statusReg & 0x01;
    cpu->SP = cpu->SP + 2;

    // std::cout << "added to SP + 2 in PopPSW" << std::endl;
}

void Call(intel8080 *cpu)
{
    uint16_t nextAddress = cpu->PC + 2; // next command address hopefully, Call is 3 bytes, cycle adds 1 to PC, add 2 to get next instruction
    uint8_t HReg = (nextAddress >> 8) & 0xFF;
    uint8_t LReg = (nextAddress) & 0xFF;
    // cpu->memory[cpu->SP - 1] = HReg;
    // cpu->memory[cpu->SP - 2] = LReg;
    cpu->WriteMem(cpu->SP - 1, HReg);
    cpu->WriteMem(cpu->SP - 2, LReg);
    cpu->SP = cpu->SP - 2;
    cpu->PC = AdvanceWord(cpu);
    // std::cout << "subtracted 2 from SP in Call" << std::endl;
}

// Conditional CALL: base cost is 11 cycles (table); add 6 when taken → 17 total.
#define COND_CALL(cpu, cond)             \
    do                                   \
    {                                    \
        if (cond)                        \
        {                                \
            Call(cpu);                   \
            (cpu)->cycles += 6;          \
            (cpu)->cyclesInterrupt += 6; \
        }                                \
        else                             \
        {                                \
            AdvanceWord(cpu);            \
        }                                \
    } while (0)

void CNZ(intel8080 *cpu) { COND_CALL(cpu, !cpu->zf); }
void CNC(intel8080 *cpu) { COND_CALL(cpu, !cpu->cf); }
void CPO(intel8080 *cpu) { COND_CALL(cpu, !cpu->pf); }
void CP(intel8080 *cpu) { COND_CALL(cpu, !cpu->sf); }
void CZ(intel8080 *cpu) { COND_CALL(cpu, cpu->zf); }
void CC(intel8080 *cpu) { COND_CALL(cpu, cpu->cf); }
void CPE(intel8080 *cpu) { COND_CALL(cpu, cpu->pf); }
void CM(intel8080 *cpu) { COND_CALL(cpu, cpu->sf); }

#undef COND_CALL

void RST(intel8080 *cpu, uint8_t N)
{
    uint8_t HReg = (cpu->PC >> 8) & 0xFF;
    uint8_t LReg = cpu->PC & 0xFF;
    // cpu->memory[cpu->SP - 1] = HReg;
    // cpu->memory[cpu->SP - 2] = LReg;
    cpu->WriteMem(cpu->SP - 1, HReg);
    cpu->WriteMem(cpu->SP - 2, LReg);
    cpu->SP = cpu->SP - 2;
    cpu->PC = N * 8;
    // std::cout << "subtracted 2 from SP in RST" << std::endl;
}

void ADI(intel8080 *cpu)
{
    AddReg(cpu, &cpu->A, AdvanceByte(cpu), 0);
}

void ACI(intel8080 *cpu)
{
    AddReg(cpu, &cpu->A, AdvanceByte(cpu), cpu->cf);
}

void SUI(intel8080 *cpu)
{
    SubReg(cpu, &cpu->A, AdvanceByte(cpu), 0);
}

void SBI(intel8080 *cpu)
{
    SubReg(cpu, &cpu->A, AdvanceByte(cpu), cpu->cf);
}

void ANI(intel8080 *cpu)
{
    ANA(cpu, &cpu->A, AdvanceByte(cpu));
}

void ORI(intel8080 *cpu)
{
    ORA(cpu, &cpu->A, AdvanceByte(cpu));
}

void XRI(intel8080 *cpu)
{
    XRA(cpu, &cpu->A, AdvanceByte(cpu));
}

void CPI(intel8080 *cpu)
{
    CMP(cpu, cpu->A, AdvanceByte(cpu));
}

void XTHL(intel8080 *cpu)
{
    uint8_t HReg = cpu->H;
    uint8_t LReg = cpu->L;
    uint8_t SP0 = cpu->ReadMem8(cpu->SP);
    uint8_t SP1 = cpu->ReadMem8((uint16_t)(cpu->SP + 1));
    // cpu->memory[cpu->SP] = LReg;
    // cpu->memory[cpu->SP + 1] = HReg;
    cpu->WriteMem(cpu->SP, LReg);
    cpu->WriteMem(cpu->SP + 1, HReg);
    cpu->L = SP0;
    cpu->H = SP1;
}

void PCHL(intel8080 *cpu)
{
    cpu->PC = (cpu->H << 8) | cpu->L;
}

void SPHL(intel8080 *cpu)
{
    cpu->SP = (cpu->H << 8) | cpu->L;
}

void XCHG(intel8080 *cpu)
{
    uint8_t DReg = cpu->D;
    uint8_t EReg = cpu->E;
    uint8_t HReg = cpu->H;
    uint8_t LReg = cpu->L;
    cpu->D = HReg;
    cpu->E = LReg;
    cpu->H = DReg;
    cpu->L = EReg;
}

// Per-port handler table
static PortInFn g_portIn[256];
static PortOutFn g_portOut[256];

void RegisterPortIn(uint8_t port, PortInFn fn) { g_portIn[port] = fn; }
void RegisterPortOut(uint8_t port, PortOutFn fn) { g_portOut[port] = fn; }

void ClearPortHandlers()
{
    for (int i = 0; i < 256; i++)
    {
        g_portIn[i] = nullptr;
        g_portOut[i] = nullptr;
    }
}

// Machine-specific registration helpers
void RegisterSpaceInvadersPorts()
{
    RegisterPortIn(3, [](intel8080 *cpu) -> uint8_t
                   { return (cpu->shiftRegister >> (8 - cpu->shiftOffset)) & 0xFF; });
    RegisterPortOut(2, [](intel8080 *cpu)
                    { cpu->shiftOffset = cpu->A & 0x07; });
    RegisterPortOut(4, [](intel8080 *cpu)
                    {
        uint16_t mask = 0xFF00;
        uint16_t value = cpu->A << 8;
        cpu->shiftRegister &= ~(mask >> cpu->shiftOffset);
        cpu->shiftRegister |= value >> cpu->shiftOffset; });
}

// 88-SIO (Altair 8800): port 0x00 = status, port 0x01 = data.
// Bit 0 of status is ACTIVE LOW: 0 = input ready, 1 = no input.
// Output routine checks ANI 0xC8 — error bits are kept clear so output always proceeds.
void RegisterAltairSIOPorts()
{
    RegisterPortIn(0x00, [](intel8080 *) -> uint8_t
                   {
                       if (g_serialState && g_serialState->serial.connected() &&
                           !g_serialState->serial.rxBuf.empty())
                           return 0x00; // bit 0 = 0 → input ready
                       return 0x01;     // bit 0 = 1 → no input
                   });
    RegisterPortIn(0x01, [](intel8080 *) -> uint8_t
                   {
        if (g_serialState && g_serialState->serial.connected() &&
            !g_serialState->serial.rxBuf.empty())
        {
            uint8_t b = g_serialState->serial.rxBuf.front();
            g_serialState->serial.rxBuf.pop_front();
            return b;
        }
        return 0x00; });
    // Buffer unconditionally so output before nc connects is not lost.
    RegisterPortOut(0x01, [](intel8080 *cpu)
                    {
        if (g_serialState)
            g_serialState->serial.txBuf.push_back(cpu->A); });
}

// 8251 USART simulation: port 0x10 = data, port 0x11 = status/command.
void RegisterUSARTPorts()
{
    RegisterPortIn(0x10, [](intel8080 *) -> uint8_t
                   {
        if (g_serialState && g_serialState->serial.connected() &&
            !g_serialState->serial.rxBuf.empty())
        {
            uint8_t b = g_serialState->serial.rxBuf.front();
            g_serialState->serial.rxBuf.pop_front();
            return b;
        }
        return 0x00; });
    RegisterPortIn(0x11, [](intel8080 *) -> uint8_t
                   {
        // bit 0 = RxRDY, bit 1 = TxRDY, bit 7 = DSR/CD
        uint8_t s = 0x02; // TxRDY always set
        if (g_serialState && g_serialState->serial.connected())
        {
            s |= 0x80; // DSR/CD — caller present
            if (!g_serialState->serial.rxBuf.empty())
                s |= 0x01; // RxRDY
        }
        return s; });
    RegisterPortOut(0x10, [](intel8080 *cpu)
                    {
        if (g_serialState && g_serialState->serial.connected())
            g_serialState->serial.txBuf.push_back(cpu->A); });
    RegisterPortOut(0x11, [](intel8080 *) {}); // command/mode write — ignored
}

// Dispatch
uint8_t DispatchPortIn(intel8080 *cpu, uint8_t port)
{
    return g_portIn[port] ? g_portIn[port](cpu) : cpu->IOPorts[port];
}

void DispatchPortOut(intel8080 *cpu, uint8_t port, uint8_t value)
{
    uint8_t savedA = cpu->A;
    cpu->A = value;
    if (g_portOut[port])
        g_portOut[port](cpu);
    else
        cpu->IOPorts[port] = value;
    cpu->A = savedA;
}

void MachineIn(intel8080 *cpu)
{
    uint8_t port = AdvanceByte(cpu);
    cpu->A = cpu->InPort8(port);
}

void MachineOut(intel8080 *cpu)
{
    uint8_t port = AdvanceByte(cpu);
    cpu->OutPort8(port, cpu->A);
}
