#include "game_config.h"
#include <fstream>
#include <iostream>
#include <unistd.h>

static void ParseMSXCartValue(const std::string &gameName, const std::string &val,
                              std::string &path, int32_t &addr, int32_t defaultAddr)
{
    auto at = val.find('@');
    std::string fileName = (at != std::string::npos) ? val.substr(0, at) : val;
    path = "/roms/" + gameName + "/" + fileName;

    if (at != std::string::npos)
        addr = (int32_t)std::stoi(val.substr(at + 1), nullptr, 16);
    else
        addr = defaultAddr;
}

std::string GetExeDir()
{
    char pBuf[256];
    ssize_t bytes = readlink("/proc/self/exe", pBuf, sizeof(pBuf) - 1);
    if (bytes != -1)
        pBuf[bytes] = '\0';
    std::string exePath(pBuf);
    return exePath.substr(0, exePath.rfind('/'));
}

GameConfig LoadGameConfig(const std::string &exeDir, const std::string &gameName)
{
    GameConfig cfg;
    cfg.name = gameName;
    cfg.title = gameName;

    std::string cfgPath = exeDir + "/roms/" + gameName + "/game.cfg";
    std::ifstream file(cfgPath);
    if (!file.is_open())
    {
        std::cerr << "Could not open config: " << cfgPath << std::endl;
        return cfg;
    }

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        auto eq = line.find('=');
        if (eq != std::string::npos)
        {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (key == "title")
                cfg.title = val;
            else if (key == "cpu")
                cfg.cpu = (val == "z80") ? CpuType::Z80 : CpuType::I8080;
            else if (key == "mode")
            {
                if (val == "cpm")
                {
                    cfg.mode = EmulatorMode::CPM;
                    cfg.romLoadOffset = 0x0100;
                }
                else if (val == "altair")
                    cfg.mode = EmulatorMode::ALTAIR;
                else if (val == "msx")
                {
                    cfg.mode = EmulatorMode::MSX;
                    cfg.cpu  = CpuType::Z80; // MSX always runs a Z80
                }
                else
                    cfg.mode = EmulatorMode::ARCADE;
            }
            else if (key == "vramStart")
                cfg.vramStart = std::stoi(val, nullptr, 16);
            else if (key == "vramEnd")
                cfg.vramEnd = std::stoi(val, nullptr, 16);
            else if (key == "screenW")
                cfg.screenW = std::stoi(val);
            else if (key == "screenH")
                cfg.screenH = std::stoi(val);
            else if (key == "interrupt_hz")
                cfg.interruptHz = std::stof(val);
            else if (key == "cpu_hz")
                cfg.cpuHz = (uint32_t)std::stoul(val);
            else if (key == "scanlines")
                cfg.arcadeScanlines = std::stoi(val);
            else if (key == "rst_mid")
                cfg.rstMidFrame = (uint16_t)std::stoi(val, nullptr, 16);
            else if (key == "rst_end")
                cfg.rstEndFrame = (uint16_t)std::stoi(val, nullptr, 16);
            else if (key == "reader")
                cfg.cpmReader = val;
            else if (key == "punch")
                cfg.cpmPunch = val;
            else if (key == "printer")
                cfg.cpmPrinter = val;
            else if (key == "terminal")
            {
                if (val == "ibm3101")
                    cfg.cpmTerminal = TermType::IBM3101;
                else if (val == "visual200")
                    cfg.cpmTerminal = TermType::VISUAL200;
                else
                    cfg.cpmTerminal = TermType::ADM3A;
            }
            else if (key == "overlay_base")
                cfg.overlayBase = (uint16_t)std::stoi(val, nullptr, 16);
            else if (key == "overlay_size")
                cfg.overlaySize = (uint16_t)std::stoi(val, nullptr, 16);
            else if (key == "serial_port")
                cfg.serialPort = (uint16_t)std::stoi(val);
            else if (key == "serial_baud")
                cfg.serialBaud = (uint32_t)std::stoi(val);
            else if (key == "serial_fifo_path")
                cfg.serialFifoPath = val;
            else if (key == "serial_console")
                cfg.serialConsole = (val == "yes" || val == "1" || val == "true");
            else if (key == "pc_start")
                cfg.pcStart = (uint16_t)std::stoi(val, nullptr, 16);
            else if (key == "cart1")
                ParseMSXCartValue(gameName, val, cfg.msxCart1, cfg.msxCart1Addr, -1);
            else if (key == "cart2")
                ParseMSXCartValue(gameName, val, cfg.msxCart2, cfg.msxCart2Addr, -1);
            else if (key == "cart1_ext")
            {
                // cart1_ext=file.rom@0x8000
                int32_t addr = 0x8000;
                ParseMSXCartValue(gameName, val, cfg.msxCart1Ext, addr, 0x8000);
                cfg.msxCart1ExtAddr = (uint16_t)addr;
            }
            else if (key == "eprom")
            {
                // eprom=0xE000,bios.bin
                auto comma = val.find(',');
                if (comma != std::string::npos)
                {
                    EpromEntry e;
                    e.addr = (uint16_t)std::stoi(val.substr(0, comma), nullptr, 16);
                    e.path = "/roms/" + gameName + "/" + val.substr(comma + 1);
                    cfg.epromRegions.push_back(e);
                }
            }
        }
        else
        {
            // filename or filename@0xADDR — explicit address is optional
            RomEntry entry;
            auto at = line.find('@');
            if (at != std::string::npos)
            {
                entry.path = "/roms/" + gameName + "/" + line.substr(0, at);
                entry.loadAddr = (int32_t)std::stoi(line.substr(at + 1), nullptr, 16);
            }
            else
            {
                entry.path = "/roms/" + gameName + "/" + line;
                entry.loadAddr = -1;
            }
            cfg.romFiles.push_back(entry);
        }
    }
    return cfg;
}
