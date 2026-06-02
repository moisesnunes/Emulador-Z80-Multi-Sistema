#include <cstdio>
#include "zilogZ80.h"
#include "intel8080.h"
#include "alu.h"
#include "cpm_bios.h"

static uint16_t Z80GetHL(intel8080 *cpu)
{
    return (cpu->H << 8) | cpu->L;
}

static uint16_t Z80GetPair(zilogZ80 *cpu, uint8_t pair)
{
    switch (pair)
    {
    case 0:
        return GetBC(cpu);
    case 1:
        return GetDE(cpu);
    case 2:
        return Z80GetHL(cpu);
    case 3:
        return cpu->SP;
    default:
        return 0;
    }
}

static void Z80SetPair(zilogZ80 *cpu, uint8_t pair, uint16_t value)
{
    switch (pair)
    {
    case 0:
        SetBC(cpu, value);
        break;
    case 1:
        SetDE(cpu, value);
        break;
    case 2:
        cpu->H = (value >> 8) & 0xFF;
        cpu->L = value & 0xFF;
        break;
    case 3:
        cpu->SP = value;
        break;
    }
}

static uint8_t *Z80RegRef(zilogZ80 *cpu, uint8_t reg)
{
    switch (reg)
    {
    case 0:
        return &cpu->B;
    case 1:
        return &cpu->C;
    case 2:
        return &cpu->D;
    case 3:
        return &cpu->E;
    case 4:
        return &cpu->H;
    case 5:
        return &cpu->L;
    case 7:
        return &cpu->A;
    default:
        return nullptr;
    }
}

static uint8_t Z80ReadRegOrMem(zilogZ80 *cpu, uint8_t reg)
{
    if (reg == 6)
        return ReadMemoryHL(cpu);
    return *Z80RegRef(cpu, reg);
}

static void Z80WriteRegOrMem(zilogZ80 *cpu, uint8_t reg, uint8_t value)
{
    if (reg == 6)
        cpu->WriteMem(Z80GetHL(cpu), value);
    else
        *Z80RegRef(cpu, reg) = value;
}

// Z80 F register: S Z Y H X P/V N C  (bits 7..0)
static uint8_t Z80PackF(zilogZ80 *cpu)
{
    return ((uint8_t)cpu->sf << 7) |
           ((uint8_t)cpu->zf << 6) |
           ((uint8_t)cpu->yf << 5) |
           ((uint8_t)cpu->acf << 4) |
           ((uint8_t)cpu->xf << 3) |
           ((uint8_t)cpu->pf << 2) |
           ((uint8_t)cpu->nf << 1) |
           ((uint8_t)cpu->cf << 0);
}

static void Z80UnpackF(zilogZ80 *cpu, uint8_t f)
{
    cpu->sf = (f >> 7) & 1;
    cpu->zf = (f >> 6) & 1;
    cpu->yf = (f >> 5) & 1;
    cpu->acf = (f >> 4) & 1;
    cpu->xf = (f >> 3) & 1;
    cpu->pf = (f >> 2) & 1;
    cpu->nf = (f >> 1) & 1;
    cpu->cf = (f >> 0) & 1;
}

static void Z80SetFlagsShift(zilogZ80 *cpu, uint8_t result, bool carry)
{
    cpu->Set_SZP_Flags(result);
    cpu->yf = (result >> 5) & 1;
    cpu->xf = (result >> 3) & 1;
    cpu->acf = false;
    cpu->nf = false;
    cpu->cf = carry;
}

static void Z80SetFlagsBit(zilogZ80 *cpu, uint8_t value, uint8_t bit)
{
    bool oldCarry = cpu->cf;
    uint8_t mask = 1u << bit;
    uint8_t test = value & mask;
    cpu->Set_SZP_Flags(test);
    cpu->acf = true;
    cpu->nf = false;
    cpu->cf = oldCarry;
}

static uint8_t Z80RLC(uint8_t value, bool &carry)
{
    carry = (value & 0x80) != 0;
    return (value << 1) | (carry ? 1 : 0);
}

static uint8_t Z80RRC(uint8_t value, bool &carry)
{
    carry = (value & 0x01) != 0;
    return (value >> 1) | (carry ? 0x80 : 0);
}

static uint8_t Z80RL(uint8_t value, bool carryIn, bool &carryOut)
{
    carryOut = (value & 0x80) != 0;
    return (value << 1) | (carryIn ? 1 : 0);
}

static uint8_t Z80RR(uint8_t value, bool carryIn, bool &carryOut)
{
    carryOut = (value & 0x01) != 0;
    return (value >> 1) | (carryIn ? 0x80 : 0);
}

static uint8_t Z80SLA(uint8_t value, bool &carry)
{
    carry = (value & 0x80) != 0;
    return value << 1;
}

static uint8_t Z80SRA(uint8_t value, bool &carry)
{
    carry = (value & 0x01) != 0;
    return (value >> 1) | (value & 0x80);
}

static uint8_t Z80SRL(uint8_t value, bool &carry)
{
    carry = (value & 0x01) != 0;
    return value >> 1;
}

static uint8_t Z80SLL(uint8_t value, bool &carry)
{
    carry = (value & 0x80) != 0;
    return (value << 1) | 0x01;
}

static uint8_t Z80ReadPort(zilogZ80 *cpu, uint8_t port)
{
    return cpu->InPort8(port);
}

static void Z80WritePort(zilogZ80 *cpu, uint8_t port, uint8_t value)
{
    cpu->OutPort8(port, value);
}

static void Z80InRegisterFromC(zilogZ80 *cpu, uint8_t reg)
{
    uint8_t value = Z80ReadPort(cpu, cpu->C);
    Z80WriteRegOrMem(cpu, reg, value);
    cpu->acf = false;
    cpu->nf = false;
    cpu->Set_SZP_Flags(value);
}

static void Z80OutCRegister(zilogZ80 *cpu, uint8_t reg)
{
    uint8_t value = Z80ReadRegOrMem(cpu, reg);
    Z80WritePort(cpu, cpu->C, value);
}

// Used by LDI/LDD/LDIR/LDDR: H=0, N=0, P=(BC!=0)
static void Z80AdjustLDFlags(zilogZ80 *cpu, uint16_t bc)
{
    cpu->acf = false;
    cpu->nf = false;
    cpu->pf = (bc != 0);
}

static void Z80BlockCopy(zilogZ80 *cpu, bool increment)
{
    uint16_t hl = Z80GetHL(cpu);
    uint16_t de = GetDE(cpu);
    uint8_t value = cpu->ReadMem8(hl);
    cpu->WriteMem(de, value);
    uint16_t bc = GetBC(cpu);
    bc -= 1;
    SetBC(cpu, bc);
    if (increment)
    {
        hl += 1;
        de += 1;
    }
    else
    {
        hl -= 1;
        de -= 1;
    }
    cpu->H = (hl >> 8) & 0xFF;
    cpu->L = hl & 0xFF;
    cpu->D = (de >> 8) & 0xFF;
    cpu->E = de & 0xFF;
    Z80AdjustLDFlags(cpu, bc);
}

static void Z80BlockCompare(zilogZ80 *cpu, bool increment)
{
    uint16_t hl = Z80GetHL(cpu);
    uint8_t value = cpu->ReadMem8(hl);
    uint8_t temp = cpu->A;
    bool old_cf = cpu->cf;
    SubReg(cpu, &temp, value, 0); // sets S,Z,H from A-value; we restore CF below
    bool sub_hf = cpu->acf;
    uint8_t undocumented = temp - (sub_hf ? 1 : 0);

    cpu->cf = old_cf; // CPI/CPD don't affect C
    cpu->acf = sub_hf;
    cpu->nf = true; // N=1 for compare
    cpu->yf = (undocumented >> 5) & 1;
    cpu->xf = (undocumented >> 3) & 1;

    uint16_t bc = GetBC(cpu);
    bc -= 1;
    SetBC(cpu, bc);
    cpu->pf = (bc != 0); // P = BC != 0

    if (increment)
        hl += 1;
    else
        hl -= 1;

    cpu->H = (hl >> 8) & 0xFF;
    cpu->L = hl & 0xFF;
}

static void Z80RepeatIfNonZero(zilogZ80 *cpu)
{
    if (GetBC(cpu) != 0)
        cpu->PC -= 2;
}

static void Z80RepeatCompare(zilogZ80 *cpu)
{
    if (GetBC(cpu) != 0 && !cpu->zf)
        cpu->PC -= 2;
}

// ADD HL, rr — only CF, HF, N affected (S/Z/P/V unchanged)
static void Z80AddHLPlain(zilogZ80 *cpu, uint16_t value)
{
    uint32_t hl = Z80GetHL(cpu);
    uint32_t sum = hl + value;
    cpu->cf = (sum >> 16) & 1;
    uint16_t result = sum & 0xFFFF;
    cpu->H = (result >> 8) & 0xFF;
    cpu->L = result & 0xFF;
    cpu->acf = (((hl ^ value ^ sum) & 0x1000) != 0); // H: carry bit 11→12
    cpu->nf = false;
}

// ADC HL, rr — all flags affected
static void Z80AddHL(zilogZ80 *cpu, uint16_t value, bool carry)
{
    uint32_t hl = Z80GetHL(cpu);
    uint32_t sum = hl + value + (carry ? 1u : 0u);
    cpu->cf = (sum >> 16) & 0x01;
    uint16_t result = sum & 0xFFFF;
    cpu->H = (result >> 8) & 0xFF;
    cpu->L = result & 0xFF;
    cpu->acf = (((hl ^ value ^ sum) & 0x1000) != 0);
    bool signHl = (hl & 0x8000) != 0;
    bool signVal = (value & 0x8000) != 0;
    bool signRes = (result & 0x8000) != 0;
    cpu->pf = (signHl == signVal) && (signRes != signHl);
    cpu->sf = (result >> 15) & 1;
    cpu->zf = (result == 0);
    cpu->nf = false;
}

// SBC HL, rr — all flags affected
static void Z80SubHL(zilogZ80 *cpu, uint16_t value, bool carry)
{
    uint32_t hl = Z80GetHL(cpu);
    uint32_t sub = hl - value - (carry ? 1u : 0u);
    cpu->cf = (sub >> 16) & 0x01;
    uint16_t result = sub & 0xFFFF;
    cpu->H = (result >> 8) & 0xFF;
    cpu->L = result & 0xFF;
    cpu->acf = (((hl ^ value ^ result) & 0x1000) != 0);
    bool signHl = (hl & 0x8000) != 0;
    bool signVal = (value & 0x8000) != 0;
    bool signRes = (result & 0x8000) != 0;
    cpu->pf = (signHl != signVal) && (signRes != signHl);
    cpu->sf = (result >> 15) & 1;
    cpu->zf = (result == 0);
    cpu->nf = true;
}

static void Z80RotateMemoryOrRegister(zilogZ80 *cpu, uint8_t code)
{
    uint8_t reg = code & 0x07;
    uint8_t op = (code >> 3) & 0x07;
    uint8_t value = Z80ReadRegOrMem(cpu, reg);
    uint8_t result = value;
    bool carry = false;

    switch (op)
    {
    case 0:
        result = Z80RLC(value, carry);
        break;
    case 1:
        result = Z80RRC(value, carry);
        break;
    case 2:
        result = Z80RL(value, cpu->cf, carry);
        break;
    case 3:
        result = Z80RR(value, cpu->cf, carry);
        break;
    case 4:
        result = Z80SLA(value, carry);
        break;
    case 5:
        result = Z80SRA(value, carry);
        break;
    case 6:
        result = Z80SLL(value, carry);
        break;
    case 7:
        result = Z80SRL(value, carry);
        break;
    }

    Z80WriteRegOrMem(cpu, reg, result);
    Z80SetFlagsShift(cpu, result, carry);
}

static void Z80ProcessCB(uint8_t code, zilogZ80 *cpu)
{
    if (code < 0x40)
    {
        Z80RotateMemoryOrRegister(cpu, code);
        return;
    }

    uint8_t reg = code & 0x07;
    uint8_t op = (code >> 3) & 0x07;
    uint8_t value = Z80ReadRegOrMem(cpu, reg);

    if (code < 0x80)
    {
        Z80SetFlagsBit(cpu, value, op);
        return;
    }

    uint8_t result;
    if (code < 0xC0)
    {
        result = value & ~(1u << op);
        Z80WriteRegOrMem(cpu, reg, result);
    }
    else
    {
        result = value | (1u << op);
        Z80WriteRegOrMem(cpu, reg, result);
    }
}

static void Z80PrefixED(zilogZ80 *cpu)
{
    uint8_t code = AdvanceByte(cpu);
    switch (code)
    {
    case 0x40:
    case 0x48:
    case 0x50:
    case 0x58:
    case 0x60:
    case 0x68:
    case 0x78:
    {
        uint8_t reg = (code - 0x40) / 8;
        Z80InRegisterFromC(cpu, reg);
        return;
    }
    case 0x41:
    case 0x49:
    case 0x51:
    case 0x59:
    case 0x61:
    case 0x69:
    case 0x79:
    {
        uint8_t reg = (code - 0x41) / 8;
        Z80OutCRegister(cpu, reg);
        return;
    }
    case 0x70:
        Z80ReadPort(cpu, cpu->C);
        return;
    case 0x71:
        Z80WritePort(cpu, cpu->C, 0);
        return;
    case 0x42:
        Z80SubHL(cpu, GetBC(cpu), cpu->cf);
        return;
    case 0x4A:
        Z80AddHL(cpu, GetBC(cpu), cpu->cf);
        return;
    case 0x52:
        Z80SubHL(cpu, GetDE(cpu), cpu->cf);
        return;
    case 0x5A:
        Z80AddHL(cpu, GetDE(cpu), cpu->cf);
        return;
    case 0x62:
        Z80SubHL(cpu, Z80GetHL(cpu), cpu->cf);
        return;
    case 0x6A:
        Z80AddHL(cpu, Z80GetHL(cpu), cpu->cf);
        return;
    case 0x72:
        Z80SubHL(cpu, cpu->SP, cpu->cf);
        return;
    case 0x7A:
        Z80AddHL(cpu, cpu->SP, cpu->cf);
        return;
    case 0x43:
    {
        uint16_t addr = AdvanceWord(cpu);
        cpu->WriteMem(addr, cpu->C);
        cpu->WriteMem(addr + 1, cpu->B);
        return;
    }
    case 0x4B:
    {
        uint16_t addr = AdvanceWord(cpu);
        uint16_t value = cpu->ReadMem16(addr);
        SetBC(cpu, value);
        return;
    }
    case 0x53:
    {
        uint16_t addr = AdvanceWord(cpu);
        cpu->WriteMem(addr, cpu->E);
        cpu->WriteMem(addr + 1, cpu->D);
        return;
    }
    case 0x5B:
    {
        uint16_t addr = AdvanceWord(cpu);
        uint16_t value = cpu->ReadMem16(addr);
        SetDE(cpu, value);
        return;
    }
    case 0x63:
    {
        uint16_t addr = AdvanceWord(cpu);
        cpu->WriteMem(addr, cpu->L);
        cpu->WriteMem(addr + 1, cpu->H);
        return;
    }
    case 0x6B:
    {
        uint16_t addr = AdvanceWord(cpu);
        uint16_t value = cpu->ReadMem16(addr);
        cpu->H = (value >> 8) & 0xFF;
        cpu->L = value & 0xFF;
        return;
    }
    case 0x73:
    {
        uint16_t addr = AdvanceWord(cpu);
        cpu->WriteMem(addr, cpu->SP & 0xFF);
        cpu->WriteMem(addr + 1, (cpu->SP >> 8) & 0xFF);
        return;
    }
    case 0x7B:
    {
        uint16_t addr = AdvanceWord(cpu);
        uint16_t value = cpu->ReadMem16(addr);
        cpu->SP = value;
        return;
    }
    case 0x44: // NEG
    {
        uint8_t original = cpu->A;
        cpu->A = 0;
        SubReg(cpu, &cpu->A, original, 0);
        cpu->nf = true;
        return;
    }
    case 0x45:
        RET(cpu);
        cpu->IFF1 = cpu->IFF2;
        cpu->interrupts = cpu->IFF1;
        return;
    case 0x4D:
        RET(cpu);
        cpu->IFF1 = cpu->IFF2;
        cpu->interrupts = cpu->IFF1;
        return;
    case 0x46:
        cpu->IM = 0;
        return;
    case 0x56:
        cpu->IM = 1;
        return;
    case 0x5E:
        cpu->IM = 2;
        return;
    case 0x47:
        cpu->I = cpu->A;
        return;
    case 0x4F:
        cpu->R = cpu->A;
        return;
    case 0x57: // LD A, I
        cpu->A = cpu->I;
        cpu->Set_SZP_Flags(cpu->A);
        cpu->acf = false;
        cpu->nf = false;
        cpu->pf = cpu->IFF2; // P/V = IFF2
        cpu->yf = (cpu->A >> 5) & 1;
        cpu->xf = (cpu->A >> 3) & 1;
        return;
    case 0x5F: // LD A, R
        cpu->A = cpu->R;
        cpu->Set_SZP_Flags(cpu->A);
        cpu->acf = false;
        cpu->nf = false;
        cpu->pf = cpu->IFF2; // P/V = IFF2
        cpu->yf = (cpu->A >> 5) & 1;
        cpu->xf = (cpu->A >> 3) & 1;
        return;
    case 0x6F: // RLD
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t m = cpu->ReadMem8(hl);
        uint8_t new_m = (uint8_t)((m << 4) | (cpu->A & 0x0F));
        cpu->A = (cpu->A & 0xF0) | (m >> 4);
        cpu->WriteMem(hl, new_m);
        cpu->Set_SZP_Flags(cpu->A);
        cpu->yf = (cpu->A >> 5) & 1;
        cpu->xf = (cpu->A >> 3) & 1;
        cpu->acf = false;
        cpu->nf = false;
        return;
    }
    case 0x67: // RRD
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t m = cpu->ReadMem8(hl);
        uint8_t new_m = (uint8_t)(((cpu->A & 0x0F) << 4) | (m >> 4));
        cpu->A = (cpu->A & 0xF0) | (m & 0x0F);
        cpu->WriteMem(hl, new_m);
        cpu->Set_SZP_Flags(cpu->A);
        cpu->yf = (cpu->A >> 5) & 1;
        cpu->xf = (cpu->A >> 3) & 1;
        cpu->acf = false;
        cpu->nf = false;
        return;
    }
    case 0xA0:
        Z80BlockCopy(cpu, true);
        return;
    case 0xA8:
        Z80BlockCopy(cpu, false);
        return;
    case 0xB0:
        Z80BlockCopy(cpu, true);
        Z80RepeatIfNonZero(cpu);
        return;
    case 0xB8:
        Z80BlockCopy(cpu, false);
        Z80RepeatIfNonZero(cpu);
        return;
    case 0xA1:
        Z80BlockCompare(cpu, true);
        return;
    case 0xA9:
        Z80BlockCompare(cpu, false);
        return;
    case 0xB1:
        Z80BlockCompare(cpu, true);
        Z80RepeatCompare(cpu);
        return;
    case 0xB9:
        Z80BlockCompare(cpu, false);
        Z80RepeatCompare(cpu);
        return;
    case 0xA2: // INI
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t value = Z80ReadPort(cpu, cpu->C);
        cpu->WriteMem(hl, value);
        hl += 1;
        cpu->H = (hl >> 8) & 0xFF;
        cpu->L = hl & 0xFF;
        cpu->B -= 1;
        cpu->zf = (cpu->B == 0);
        cpu->nf = true;
        return;
    }
    case 0xAA: // IND
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t value = Z80ReadPort(cpu, cpu->C);
        cpu->WriteMem(hl, value);
        hl -= 1;
        cpu->H = (hl >> 8) & 0xFF;
        cpu->L = hl & 0xFF;
        cpu->B -= 1;
        cpu->zf = (cpu->B == 0);
        cpu->nf = true;
        return;
    }
    case 0xB2: // INIR
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t value = Z80ReadPort(cpu, cpu->C);
        cpu->WriteMem(hl, value);
        hl += 1;
        cpu->H = (hl >> 8) & 0xFF;
        cpu->L = hl & 0xFF;
        cpu->B -= 1;
        cpu->zf = true;
        cpu->nf = true;
        if (cpu->B != 0)
            cpu->PC -= 2;
        return;
    }
    case 0xBA: // INDR
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t value = Z80ReadPort(cpu, cpu->C);
        cpu->WriteMem(hl, value);
        hl -= 1;
        cpu->H = (hl >> 8) & 0xFF;
        cpu->L = hl & 0xFF;
        cpu->B -= 1;
        cpu->zf = true;
        cpu->nf = true;
        if (cpu->B != 0)
            cpu->PC -= 2;
        return;
    }
    case 0xA3: // OUTI
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t value = cpu->ReadMem8(hl);
        Z80WritePort(cpu, cpu->C, value);
        hl += 1;
        cpu->H = (hl >> 8) & 0xFF;
        cpu->L = hl & 0xFF;
        cpu->B -= 1;
        cpu->zf = (cpu->B == 0);
        cpu->nf = true;
        return;
    }
    case 0xAB: // OUTD
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t value = cpu->ReadMem8(hl);
        Z80WritePort(cpu, cpu->C, value);
        hl -= 1;
        cpu->H = (hl >> 8) & 0xFF;
        cpu->L = hl & 0xFF;
        cpu->B -= 1;
        cpu->zf = (cpu->B == 0);
        cpu->nf = true;
        return;
    }
    case 0xB3: // OTIR
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t value = cpu->ReadMem8(hl);
        Z80WritePort(cpu, cpu->C, value);
        hl += 1;
        cpu->H = (hl >> 8) & 0xFF;
        cpu->L = hl & 0xFF;
        cpu->B -= 1;
        cpu->zf = true;
        cpu->nf = true;
        if (cpu->B != 0)
            cpu->PC -= 2;
        return;
    }
    case 0xBB: // OTDR
    {
        uint16_t hl = Z80GetHL(cpu);
        uint8_t value = cpu->ReadMem8(hl);
        Z80WritePort(cpu, cpu->C, value);
        hl -= 1;
        cpu->H = (hl >> 8) & 0xFF;
        cpu->L = hl & 0xFF;
        cpu->B -= 1;
        cpu->zf = true;
        cpu->nf = true;
        if (cpu->B != 0)
            cpu->PC -= 2;
        return;
    }
    default:
        return;
    }
}

static uint8_t Z80IRH(uint16_t ir) { return (ir >> 8) & 0xFF; }
static uint8_t Z80IRL(uint16_t ir) { return ir & 0xFF; }
static void Z80SetIRH(uint16_t &ir, uint8_t v) { ir = (ir & 0x00FF) | ((uint16_t)v << 8); }
static void Z80SetIRL(uint16_t &ir, uint8_t v) { ir = (ir & 0xFF00) | v; }

// ADD IX/IY, rr — only CF, HF, N affected (S/Z/P/V unchanged)
static void Z80AddIR(zilogZ80 *cpu, uint16_t &ir, uint16_t value)
{
    uint32_t sum = (uint32_t)ir + value;
    cpu->cf = (sum >> 16) & 1;
    cpu->acf = ((ir ^ value ^ sum) & 0x1000) != 0;
    cpu->nf = false;
    ir = sum & 0xFFFF;
}

static void Z80ProcessDDCB(zilogZ80 *cpu, uint16_t addr)
{
    uint8_t op = AdvanceByte(cpu);
    uint8_t value = cpu->ReadMem8(addr);
    uint8_t result = value;
    bool carry = false;

    if (op < 0x40)
    {
        switch ((op >> 3) & 0x07)
        {
        case 0:
            result = Z80RLC(value, carry);
            break;
        case 1:
            result = Z80RRC(value, carry);
            break;
        case 2:
            result = Z80RL(value, cpu->cf, carry);
            break;
        case 3:
            result = Z80RR(value, cpu->cf, carry);
            break;
        case 4:
            result = Z80SLA(value, carry);
            break;
        case 5:
            result = Z80SRA(value, carry);
            break;
        case 6:
            result = Z80SLL(value, carry);
            break;
        case 7:
            result = Z80SRL(value, carry);
            break;
        }
        cpu->WriteMem(addr, result);
        Z80SetFlagsShift(cpu, result, carry);
        uint8_t reg = op & 0x07;
        if (reg != 6)
            Z80WriteRegOrMem(cpu, reg, result);
        return;
    }

    uint8_t bit = (op >> 3) & 0x07;
    if (op < 0x80)
    {
        Z80SetFlagsBit(cpu, value, bit);
        return;
    }
    result = (op < 0xC0) ? (value & ~(1u << bit)) : (value | (1u << bit));
    cpu->WriteMem(addr, result);
    uint8_t reg = op & 0x07;
    if (reg != 6)
        Z80WriteRegOrMem(cpu, reg, result);
}

static void Z80PrefixDDFD(zilogZ80 *cpu, uint16_t &IR)
{
    uint8_t code = AdvanceByte(cpu);

    switch (code)
    {
    // ADD IR, rr
    case 0x09:
        Z80AddIR(cpu, IR, GetBC(cpu));
        return;
    case 0x19:
        Z80AddIR(cpu, IR, GetDE(cpu));
        return;
    case 0x29:
        Z80AddIR(cpu, IR, IR);
        return;
    case 0x39:
        Z80AddIR(cpu, IR, cpu->SP);
        return;

    // LD IR, nn
    case 0x21:
        IR = AdvanceWord(cpu);
        return;

    // LD (nn), IR
    case 0x22:
    {
        uint16_t addr = AdvanceWord(cpu);
        cpu->WriteMem(addr, IR & 0xFF);
        cpu->WriteMem(addr + 1, (IR >> 8) & 0xFF);
        return;
    }

    case 0x23:
        IR++;
        return; // INC IR
    case 0x2B:
        IR--;
        return; // DEC IR

    // INC/DEC IRH/IRL (undocumented)
    case 0x24:
        Z80SetIRH(IR, INR(cpu, Z80IRH(IR)));
        return;
    case 0x25:
        Z80SetIRH(IR, DCR(cpu, Z80IRH(IR)));
        return;
    case 0x26:
        Z80SetIRH(IR, AdvanceByte(cpu));
        return;
    case 0x2C:
        Z80SetIRL(IR, INR(cpu, Z80IRL(IR)));
        return;
    case 0x2D:
        Z80SetIRL(IR, DCR(cpu, Z80IRL(IR)));
        return;
    case 0x2E:
        Z80SetIRL(IR, AdvanceByte(cpu));
        return;

    // LD IR, (nn)
    case 0x2A:
    {
        uint16_t addr = AdvanceWord(cpu);
        IR = cpu->ReadMem16(addr);
        return;
    }

    // INC (IR+d)
    case 0x34:
    {
        uint16_t addr = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(addr, INR(cpu, cpu->ReadMem8(addr)));
        return;
    }

    // DEC (IR+d)
    case 0x35:
    {
        uint16_t addr = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(addr, DCR(cpu, cpu->ReadMem8(addr)));
        return;
    }

    // LD (IR+d), n
    case 0x36:
    {
        uint16_t addr = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(addr, AdvanceByte(cpu));
        return;
    }

    // LD r, IRH / LD r, IRL / LD r, (IR+d) — B, C, D, E
    case 0x44:
        cpu->B = Z80IRH(IR);
        return;
    case 0x45:
        cpu->B = Z80IRL(IR);
        return;
    case 0x46:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->B = cpu->ReadMem8(a);
        return;
    }
    case 0x4C:
        cpu->C = Z80IRH(IR);
        return;
    case 0x4D:
        cpu->C = Z80IRL(IR);
        return;
    case 0x4E:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->C = cpu->ReadMem8(a);
        return;
    }
    case 0x54:
        cpu->D = Z80IRH(IR);
        return;
    case 0x55:
        cpu->D = Z80IRL(IR);
        return;
    case 0x56:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->D = cpu->ReadMem8(a);
        return;
    }
    case 0x5C:
        cpu->E = Z80IRH(IR);
        return;
    case 0x5D:
        cpu->E = Z80IRL(IR);
        return;
    case 0x5E:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->E = cpu->ReadMem8(a);
        return;
    }

    // LD IRH, r / LD H, (IR+d) (undocumented)
    case 0x60:
        Z80SetIRH(IR, cpu->B);
        return;
    case 0x61:
        Z80SetIRH(IR, cpu->C);
        return;
    case 0x62:
        Z80SetIRH(IR, cpu->D);
        return;
    case 0x63:
        Z80SetIRH(IR, cpu->E);
        return;
    case 0x64:
        return; // LD IRH, IRH — NOP
    case 0x65:
        Z80SetIRH(IR, Z80IRL(IR));
        return;
    case 0x66:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->H = cpu->ReadMem8(a);
        return;
    }
    case 0x67:
        Z80SetIRH(IR, cpu->A);
        return;

    // LD IRL, r / LD L, (IR+d) (undocumented)
    case 0x68:
        Z80SetIRL(IR, cpu->B);
        return;
    case 0x69:
        Z80SetIRL(IR, cpu->C);
        return;
    case 0x6A:
        Z80SetIRL(IR, cpu->D);
        return;
    case 0x6B:
        Z80SetIRL(IR, cpu->E);
        return;
    case 0x6C:
        Z80SetIRL(IR, Z80IRH(IR));
        return;
    case 0x6D:
        return; // LD IRL, IRL — NOP
    case 0x6E:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->L = cpu->ReadMem8(a);
        return;
    }
    case 0x6F:
        Z80SetIRL(IR, cpu->A);
        return;

    // LD (IR+d), r
    case 0x70:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(a, cpu->B);
        return;
    }
    case 0x71:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(a, cpu->C);
        return;
    }
    case 0x72:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(a, cpu->D);
        return;
    }
    case 0x73:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(a, cpu->E);
        return;
    }
    case 0x74:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(a, cpu->H);
        return;
    }
    case 0x75:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(a, cpu->L);
        return;
    }
    case 0x77:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->WriteMem(a, cpu->A);
        return;
    }

    // LD A, IRH / LD A, IRL / LD A, (IR+d)
    case 0x7C:
        cpu->A = Z80IRH(IR);
        return;
    case 0x7D:
        cpu->A = Z80IRL(IR);
        return;
    case 0x7E:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        cpu->A = cpu->ReadMem8(a);
        return;
    }

    // ADD A / ADC A / SUB / SBC / AND / XOR / OR / CP — IRH, IRL, (IR+d)
    case 0x84:
        AddReg(cpu, &cpu->A, Z80IRH(IR), 0);
        return;
    case 0x85:
        AddReg(cpu, &cpu->A, Z80IRL(IR), 0);
        return;
    case 0x86:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        AddReg(cpu, &cpu->A, cpu->ReadMem8(a), 0);
        return;
    }
    case 0x8C:
        AddReg(cpu, &cpu->A, Z80IRH(IR), cpu->cf);
        return;
    case 0x8D:
        AddReg(cpu, &cpu->A, Z80IRL(IR), cpu->cf);
        return;
    case 0x8E:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        AddReg(cpu, &cpu->A, cpu->ReadMem8(a), cpu->cf);
        return;
    }
    case 0x94:
        SubReg(cpu, &cpu->A, Z80IRH(IR), 0);
        return;
    case 0x95:
        SubReg(cpu, &cpu->A, Z80IRL(IR), 0);
        return;
    case 0x96:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        SubReg(cpu, &cpu->A, cpu->ReadMem8(a), 0);
        return;
    }
    case 0x9C:
        SubReg(cpu, &cpu->A, Z80IRH(IR), cpu->cf);
        return;
    case 0x9D:
        SubReg(cpu, &cpu->A, Z80IRL(IR), cpu->cf);
        return;
    case 0x9E:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        SubReg(cpu, &cpu->A, cpu->ReadMem8(a), cpu->cf);
        return;
    }
    case 0xA4:
        ANA(cpu, &cpu->A, Z80IRH(IR));
        return;
    case 0xA5:
        ANA(cpu, &cpu->A, Z80IRL(IR));
        return;
    case 0xA6:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        ANA(cpu, &cpu->A, cpu->ReadMem8(a));
        return;
    }
    case 0xAC:
        XRA(cpu, &cpu->A, Z80IRH(IR));
        return;
    case 0xAD:
        XRA(cpu, &cpu->A, Z80IRL(IR));
        return;
    case 0xAE:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        XRA(cpu, &cpu->A, cpu->ReadMem8(a));
        return;
    }
    case 0xB4:
        ORA(cpu, &cpu->A, Z80IRH(IR));
        return;
    case 0xB5:
        ORA(cpu, &cpu->A, Z80IRL(IR));
        return;
    case 0xB6:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        ORA(cpu, &cpu->A, cpu->ReadMem8(a));
        return;
    }
    case 0xBC:
        CMP(cpu, cpu->A, Z80IRH(IR));
        return;
    case 0xBD:
        CMP(cpu, cpu->A, Z80IRL(IR));
        return;
    case 0xBE:
    {
        uint16_t a = IR + (int8_t)AdvanceByte(cpu);
        CMP(cpu, cpu->A, cpu->ReadMem8(a));
        return;
    }

    // DDCB / FDCB — displacement d comes before the operation byte
    case 0xCB:
    {
        uint16_t addr = IR + (int8_t)AdvanceByte(cpu);
        Z80ProcessDDCB(cpu, addr);
        return;
    }

    // POP IR
    case 0xE1:
    {
        uint8_t lo = cpu->ReadMem8(cpu->SP++);
        uint8_t hi = cpu->ReadMem8(cpu->SP++);
        IR = lo | ((uint16_t)hi << 8);
        return;
    }

    // EX (SP), IR
    case 0xE3:
    {
        uint8_t lo = cpu->ReadMem8(cpu->SP);
        uint8_t hi = cpu->ReadMem8((uint16_t)(cpu->SP + 1));
        cpu->WriteMem(cpu->SP, IR & 0xFF);
        cpu->WriteMem(cpu->SP + 1, (IR >> 8) & 0xFF);
        IR = lo | ((uint16_t)hi << 8);
        return;
    }

    // PUSH IR
    case 0xE5:
        cpu->WriteMem(--cpu->SP, (IR >> 8) & 0xFF);
        cpu->WriteMem(--cpu->SP, IR & 0xFF);
        return;

    // JP (IR)
    case 0xE9:
        cpu->PC = IR;
        return;

    // LD SP, IR
    case 0xF9:
        cpu->SP = IR;
        return;

    default:
        // Unrecognized DD/FD opcode — fall through to base 8080 execution
        ExecuteOpCode(code, cpu);
        return;
    }
}

zilogZ80::zilogZ80()
    : I(0), R(0), IX(0), IY(0), IM(0), IFF1(false), IFF2(false), prefixState(Z80Prefix::None),
      A_(0), B_(0), C_(0), D_(0), E_(0), H_(0), L_(0),
      sf_(false), zf_(false), acf_(false), pf_(false), cf_(false), yf_(false), xf_(false), nf_(false),
      yf(false), xf(false), nf(false), bus(nullptr)
{
    // Base intel8080 constructor already initialises registers, memory, flags and state.
    z80F = true;
}

void zilogZ80::AttachBus(core::Z80Bus *newBus)
{
    bus = newBus;
}

uint8_t zilogZ80::ReadMem8(uint16_t address)
{
    return bus ? bus->read(address) : intel8080::ReadMem8(address);
}

uint16_t zilogZ80::ReadMem16(uint16_t address)
{
    return (uint16_t)ReadMem8(address) |
           ((uint16_t)ReadMem8((uint16_t)(address + 1)) << 8);
}

void zilogZ80::WriteMem(uint16_t address, uint16_t value)
{
    if (bus)
    {
        bus->write(address, (uint8_t)value);
        return;
    }
    intel8080::WriteMem(address, value);
}

uint8_t zilogZ80::InPort8(uint16_t port)
{
    return bus ? bus->in(port) : intel8080::InPort8(port);
}

void zilogZ80::OutPort8(uint16_t port, uint8_t value)
{
    if (bus)
    {
        bus->out(port, value);
        return;
    }
    intel8080::OutPort8(port, value);
}

void zilogZ80::ResetZ80()
{
    this->A = this->B = this->C = this->D = this->E = this->H = this->L = 0;
    this->sf = this->zf = this->acf = this->pf = this->cf = false;
    this->halted = false;
    this->interrupts = false;
    this->cycles = 0;
    this->cyclesInterrupt = 0;
    this->I = 0;
    this->R = 0;
    this->IX = 0;
    this->IY = 0;
    this->IM = 0;
    this->IFF1 = false;
    this->IFF2 = false;
    this->prefixState = Z80Prefix::None;
    this->SP = 0;
    this->PC = 0;
    this->A_ = this->B_ = this->C_ = this->D_ = this->E_ = this->H_ = this->L_ = 0;
    this->sf_ = this->zf_ = this->acf_ = this->pf_ = this->cf_ = this->yf_ = this->xf_ = this->nf_ = false;
    this->yf = this->xf = this->nf = false;
    for (int i = 0; i < 256; i++)
        this->IOPorts[i] = 0;
    for (int i = 0; i < 0x10000; i++)
        this->memory[i] = 0;
}

bool zilogZ80::MaskableInterrupt(uint8_t dataBus)
{
    if (!IFF1)
        return false;

    halted = false;
    IFF1 = false;
    IFF2 = false;
    interrupts = false;
    prefixState = Z80Prefix::None;

    PushRegisterPair(this, (PC >> 8) & 0xff, PC & 0xff);
    if (IM == 2)
    {
        uint16_t vectorAddress =
            (static_cast<uint16_t>(I) << 8) | static_cast<uint16_t>(dataBus);
        PC = ReadMem16(vectorAddress);
        cycles += 19;
        cyclesInterrupt += 19;
        return true;
    }

    PC = 0x0038;
    cycles += 13;
    cyclesInterrupt += 13;
    return true;
}

void ExecuteZ80OpCode(uint8_t OpCode, zilogZ80 *cpu)
{
    if (cpu->prefixState != Z80Prefix::None)
    {
        switch (cpu->prefixState)
        {
        case Z80Prefix::CB:
            Z80ProcessCB(cpu->ReadMem8(cpu->PC), cpu);
            cpu->PC += 1;
            break;
        case Z80Prefix::ED:
            Z80PrefixED(cpu);
            break;
        case Z80Prefix::DD:
            Z80PrefixDDFD(cpu, cpu->IX);
            break;
        case Z80Prefix::FD:
            Z80PrefixDDFD(cpu, cpu->IY);
            break;
        default:
            break;
        }
        cpu->prefixState = Z80Prefix::None;
        return;
    }

    switch (OpCode)
    {
    case 0xCB:
        cpu->prefixState = Z80Prefix::CB;
        cpu->PC += 1;
        return;
    case 0xED:
        cpu->prefixState = Z80Prefix::ED;
        cpu->PC += 1;
        return;
    case 0xDD:
        cpu->prefixState = Z80Prefix::DD;
        cpu->PC += 1;
        return;
    case 0xFD:
        cpu->prefixState = Z80Prefix::FD;
        cpu->PC += 1;
        return;

    // PUSH AF — Z80 F format: S Z Y H X P/V N C
    case 0xF5:
        cpu->PC += 1;
        cpu->WriteMem(--cpu->SP, cpu->A);
        cpu->WriteMem(--cpu->SP, Z80PackF(cpu));
        return;

    // POP AF — Z80 F format
    case 0xF1:
    {
        cpu->PC += 1;
        uint8_t f = cpu->ReadMem8(cpu->SP++);
        cpu->A = cpu->ReadMem8(cpu->SP++);
        Z80UnpackF(cpu, f);
        return;
    }

    // HALT — keep PC on the byte after HALT and wait for an interrupt.
    case 0x76:
        cpu->PC += 1;
        cpu->halted = true;
        return;

    // DI/EI affect Z80 interrupt flip-flops, not only the legacy 8080 flag.
    case 0xF3:
        cpu->PC += 1;
        cpu->IFF1 = false;
        cpu->IFF2 = false;
        cpu->interrupts = false;
        return;

    case 0xFB:
        cpu->PC += 1;
        cpu->IFF1 = true;
        cpu->IFF2 = true;
        cpu->interrupts = true;
        return;

    // ADD HL, rr — sets H and N=0 only (S/Z/P/V unchanged)
    case 0x09:
        cpu->PC += 1;
        Z80AddHLPlain(cpu, GetBC(cpu));
        return;
    case 0x19:
        cpu->PC += 1;
        Z80AddHLPlain(cpu, GetDE(cpu));
        return;
    case 0x29:
        cpu->PC += 1;
        Z80AddHLPlain(cpu, Z80GetHL(cpu));
        return;
    case 0x39:
        cpu->PC += 1;
        Z80AddHLPlain(cpu, cpu->SP);
        return;

    // RLCA — Z80: N=0, H=0
    case 0x07:
    {
        cpu->PC += 1;
        bool c = (cpu->A & 0x80) != 0;
        cpu->A = (uint8_t)((cpu->A << 1) | (c ? 1 : 0));
        cpu->cf = c;
        cpu->acf = false;
        cpu->nf = false;
        return;
    }
    // RRCA — Z80: N=0, H=0
    case 0x0F:
    {
        cpu->PC += 1;
        bool c = (cpu->A & 0x01) != 0;
        cpu->A = (uint8_t)((cpu->A >> 1) | (c ? 0x80 : 0));
        cpu->cf = c;
        cpu->acf = false;
        cpu->nf = false;
        return;
    }
    // RLA — Z80: N=0, H=0
    case 0x17:
    {
        cpu->PC += 1;
        bool c = (cpu->A & 0x80) != 0;
        cpu->A = (uint8_t)((cpu->A << 1) | (cpu->cf ? 1 : 0));
        cpu->cf = c;
        cpu->acf = false;
        cpu->nf = false;
        return;
    }
    // RRA — Z80: N=0, H=0
    case 0x1F:
    {
        cpu->PC += 1;
        bool c = (cpu->A & 0x01) != 0;
        cpu->A = (uint8_t)((cpu->A >> 1) | (cpu->cf ? 0x80 : 0));
        cpu->cf = c;
        cpu->acf = false;
        cpu->nf = false;
        return;
    }

    // DAA — Z80 version: uses N flag to handle subtract mode
    case 0x27:
    {
        cpu->PC += 1;
        uint8_t old_a = cpu->A;
        bool old_n = cpu->nf;
        bool carry = cpu->cf;
        uint8_t correction = 0;
        int a = cpu->A;

        if (cpu->acf || (a & 0x0F) > 9)
            correction |= 0x06;
        if (cpu->cf || a > 0x99)
        {
            correction |= 0x60;
            carry = true;
        }

        a = old_n ? (a - correction) : (a + correction);
        cpu->cf = carry;
        cpu->A = (uint8_t)(a & 0xFF);
        cpu->acf = ((old_a ^ cpu->A) & 0x10) != 0;
        cpu->Set_SZP_Flags(cpu->A);
        cpu->yf = (cpu->A >> 5) & 1;
        cpu->xf = (cpu->A >> 3) & 1;
        // nf unchanged
        return;
    }

    // CPL — Z80: complement A, set H=1, N=1 (S/Z/P/V/C unchanged)
    case 0x2F:
        cpu->PC += 1;
        cpu->A = ~cpu->A;
        cpu->acf = true;
        cpu->nf = true;
        cpu->yf = (cpu->A >> 5) & 1;
        cpu->xf = (cpu->A >> 3) & 1;
        return;

    // SCF — Z80: C=1, H=0, N=0
    case 0x37:
        cpu->PC += 1;
        cpu->cf = true;
        cpu->acf = false;
        cpu->nf = false;
        cpu->yf = (cpu->A >> 5) & 1;
        cpu->xf = (cpu->A >> 3) & 1;
        return;

    // CCF — Z80: C=~C, H=old_C, N=0
    case 0x3F:
        cpu->PC += 1;
        cpu->acf = cpu->cf; // H = old carry
        cpu->nf = false;
        cpu->cf = !cpu->cf;
        cpu->yf = (cpu->A >> 5) & 1;
        cpu->xf = (cpu->A >> 3) & 1;
        return;

    case 0x08:
    {
        // EX AF, AF'
        cpu->PC += 1;
        uint8_t tmpA = cpu->A;
        cpu->A = cpu->A_;
        cpu->A_ = tmpA;
        bool tmpSF = cpu->sf;
        cpu->sf = cpu->sf_;
        cpu->sf_ = tmpSF;
        bool tmpZF = cpu->zf;
        cpu->zf = cpu->zf_;
        cpu->zf_ = tmpZF;
        bool tmpACF = cpu->acf;
        cpu->acf = cpu->acf_;
        cpu->acf_ = tmpACF;
        bool tmpPF = cpu->pf;
        cpu->pf = cpu->pf_;
        cpu->pf_ = tmpPF;
        bool tmpCF = cpu->cf;
        cpu->cf = cpu->cf_;
        cpu->cf_ = tmpCF;
        bool tmpYF = cpu->yf;
        cpu->yf = cpu->yf_;
        cpu->yf_ = tmpYF;
        bool tmpXF = cpu->xf;
        cpu->xf = cpu->xf_;
        cpu->xf_ = tmpXF;
        bool tmpNF = cpu->nf;
        cpu->nf = cpu->nf_;
        cpu->nf_ = tmpNF;
        return;
    }

    case 0xD9:
    {
        // EXX — swap BC, DE, HL with their shadow counterparts
        cpu->PC += 1;
        uint8_t tmpB = cpu->B;
        cpu->B = cpu->B_;
        cpu->B_ = tmpB;
        uint8_t tmpC = cpu->C;
        cpu->C = cpu->C_;
        cpu->C_ = tmpC;
        uint8_t tmpD = cpu->D;
        cpu->D = cpu->D_;
        cpu->D_ = tmpD;
        uint8_t tmpE = cpu->E;
        cpu->E = cpu->E_;
        cpu->E_ = tmpE;
        uint8_t tmpH = cpu->H;
        cpu->H = cpu->H_;
        cpu->H_ = tmpH;
        uint8_t tmpL = cpu->L;
        cpu->L = cpu->L_;
        cpu->L_ = tmpL;
        return;
    }

    case 0x18:
    {
        // JR e — unconditional relative jump
        cpu->PC += 1;
        int8_t d = (int8_t)AdvanceByte(cpu);
        cpu->PC += d;
        return;
    }

    case 0x10:
    {
        // DJNZ e — decrement B, relative jump if B != 0
        cpu->PC += 1;
        int8_t d = (int8_t)AdvanceByte(cpu);
        cpu->B -= 1;
        if (cpu->B != 0)
            cpu->PC += d;
        return;
    }

    case 0x20:
    {
        // JR NZ, e
        cpu->PC += 1;
        int8_t d = (int8_t)AdvanceByte(cpu);
        if (!cpu->zf)
            cpu->PC += d;
        return;
    }

    case 0x28:
    {
        // JR Z, e
        cpu->PC += 1;
        int8_t d = (int8_t)AdvanceByte(cpu);
        if (cpu->zf)
            cpu->PC += d;
        return;
    }

    case 0x30:
    {
        // JR NC, e
        cpu->PC += 1;
        int8_t d = (int8_t)AdvanceByte(cpu);
        if (!cpu->cf)
            cpu->PC += d;
        return;
    }

    case 0x38:
    {
        // JR C, e
        cpu->PC += 1;
        int8_t d = (int8_t)AdvanceByte(cpu);
        if (cpu->cf)
            cpu->PC += d;
        return;
    }

    default:
    {
        ExecuteOpCode(OpCode, cpu);
        // Fix N (subtract) flag — not present in 8080 but required by Z80
        if ((OpCode & 0xC0) == 0x80)
        {
            // 8-bit arithmetic group 0x80-0xBF: bits [5:3] = operation
            // 0=ADD,1=ADC → N=0; 2=SUB,3=SBC,7=CP → N=1; 4=AND,5=XOR,6=OR → N=0
            uint8_t op = (OpCode >> 3) & 7;
            cpu->nf = (op == 2 || op == 3 || op == 7);
        }
        else if ((OpCode & 0xC7) == 0x04)
        {
            cpu->nf = false; // INC r (00rrr100)
        }
        else if ((OpCode & 0xC7) == 0x05)
        {
            cpu->nf = true; // DEC r (00rrr101)
        }
        else
        {
            switch (OpCode)
            {
            case 0xC6:
            case 0xCE:
                cpu->nf = false;
                break; // ADD A,n; ADC A,n
            case 0xD6:
            case 0xDE:
                cpu->nf = true;
                break; // SUB n; SBC A,n
            case 0xE6:
            case 0xEE:
            case 0xF6:
                cpu->nf = false;
                break; // AND/XOR/OR n
            case 0xFE:
                cpu->nf = true;
                break; // CP n
            default:
                break;
            }
        }
        return;
    }
    }
}

uint32_t ExecuteZ80Next(zilogZ80 *cpu)
{
    if (cpu->halted)
        return 0;

    uint32_t before = cpu->cycles;
    uint8_t opcode = cpu->ReadMem8(cpu->PC);
    ExecuteZ80OpCode(opcode, cpu);

    uint32_t elapsed = cpu->cycles - before;
    if (elapsed == 0) {
        elapsed = OPCODE_CYCLES[opcode];
        cpu->cycles += elapsed;
        cpu->cyclesInterrupt += elapsed;
    }
    return elapsed;
}

uint32_t ExecuteZ80ForCycles(zilogZ80 *cpu, uint32_t cycleBudget)
{
    uint32_t elapsed = 0;
    while (elapsed < cycleBudget && !cpu->halted) {
        uint32_t step = ExecuteZ80Next(cpu);
        elapsed += step ? step : 4;
    }
    return elapsed;
}
