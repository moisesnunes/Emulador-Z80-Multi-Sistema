#pragma once
#include <string>
#include <vector>
#include <cstdint>

enum class EmulatorMode
{
    ARCADE, // arcade game: bitmap VRAM, scanline interrupts, custom I/O ports
    CPM,    // CP/M 2.2: loads a .COM at 0x0100, BDOS calls intercepted at 0x0005
    ALTAIR, // Altair 8800: 100% terminal serial, MITS BASIC via eprom=, 88-SIO on ports 0x00/0x01
    MSX     // MSX computer: Z80 + TMS9918A VDP + AY-3-8910 PSG + 8255 PPI + slot mapper
};

enum class CpuType
{
    I8080, // Intel 8080 (default)
    Z80    // Zilog Z80
};

enum class TermType
{
    ADM3A,    // Lear Siegler ADM-3A + ANSI/VT100 (default)
    IBM3101,  // IBM 3101 ASCII Display Terminal
    VISUAL200 // Visual Technology Visual 200 (VT100-compatible, same as ADM3A)
};

struct RomEntry
{
    std::string path; // path relative to exe dir, with leading /
    int32_t loadAddr; // -1 = sequential (auto), >= 0 = explicit address
};

struct EpromEntry
{
    uint16_t addr;    // base address in the 64 KB space
    std::string path; // path relative to exe dir, with leading /
};

struct GameConfig
{
    std::string name;
    std::string title;
    std::vector<RomEntry> romFiles;
    EmulatorMode mode = EmulatorMode::ARCADE;
    CpuType cpu = CpuType::I8080;
    uint16_t romLoadOffset = 0x0000; // CP/M .COM files must start at 0x0100
    // ARCADE-only fields:
    int vramStart = 0x2400;
    int vramEnd = 0x4000;
    int screenW = 224;
    int screenH = 256;
    // ARCADE interrupt timing
    float interruptHz = 60.0f;    // scanline interrupt frequency
    uint32_t cpuHz = 2000000;     // CPU clock frequency in Hz
    int arcadeScanlines = 256;    // scanlines per frame
    uint16_t rstMidFrame = 0x08;  // RST vector for mid-frame interrupt
    uint16_t rstEndFrame = 0x10;  // RST vector for end-of-frame interrupt
    // CP/M peripheral device paths (empty = use defaults)
    std::string cpmReader;
    std::string cpmPunch;
    std::string cpmPrinter;
    // CP/M terminal type (default ADM3A)
    TermType cpmTerminal = TermType::ADM3A;
    // Overlay region: address where the overlay area begins inside the TPA.
    // 0 = no overlay region defined (entire TPA is resident code/data).
    uint16_t overlayBase = 0;
    // Size of the overlay region in bytes. 0 = extends to BDOS (0xF800).
    uint16_t overlaySize = 0;
    // Simulated serial port TCP port (0 = disabled).
    // Connect with: nc localhost <serial_port>  or  telnet localhost <serial_port>
    uint16_t serialPort = 0;
    // Serial baud rate (9600, 19200, 38400, etc.). Default 9600.
    // Used for throttling byte transfer rates when using FIFO.
    uint32_t serialBaud = 9600;
    // Serial mode: empty = TCP mode (use serialPort); non-empty = FIFO mode (use this path).
    // Example: "/tmp/cpm-serial" for a named pipe.
    std::string serialFifoPath;
    // Console mode: route serial I/O directly through stdin/stdout (no TCP/FIFO).
    // game.cfg: serial_console=yes
    bool serialConsole = false;
    // EPROM regions: loaded into memory and marked read-only after main ROMs.
    // game.cfg syntax: eprom=0xE000,bios.bin
    std::vector<EpromEntry> epromRegions;
    // Initial program counter (0 = default, i.e. 0x0000 for arcade/altair, 0x0100 for CP/M).
    // game.cfg syntax: pc_start=0xE000
    uint16_t pcStart = 0x0000;
    // MSX cartridge slots: filename relative to roms/<game>/ directory.
    // Loaded into slotRam[1] / slotRam[2], protected as ROM.
    // game.cfg syntax: cart1=mygame.rom  /  cart2=other.rom
    // Optional address syntax: cart1=mygame.rom@0x4000 or @0x8000.
    // cart1_ext=file.rom@0x8000  loads an extra bank into slot 1 at the given address.
    std::string msxCart1;
    std::string msxCart2;
    int32_t     msxCart1Addr = -1; // -1 = auto-detect from ROM header/size
    int32_t     msxCart2Addr = -1; // -1 = auto-detect from ROM header/size
    std::string msxCart1Ext;      // extra file for slot 1 (e.g. second bank at 0x8000)
    uint16_t    msxCart1ExtAddr = 0x8000;
};

std::string GetExeDir();
GameConfig LoadGameConfig(const std::string &exeDir, const std::string &gameName);
