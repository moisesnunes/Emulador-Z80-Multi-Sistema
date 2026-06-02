#include "msx_machine.h"
#include "alu.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <GLFW/glfw3.h>

// ── TMS9918A colour palette (16 entries, RGB) ────────────────────────────────
static const uint8_t kPal[16][3] = {
    {0, 0, 0},       // 0  transparent (rendered as backdrop)
    {0, 0, 0},       // 1  black
    {62, 184, 73},   // 2  medium green
    {116, 208, 125}, // 3  light green
    {89, 85, 224},   // 4  dark blue
    {128, 118, 241}, // 5  light blue
    {185, 94, 81},   // 6  dark red
    {101, 219, 239}, // 7  cyan
    {219, 101, 89},  // 8  medium red
    {255, 137, 125}, // 9  light red
    {204, 195, 94},  // 10 dark yellow
    {222, 208, 135}, // 11 light yellow
    {58, 162, 65},   // 12 dark green
    {183, 102, 181}, // 13 magenta
    {204, 204, 204}, // 14 grey
    {255, 255, 255}, // 15 white
};

// ── TMS9918A ─────────────────────────────────────────────────────────────────

void TMS9918A::Reset()
{
    memset(vram, 0, sizeof(vram));
    memset(regs, 0, sizeof(regs));
    status = 0;
    addr = 0;
    latch = 0;
    latched = false;
    readBuf = 0;
}

void TMS9918A::WriteData(uint8_t val)
{
    latched = false;
    vram[addr & 0x3FFF] = val;
    addr = (addr + 1) & 0x3FFF;
}

uint8_t TMS9918A::ReadData()
{
    latched = false;
    uint8_t ret = readBuf;
    readBuf = vram[addr & 0x3FFF];
    addr = (addr + 1) & 0x3FFF;
    return ret;
}

// Two-byte control-write protocol:
//   1st byte → address low 8 bits (latched)
//   2nd byte:
//     bit7=0 → VRAM address: addr = latch | (byte[5:0] << 8); bit6=0→read, 1→write
//     bit7=1 → register write: regs[byte[2:0]] = latch
void TMS9918A::WriteControl(uint8_t val)
{
    if (!latched)
    {
        latch = val;
        latched = true;
    }
    else
    {
        latched = false;
        if (val & 0x80)
        {
            regs[val & 0x07] = latch;
        }
        else
        {
            addr = (latch | ((uint16_t)(val & 0x3F) << 8)) & 0x3FFF;
            if (!(val & 0x40))
            {
                readBuf = vram[addr & 0x3FFF];
                addr = (addr + 1) & 0x3FFF;
            }
        }
    }
}

uint8_t TMS9918A::ReadStatus()
{
    latched = false;
    uint8_t ret = status;
    status &= ~0xE0; // clear INT, 5th-sprite, coincidence on read
    return ret;
}

// ── VDP renderer ─────────────────────────────────────────────────────────────

static inline void PutPixel(uint8_t *buf, int x, int y, uint8_t ci)
{
    if ((unsigned)x >= 256u || (unsigned)y >= 192u)
        return;
    const uint8_t *c = kPal[ci & 0x0F];
    int off = (y * 256 + x) * 3;
    buf[off] = c[0];
    buf[off + 1] = c[1];
    buf[off + 2] = c[2];
}

void TMS9918A::RenderFrame(uint8_t *rgb)
{
    // Reset per-frame sprite status bits (5th-sprite overflow, coincidence, sprite#).
    // Keep bit7 (VBlank) — it is set by the main loop and cleared by ReadStatus().
    status &= 0x80;

    uint8_t bgCI = regs[7] & 0x0F;
    if (bgCI == 0)
        bgCI = 1; // transparent → black

    // Fill backdrop
    const uint8_t *bgC = kPal[bgCI];
    for (int i = 0; i < 256 * 192; i++)
    {
        rgb[i * 3] = bgC[0];
        rgb[i * 3 + 1] = bgC[1];
        rgb[i * 3 + 2] = bgC[2];
    }

    // R1 bit6 = BLANK: when 0 the VDP outputs only backdrop, no tiles or sprites.
    if (!(regs[1] & 0x40))
        return;

    bool m1 = (regs[1] >> 4) & 1; // R1 bit4 = M1 (text mode)
    bool m2 = (regs[0] >> 1) & 1; // R0 bit1 = M3 (graphics II)

    // ── Graphics I (SCREEN 1): 32×24 tiles, 8×8 pixels, colour per 8 tiles ──
    if (!m1 && !m2)
    {
        uint16_t nameBase = (uint16_t)(regs[2] & 0x0F) << 10;
        uint16_t colorBase = (uint16_t)regs[3] << 6;
        uint16_t patternBase = (uint16_t)(regs[4] & 0x07) << 11;

        for (int row = 0; row < 24; row++)
        {
            for (int col = 0; col < 32; col++)
            {
                uint8_t tile = vram[(nameBase + row * 32 + col) & 0x3FFF];
                uint8_t colorByte = vram[(colorBase + tile / 8) & 0x3FFF];
                uint8_t fg = (colorByte >> 4) & 0x0F;
                uint8_t bg = colorByte & 0x0F;
                if (fg == 0)
                    fg = bgCI;
                if (bg == 0)
                    bg = bgCI;

                for (int py = 0; py < 8; py++)
                {
                    uint8_t pat = vram[(patternBase + tile * 8 + py) & 0x3FFF];
                    for (int px = 0; px < 8; px++)
                        PutPixel(rgb, col * 8 + px, row * 8 + py, (pat >> (7 - px)) & 1 ? fg : bg);
                }
            }
        }
    }
    // ── Text mode (SCREEN 0): 40×24 chars, 6×8 pixels, two fixed colours ────
    else if (m1 && !m2)
    {
        uint16_t nameBase = (uint16_t)(regs[2] & 0x0F) << 10;
        uint16_t patternBase = (uint16_t)(regs[4] & 0x07) << 11;
        uint8_t fg = (regs[7] >> 4) & 0x0F;
        uint8_t bg = regs[7] & 0x0F;
        if (fg == 0)
            fg = 15;
        if (bg == 0)
            bg = 1;

        for (int row = 0; row < 24; row++)
        {
            for (int col = 0; col < 40; col++)
            {
                uint8_t tile = vram[(nameBase + row * 40 + col) & 0x3FFF];
                for (int py = 0; py < 8; py++)
                {
                    uint8_t pat = vram[(patternBase + tile * 8 + py) & 0x3FFF];
                    for (int px = 0; px < 6; px++)
                        PutPixel(rgb, 8 + col * 6 + px, row * 8 + py, (pat >> (7 - px)) & 1 ? fg : bg);
                }
            }
        }
    }
    // ── Graphics II (SCREEN 2): 256×192 bitmapped, colour per 8-pixel row ───
    else if (!m1 && m2)
    {
        uint16_t nameBase = (uint16_t)(regs[2] & 0x0F) << 10;
        uint16_t colorBase = (regs[3] & 0x80) ? 0x2000u : 0x0000u;
        uint16_t patternBase = (regs[4] & 0x04) ? 0x2000u : 0x0000u;

        for (int row = 0; row < 24; row++)
        {
            int bank = row / 8;
            for (int col = 0; col < 32; col++)
            {
                uint8_t tile = vram[(nameBase + row * 32 + col) & 0x3FFF];
                uint16_t patOff = patternBase + bank * 0x800 + tile * 8;
                uint16_t colOff = colorBase + bank * 0x800 + tile * 8;

                for (int py = 0; py < 8; py++)
                {
                    uint8_t pat = vram[(patOff + py) & 0x3FFF];
                    uint8_t colB = vram[(colOff + py) & 0x3FFF];
                    uint8_t fg = (colB >> 4) & 0x0F;
                    uint8_t bg = colB & 0x0F;
                    if (fg == 0)
                        fg = bgCI;
                    if (bg == 0)
                        bg = bgCI;
                    for (int px = 0; px < 8; px++)
                        PutPixel(rgb, col * 8 + px, row * 8 + py, (pat >> (7 - px)) & 1 ? fg : bg);
                }
            }
        }
    }
    // Multicolor (SCREEN 3): not rendered (rare on MSX software)

    // ── Sprites (Graphics I and II only) ────────────────────────────────────
    if (!m1)
    {
        auto spriteMaskBuf = std::make_unique<bool[]>(256 * 192);
        bool *spriteMask = spriteMaskBuf.get();
        memset(spriteMask, 0, 256 * 192 * sizeof(bool));
        int spritesPerLine[192] = {};

        uint16_t satBase = (uint16_t)(regs[5] & 0x7F) << 7;
        uint16_t spgBase = (uint16_t)(regs[6] & 0x07) << 11;
        bool size16 = (regs[1] >> 1) & 1;
        bool mag = (regs[1] >> 0) & 1;
        int spriteSize = size16 ? 16 : 8;
        int renderSize = mag ? spriteSize * 2 : spriteSize;

        for (int s = 0; s < 32; s++)
        {
            uint8_t sy = vram[(satBase + s * 4 + 0) & 0x3FFF];
            if (sy == 0xD0)
                break;

            uint8_t sx = vram[(satBase + s * 4 + 1) & 0x3FFF];
            uint8_t spat = vram[(satBase + s * 4 + 2) & 0x3FFF];
            uint8_t attr = vram[(satBase + s * 4 + 3) & 0x3FFF];
            uint8_t color = attr & 0x0F;
            if (color == 0)
                continue; // transparent sprite

            bool ec = (attr >> 7) & 1;
            int ox = (int)sx - (ec ? 32 : 0);
            int oy = (sy > 0xD0) ? ((int)sy - 255) : ((int)sy + 1);

            for (int py = 0; py < renderSize; py++)
            {
                int realY = oy + py;
                if (realY < 0 || realY >= 192)
                    continue;

                spritesPerLine[realY]++;
                if (spritesPerLine[realY] > 4)
                {
                    if (!(status & 0x40))
                        status = (status & 0xC0) | 0x40 | (uint8_t)(s & 0x1F);
                    continue;
                }

                int srcY = mag ? (py / 2) : py;
                for (int px = 0; px < renderSize; px++)
                {
                    int srcX = mag ? (px / 2) : px;
                    int tileX = srcX / 8;
                    int tileY = srcY / 8;
                    int localX = srcX & 7;
                    int localY = srcY & 7;

                    int tileIndex = spat;
                    if (size16)
                    {
                        tileIndex &= 0xFC;
                        tileIndex += tileX * 2 + tileY;
                    }

                    uint8_t rowBits = vram[(spgBase + tileIndex * 8 + localY) & 0x3FFF];
                    if (!((rowBits >> (7 - localX)) & 1))
                        continue;

                    int realX = ox + px;
                    if (realX < 0 || realX >= 256)
                        continue;

                    int maskIdx = realY * 256 + realX;
                    if (spriteMask[maskIdx])
                    {
                        status |= 0x20; // coincidence
                    }
                    else
                    {
                        spriteMask[maskIdx] = true;
                        PutPixel(rgb, realX, realY, color);
                    }
                }
            }
        }
    }
}

// ── Slot mapper ───────────────────────────────────────────────────────────────

static void MSXSwapPage(MSXState &msx, intel8080 *cpu, int page, int newSlot)
{
    int base = page * 0x4000;
    int curSlot = msx.ppi.pageSlot[page];

    if (!msx.slotIsRom[curSlot])
        memcpy(&msx.slotRam[curSlot][base], &cpu->memory[base], 0x4000);

    msx.ppi.pageSlot[page] = newSlot;
    msx.pagePtr[page] = &msx.slotRam[newSlot][base];
    memcpy(&cpu->memory[base], &msx.slotRam[newSlot][base], 0x4000);

    if (msx.slotIsRom[newSlot])
        cpu->SetRomRegion((uint16_t)base, 0x4000);
    else
        cpu->SetRamRegion((uint16_t)base, 0x4000);
}

void MSXInitMemory(MSXState &msx, intel8080 *cpu)
{
    msx.slotIsRom[0] = true;
    msx.slotIsRom[1] = false;
    msx.slotIsRom[2] = false;
    msx.slotIsRom[3] = false;

    memcpy(msx.slotRam[0], cpu->memory, 0x10000);
    memset(msx.slotRam[1], 0, sizeof(msx.slotRam[1]));
    memset(msx.slotRam[2], 0, sizeof(msx.slotRam[2]));
    memset(msx.slotRam[3], 0, sizeof(msx.slotRam[3]));

    memset(msx.keyMatrix, 0xFF, sizeof(msx.keyMatrix));
    msx.joyState[0] = 0x3F;
    msx.joyState[1] = 0x3F;

    msx.psg.Reset();
    msx.psg.regs[15] = 0x8F;

    msx.ppi.portA = 0xF0;

    for (int i = 0; i < 4; i++)
        msx.pagePtr[i] = &msx.slotRam[msx.ppi.pageSlot[i]][i * 0x4000];

    memcpy(cpu->memory, msx.slotRam[0], 0x8000);
    memset(cpu->memory + 0x8000, 0, 0x8000);
    cpu->SetRomRegion(0x0000, 0x8000);
    cpu->SetRamRegion(0x8000, 0x8000);
}

static uint16_t MSXDetectCartLoadAddress(const std::vector<uint8_t> &rom)
{
    if (rom.size() >= 4 && rom[0] == 'A' && rom[1] == 'B')
    {
        uint16_t init = (uint16_t)(rom[2] | ((uint16_t)rom[3] << 8));
        if (init >= 0x8000 && init < 0xC000)
            return 0x8000;
    }
    return 0x4000;
}

bool MSXLoadCartridge(MSXState &msx, intel8080 *cpu, int slot,
                      const std::string &path, int32_t loadAddr)
{
    if (slot < 1 || slot > 2)
        return false;

    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
    {
        fprintf(stderr, "MSX cart slot %d: cannot open '%s'\n", slot, path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fileSize <= 0)
    {
        fclose(f);
        fprintf(stderr, "MSX cart slot %d: empty ROM '%s'\n", slot, path.c_str());
        return false;
    }

    std::vector<uint8_t> rom((size_t)fileSize);
    if (fread(rom.data(), 1, rom.size(), f) != rom.size())
    {
        fclose(f);
        fprintf(stderr, "MSX cart slot %d: short read '%s'\n", slot, path.c_str());
        return false;
    }
    fclose(f);

    uint16_t base = (loadAddr < 0) ? MSXDetectCartLoadAddress(rom) : (uint16_t)loadAddr;
    long maxSize = 0x10000 - base;
    if (fileSize > maxSize)
        fileSize = maxSize;
    memcpy(&msx.slotRam[slot][base], rom.data(), (size_t)fileSize);

    msx.slotIsRom[slot] = true;

    for (int page = 0; page < 4; page++)
    {
        if (msx.ppi.pageSlot[page] == slot)
        {
            int pageBase = page * 0x4000;
            msx.pagePtr[page] = &msx.slotRam[slot][pageBase];
            memcpy(&cpu->memory[pageBase], &msx.slotRam[slot][pageBase], 0x4000);
            cpu->SetRomRegion((uint16_t)pageBase, 0x4000);
        }
    }

    return true;
}

// ── MSX keyboard matrix ───────────────────────────────────────────────────────
// 11 rows × 8 columns (bit=0 = key pressed, active low)
//
// Row  | b7   b6   b5   b4   b3   b2   b1   b0
//  0   |  7    6    5    4    3    2    1    0     (digit keys)
//  1   |  ;    ]    [    \    =    -    9    8
//  2   |  B    A    _    /    .    ,    `    '
//  3   |  J    I    H    G    F    E    D    C
//  4   |  R    Q    P    O    N    M    L    K
//  5   |  Z    Y    X    W    V    U    T    S
//  6   | F3   F2   F1  CODE  CAP GRPH CTRL SHFT
//  7   | RET  SEL   BS  STOP  TAB  ESC  F5   F4
//  8   |  ↓    →    ↑    ←   DEL  INS  HOME SPACE
//  9   |  *    +    /    -    9    8    7    (col0 unused)
// 10   |  ,    .    0   ENT   3    2    1    (col0 unused)

struct MSXKey
{
    int row, col;
};

static MSXKey GlfwToMSX(int k)
{
    switch (k)
    {
    // Row 0 — digit row
    case GLFW_KEY_0:
        return {0, 0};
    case GLFW_KEY_1:
        return {0, 1};
    case GLFW_KEY_2:
        return {0, 2};
    case GLFW_KEY_3:
        return {0, 3};
    case GLFW_KEY_4:
        return {0, 4};
    case GLFW_KEY_5:
        return {0, 5};
    case GLFW_KEY_6:
        return {0, 6};
    case GLFW_KEY_7:
        return {0, 7};
    // Row 1 — 8, 9, punctuation
    case GLFW_KEY_8:
        return {1, 0};
    case GLFW_KEY_9:
        return {1, 1};
    case GLFW_KEY_MINUS:
        return {1, 2};
    case GLFW_KEY_EQUAL:
        return {1, 3};
    case GLFW_KEY_BACKSLASH:
        return {1, 4};
    case GLFW_KEY_LEFT_BRACKET:
        return {1, 5};
    case GLFW_KEY_RIGHT_BRACKET:
        return {1, 6};
    case GLFW_KEY_SEMICOLON:
        return {1, 7};
    // Row 2 — punctuation + A, B
    case GLFW_KEY_APOSTROPHE:
        return {2, 0};
    case GLFW_KEY_GRAVE_ACCENT:
        return {2, 1};
    case GLFW_KEY_COMMA:
        return {2, 2};
    case GLFW_KEY_PERIOD:
        return {2, 3};
    case GLFW_KEY_SLASH:
        return {2, 4};
    // col5 = _ (no direct GLFW key)
    case GLFW_KEY_A:
        return {2, 6};
    case GLFW_KEY_B:
        return {2, 7};
    // Row 3 — letters C–J
    case GLFW_KEY_C:
        return {3, 0};
    case GLFW_KEY_D:
        return {3, 1};
    case GLFW_KEY_E:
        return {3, 2};
    case GLFW_KEY_F:
        return {3, 3};
    case GLFW_KEY_G:
        return {3, 4};
    case GLFW_KEY_H:
        return {3, 5};
    case GLFW_KEY_I:
        return {3, 6};
    case GLFW_KEY_J:
        return {3, 7};
    // Row 4 — letters K–R
    case GLFW_KEY_K:
        return {4, 0};
    case GLFW_KEY_L:
        return {4, 1};
    case GLFW_KEY_M:
        return {4, 2};
    case GLFW_KEY_N:
        return {4, 3};
    case GLFW_KEY_O:
        return {4, 4};
    case GLFW_KEY_P:
        return {4, 5};
    case GLFW_KEY_Q:
        return {4, 6};
    case GLFW_KEY_R:
        return {4, 7};
    // Row 5 — letters S–Z
    case GLFW_KEY_S:
        return {5, 0};
    case GLFW_KEY_T:
        return {5, 1};
    case GLFW_KEY_U:
        return {5, 2};
    case GLFW_KEY_V:
        return {5, 3};
    case GLFW_KEY_W:
        return {5, 4};
    case GLFW_KEY_X:
        return {5, 5};
    case GLFW_KEY_Y:
        return {5, 6};
    case GLFW_KEY_Z:
        return {5, 7};
    // Row 6 — modifier / function keys
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT:
        return {6, 0};
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL:
        return {6, 1};
    // GRPH → map to Left Alt; CODE → map to Right Alt
    case GLFW_KEY_LEFT_ALT:
        return {6, 2}; // GRPH
    case GLFW_KEY_RIGHT_ALT:
        return {6, 4}; // CODE
    case GLFW_KEY_F1:
        return {6, 5};
    case GLFW_KEY_F2:
        return {6, 6};
    case GLFW_KEY_F3:
        return {6, 7};
    // Row 7 — editing / function keys
    case GLFW_KEY_F4:
        return {7, 0};
    case GLFW_KEY_F5:
        return {7, 1};
    case GLFW_KEY_ESCAPE:
        return {7, 2};
    case GLFW_KEY_TAB:
        return {7, 3};
    case GLFW_KEY_F12:
        return {7, 4}; // STOP (no direct PC key)
    case GLFW_KEY_BACKSPACE:
        return {7, 5};
    case GLFW_KEY_F11:
        return {7, 6}; // SELECT
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
        return {7, 7};
    // Row 8 — cursor / editing
    case GLFW_KEY_SPACE:
        return {8, 0};
    case GLFW_KEY_HOME:
        return {8, 1};
    case GLFW_KEY_INSERT:
        return {8, 2};
    case GLFW_KEY_DELETE:
        return {8, 3};
    case GLFW_KEY_LEFT:
        return {8, 4};
    case GLFW_KEY_UP:
        return {8, 5};
    case GLFW_KEY_RIGHT:
        return {8, 6};
    case GLFW_KEY_DOWN:
        return {8, 7};
    // Row 9 — numpad
    case GLFW_KEY_KP_7:
        return {9, 1};
    case GLFW_KEY_KP_8:
        return {9, 2};
    case GLFW_KEY_KP_9:
        return {9, 3};
    case GLFW_KEY_KP_SUBTRACT:
        return {9, 4};
    case GLFW_KEY_KP_DIVIDE:
        return {9, 5};
    case GLFW_KEY_KP_ADD:
        return {9, 6};
    case GLFW_KEY_KP_MULTIPLY:
        return {9, 7};
    // Row 10 — numpad continued
    case GLFW_KEY_KP_1:
        return {10, 1};
    case GLFW_KEY_KP_2:
        return {10, 2};
    case GLFW_KEY_KP_3:
        return {10, 3};
    case GLFW_KEY_KP_DECIMAL:
        return {10, 4};
    case GLFW_KEY_KP_0:
        return {10, 5};
    default:
        return {-1, -1};
    }
}

void MSXKeyCallback(MSXState &msx, int glfwKey, int action)
{
    MSXKey mk = GlfwToMSX(glfwKey);
    if (mk.row < 0)
        return;
    bool pressed = (action == GLFW_PRESS || action == GLFW_REPEAT);
    if (pressed)
        msx.keyMatrix[mk.row] &= ~(uint8_t)(1 << mk.col); // 0 = pressed
    else
        msx.keyMatrix[mk.row] |= (uint8_t)(1 << mk.col); // 1 = released
}

// ── I/O port registration ────────────────────────────────────────────────────

void RegisterMSXPorts(MSXState &msx, intel8080 *cpu)
{
    // TMS9918A
    RegisterPortIn(0x98, [&msx](intel8080 *) -> uint8_t
                   { return msx.vdp.ReadData(); });
    RegisterPortIn(0x99, [&msx](intel8080 *) -> uint8_t
                   { return msx.vdp.ReadStatus(); });
    RegisterPortOut(0x98, [&msx](intel8080 *c)
                    { msx.vdp.WriteData(c->A); });
    RegisterPortOut(0x99, [&msx](intel8080 *c)
                    { msx.vdp.WriteControl(c->A); });

    // AY-3-8910 PSG
    RegisterPortOut(0xA0, [&msx](intel8080 *c)
                    { msx.psg.addrLatch = c->A & 0x0F; });
    RegisterPortOut(0xA1, [&msx](intel8080 *c)
                    { msx.psg.regs[msx.psg.addrLatch] = c->A; });
    RegisterPortIn(0xA2, [&msx](intel8080 *) -> uint8_t
                   { return msx.psg.regs[msx.psg.addrLatch]; });

    // Intel 8255 PPI
    // Port A write (0xA8): slot selection — swap memory pages when slot changes
    RegisterPortOut(0xA8, [&msx, cpu](intel8080 *c)
                    {
        uint8_t newA = c->A;
        uint8_t diff = newA ^ msx.ppi.portA;
        msx.ppi.portA = newA;
        for (int page = 0; page < 4; page++)
        {
            if ((diff >> (page * 2)) & 0x03)
            {
                int newSlot = (newA >> (page * 2)) & 0x03;
                MSXSwapPage(msx, cpu, page, newSlot);
            }
        } });
    // Port A read (0xA8): return current slot register
    RegisterPortIn(0xA8, [&msx](intel8080 *) -> uint8_t
                   { return msx.ppi.portA; });

    // Port B read (0xA9): keyboard matrix data for selected row (active low)
    RegisterPortIn(0xA9, [&msx](intel8080 *) -> uint8_t
                   {
        uint8_t row = msx.ppi.portC & 0x0F;
        return (row < 11) ? msx.keyMatrix[row] : 0xFF; });

    // Port C write (0xAA): select keyboard row (bits[3:0]) + misc output
    RegisterPortOut(0xAA, [&msx](intel8080 *c)
                    { msx.ppi.portC = c->A; });
    RegisterPortIn(0xAA, [&msx](intel8080 *) -> uint8_t
                   { return msx.ppi.portC; });

    // PPI control register (0xAB): mode-set byte — ignored (we hard-wire mode 0)
    RegisterPortOut(0xAB, [](intel8080 *) {});
}
