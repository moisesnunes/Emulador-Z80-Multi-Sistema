#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include "intel8080.h"

// ── TMS9918A Video Display Processor ────────────────────────────────────────
// Ports: 0x98 = data, 0x99 = control/status
struct TMS9918A
{
    uint8_t vram[0x4000]; // 16KB VRAM (separate from CPU address space)
    uint8_t regs[8];      // R0–R7
    uint8_t status;       // bit7=VBlank INT, bit6=5th-sprite, bit5=coincidence
    uint16_t addr;        // current 14-bit VRAM address
    uint8_t latch;        // first byte of two-byte control write
    bool latched;         // true = waiting for second control byte
    uint8_t readBuf;      // pre-fetched read byte (VRAM reads are one cycle ahead)

    void Reset();
    void WriteData(uint8_t val);
    void WriteControl(uint8_t val);
    uint8_t ReadData();
    uint8_t ReadStatus();

    // Render a 256×192 frame into rgb (3 bytes per pixel, row-major).
    void RenderFrame(uint8_t *rgb);
};

// ── AY-3-8910 PSG (stub: captures register state, no audio output) ──────────
// Ports: 0xA0 = address latch, 0xA1 = data write, 0xA2 = data read
struct AY8910
{
    uint8_t regs[16] = {};
    uint8_t addrLatch = 0;

    void Reset()
    {
        memset(regs, 0, sizeof(regs));
        addrLatch = 0;
    }
};

// ── Intel 8255 PPI ───────────────────────────────────────────────────────────
// Port A (0xA8): slot selection — bits[1:0]=page0 slot, [3:2]=page1, [5:4]=page2, [7:6]=page3
// Port B (0xA9): keyboard matrix data (read, active low)
// Port C (0xAA): keyboard row select (bits[3:0]) + misc output bits
struct PPI8255
{
    uint8_t portA = 0xF0;       // initial: pages 0,1→slot0(BIOS); pages 2,3→slot3(RAM)
    uint8_t portC = 0x00;       // bits[3:0] = keyboard row
    int pageSlot[4] = {0, 0, 3, 3};
};

// ── MSX machine state ────────────────────────────────────────────────────────
struct MSXState
{
    TMS9918A vdp;
    AY8910   psg;
    PPI8255  ppi;

    // Four 64KB slot banks.
    //   Slot 0: BIOS ROM (loaded from msxbios.rom via eprom=)
    //   Slot 3: Main RAM
    //   Slots 1,2: cartridge (loaded via cart1=/cart2= in game.cfg)
    uint8_t slotRam[4][0x10000];

    // true = slot is ROM (writes ignored when the page is mapped in).
    bool slotIsRom[4] = {true, false, false, false};

    // MSX keyboard matrix: 11 rows × 8 columns (bit=0 means pressed, active low).
    uint8_t keyMatrix[11];
    uint8_t joyState[2] = {0x3F, 0x3F}; // bits 0-5 active low: U,D,L,R,A,B

    // Page pointers: fast access into the currently mapped slot for each 16KB page.
    uint8_t *pagePtr[4];

    bool running = true;
};

// ── Memory helpers ───────────────────────────────────────────────────────────

inline uint8_t MSXRead(MSXState &msx, uint16_t addr)
{
    return msx.pagePtr[addr >> 14][addr & 0x3FFF];
}

inline void MSXWrite(MSXState &msx, uint16_t addr, uint8_t value)
{
    int page = addr >> 14;
    int slot = msx.ppi.pageSlot[page];
    if (msx.slotIsRom[slot])
        return;
    msx.pagePtr[page][addr & 0x3FFF] = value;
}

// Copy BIOS from cpu->memory (already loaded by eprom=) into slotRam[0],
// set up the initial page→slot mapping and ROM/RAM protection.
void MSXInitMemory(MSXState &msx, intel8080 *cpu);

// Register all MSX I/O port handlers (call ClearPortHandlers first).
// cpu is captured by the PPI slot-swap handler.
void RegisterMSXPorts(MSXState &msx, intel8080 *cpu);

// Load a cartridge ROM file into slotRam[slot] at loadAddr (auto-detected if -1).
// Supports 16 KB (page 1 only) and 32 KB (pages 1+2) cartridges.
// Marks the slot as ROM so writes are blocked when the page is mapped in.
// Call after MSXInitMemory and before the main emulation loop.
bool MSXLoadCartridge(MSXState &msx, intel8080 *cpu, int slot,
                      const std::string &path, int32_t loadAddr = -1);

// Translate a GLFW key event into the MSX keyboard matrix.
void MSXKeyCallback(MSXState &msx, int glfwKey, int action);
