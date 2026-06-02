#ifndef ZILOGZ80_H
#define ZILOGZ80_H

#include "src/core/bus/z80_bus.h"
#include "intel8080.h"

enum class Z80Prefix : uint8_t {
    None = 0,
    CB,
    ED,
    DD,
    FD,
};

class zilogZ80 : public intel8080
{
public:
    uint8_t I;
    uint8_t R;
    uint16_t IX;
    uint16_t IY;
    uint8_t IM;
    bool IFF1;
    bool IFF2;
    Z80Prefix prefixState;

    // Shadow registers (EX AF,AF' / EXX)
    uint8_t A_, B_, C_, D_, E_, H_, L_;
    bool sf_, zf_, acf_, pf_, cf_, yf_, xf_, nf_;

    // Undocumented flags: bit 5 (Y) and bit 3 (X) of F
    bool yf, xf;
    bool nf; // N (subtract) flag: set by subtraction ops, cleared by addition ops
    core::Z80Bus *bus;

    zilogZ80();
    void AttachBus(core::Z80Bus *newBus);
    uint8_t ReadMem8(uint16_t address) override;
    uint16_t ReadMem16(uint16_t address) override;
    void WriteMem(uint16_t address, uint16_t value) override;
    uint8_t InPort8(uint16_t port) override;
    void OutPort8(uint16_t port, uint8_t value) override;
    void ResetZ80();
    bool MaskableInterrupt(uint8_t dataBus = 0xff);
};

void ExecuteZ80OpCode(uint8_t OpCode, zilogZ80 *cpu);
uint32_t ExecuteZ80Next(zilogZ80 *cpu);
uint32_t ExecuteZ80ForCycles(zilogZ80 *cpu, uint32_t cycleBudget);

#endif // ZILOGZ80_H
