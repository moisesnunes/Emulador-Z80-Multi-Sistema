#include <algorithm>
#include <iostream>
#include <string>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "intel8080.h"
#include "zilogZ80.h"
#include "alu.h"
#include "hexbyte.h"
#include "gui.h"
#include "game_config.h"
#include "cpm_bios.h"
#include "cpm_ccp.h"
#include "msx_machine.h"

#include <time.h>
#include <unistd.h>
#include <csignal>

unsigned long long currentTime, lastGuiUpdate = 0, clockCycle = 0, invadersUpdate = 0, interruptTimer1 = 0, interruptTimer2 = 0;

static unsigned long long GetCurrentTime100ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 10000000ULL + (unsigned long long)ts.tv_nsec / 100ULL;
}

void ISR(intel8080 *cpu, int intaddress, zilogZ80 *z80 = nullptr)
{
    cpu->halted = false;
    if (z80)
    {
        z80->IFF1 = false;
        if (z80->IM == 2)
        {
            // IM2: vector table at (I << 8) | data_bus_byte
            uint16_t vec_addr = (z80->I << 8) | 0xFF;
            uint16_t target = cpu->memory[vec_addr] | (cpu->memory[vec_addr + 1] << 8);
            PushRegisterPair(cpu, (cpu->PC >> 8) & 0xFF, cpu->PC & 0xFF);
            cpu->PC = target;
            cpu->interrupts = false;
            return;
        }
        if (z80->IM == 1)
        {
            // IM1: always jump to 0x0038
            PushRegisterPair(cpu, (cpu->PC >> 8) & 0xFF, cpu->PC & 0xFF);
            cpu->PC = 0x0038;
            cpu->interrupts = false;
            return;
        }
        // IM0: execute opcode placed on the data bus (fall through to RST)
    }
    PushRegisterPair(cpu, ((cpu->PC & 0xff00) >> 8), (cpu->PC & 0xff));
    cpu->PC = intaddress;
    cpu->interrupts = false;
}

void framebuffer_size_callback(GLFWwindow *window, int width, int height);
void processInput(GLFWwindow *window, intel8080 *cpu);

// settings
const unsigned int SCR_WIDTH = 900;
const unsigned int SCR_HEIGHT = 700;

// interrupts
// two interrupts, both at 60hz
// one when the beam is half screen, the other is at end of screen
// when interrupt is triggered, disable interrupt
// push PC and then go to ISR

// Arcade timing is derived from GameConfig at startup — no globals here.

int main(int argc, char *argv[])
{
    std::string gameName = (argc > 1) ? argv[1] : "invaders";

    std::string exeDir = GetExeDir();
    GameConfig config = LoadGameConfig(exeDir, gameName);

    // CLI flag overrides game.cfg (backwards compat: ./Emulator game z80)
    for (int i = 2; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "z80" || arg == "--z80" || arg == "-z80")
            config.cpu = CpuType::Z80;
        else if (arg == "8080" || arg == "--8080")
            config.cpu = CpuType::I8080;
    }

    if (config.mode == EmulatorMode::MSX)
        config.cpu = CpuType::Z80; // MSX always runs on Z80

    bool useZ80 = (config.cpu == CpuType::Z80);

    // CP/M, Altair and MSX start with no romFiles — program/BIOS comes via eprom= or CCP.
    if (config.romFiles.empty() && config.mode != EmulatorMode::CPM && config.mode != EmulatorMode::ALTAIR && config.mode != EmulatorMode::MSX)
    {
        std::cout << "No ROM files listed in roms/" << gameName << "/game.cfg" << std::endl;
        return -1;
    }

    // Register per-machine I/O port handlers based on the selected mode.
    ClearPortHandlers();
    if (config.mode == EmulatorMode::ARCADE)
        RegisterSpaceInvadersPorts();
    else if (config.mode == EmulatorMode::ALTAIR)
        RegisterAltairSIOPorts();
    else if (config.mode == EmulatorMode::CPM)
        RegisterUSARTPorts();

    // ── Altair headless console (no GLFW — stdin/stdout IS the terminal) ─────
    if (config.mode == EmulatorMode::ALTAIR && config.serialConsole)
    {
        intel8080 *cpu = new intel8080();
        cpu->arcadeMode = false;

        // Load ROM files (if any), then EPROM regions.
        if (!config.romFiles.empty())
            LoadRomFile(cpu, exeDir, config.romFiles, config.romLoadOffset);

        for (const auto &ep : config.epromRegions)
        {
            char *data = nullptr;
            int size = SimpleOpenFile(exeDir + ep.path, data);
            if (size > 0)
            {
                int end = ep.addr + size;
                if (end > 0x10000) end = 0x10000;
                for (int i = 0; i < end - ep.addr; i++)
                    cpu->memory[ep.addr + i] = (uint8_t)data[i];
                cpu->SetRomRegion(ep.addr, (uint16_t)(end - ep.addr));
                delete[] data;
            }
        }

        if (config.pcStart != 0x0000)
            cpu->PC = config.pcStart;

        CPMState cpm;
        cpm.serial.consoleMode = true;
        cpm.serial.baud = config.serialBaud;
        g_serialState = &cpm;

        // Restore terminal on Ctrl+C.
        signal(SIGINT, [](int) {
            if (g_serialState)
                SerialClose(*g_serialState);
            exit(0);
        });

        unsigned long long lastSerialTick = 0;
        while (true)
        {
            uint8_t op = cpu->memory[cpu->PC];
            ExecuteOpCode(op, cpu);

            unsigned long long now = GetCurrentTime100ns();
            if (now - lastSerialTick >= 166666ULL) // ~60 Hz
            {
                SerialTick(cpm);
                lastSerialTick = now;
            }
        }

        SerialClose(cpm);
        g_serialState = nullptr;
        return 0;
    }

    uint16_t programCount;

    GLFWwindow *window = InitGUI(config.title);

    unsigned int shaderProgram = InitializeShader();
    unsigned int VAO = InitializeVAO();

    char PauseOnLine[5];
    int PauseOnLineInt;

    bool breakpointActive;

    const int MAXINSTRUCTIONS = 1000;
    int currentInstruction = 0;
    int previousInstructions[MAXINSTRUCTIONS * 4];

    for (int i = 0; i < MAXINSTRUCTIONS * 4; i++)
    {
        previousInstructions[i] = 0;
    }

    intel8080 *cpu = nullptr;
    if (useZ80)
        cpu = static_cast<intel8080 *>(new zilogZ80());
    else
        cpu = new intel8080();

    if (LoadRomFile(cpu, exeDir, config.romFiles, config.romLoadOffset) != 0)
    {
        return -1;
    }

    // Arcade ROMs are fixed at 0x0000-0x1FFF — protect them via the bitmask.
    if (config.mode == EmulatorMode::ARCADE)
        cpu->SetRomRegion(0x0000, 0x2000);

    // Load EPROM regions (eprom=0xAddr,file.bin in game.cfg).
    for (const auto &ep : config.epromRegions)
    {
        char *data = nullptr;
        int size = SimpleOpenFile(exeDir + ep.path, data);
        if (size > 0)
        {
            int end = ep.addr + size;
            if (end > 0x10000) end = 0x10000;
            for (int i = 0; i < end - ep.addr; i++)
                cpu->memory[ep.addr + i] = (uint8_t)data[i];
            cpu->SetRomRegion(ep.addr, (uint16_t)(end - ep.addr));
            delete[] data;
        }
    }

    // Override initial PC if game.cfg specifies pc_start=.
    if (config.pcStart != 0x0000)
        cpu->PC = config.pcStart;

    // ── CP/M mode ────────────────────────────────────────────────────────────
    if (config.mode == EmulatorMode::CPM)
    {
        cpu->arcadeMode = false;
        CPMState cpm;
        std::string diskDir = exeDir + "/roms/" + config.name;

        // Propagate peripheral device paths and terminal type from game.cfg.
        cpm.readerPath = config.cpmReader;
        cpm.punchPath = config.cpmPunch;
        cpm.printerPath = config.cpmPrinter;
        cpm.terminal.termType = config.cpmTerminal;
        cpm.overlayBase = config.overlayBase;
        cpm.overlaySize = config.overlaySize;
        cpm.serial.port = config.serialPort;
        cpm.serial.baud = config.serialBaud;
        cpm.serial.fifoPath = config.serialFifoPath;

        // Expose serial state to MachineIn/MachineOut (8251 USART port simulation).
        g_serialState = &cpm;

        RegisterCPMTerminalCallbacks(window, &cpm);

        if (config.romFiles.empty())
        {
            // No ROM specified → start with the CCP command prompt.
            CCPInit(cpu, cpm, diskDir);
        }
        else
        {
            // Specific .COM specified in game.cfg → load and run directly.
            CPMInit(cpu, cpm, diskDir);
            if (LoadRomFile(cpu, exeDir, config.romFiles, config.romLoadOffset) != 0)
                return -1;
        }

        CPMDebugState dbg;

        // Derive the NVRAM path from the disk directory.
        std::string nvramFile = diskDir + "/emulator.nvram";
        std::strncpy(dbg.nvramPath, nvramFile.c_str(), sizeof(dbg.nvramPath) - 1);

        // Auto-load: silently restore if an NVRAM snapshot exists.
        LoadCPMState(cpu, cpm, nvramFile);

        unsigned long long throttleEpoch = GetCurrentTime100ns();
        uint64_t throttleCycles = 0;

        while (!glfwWindowShouldClose(window) && cpm.running)
        {
            bool canRun = dbg.notHalted || dbg.runOnce;

            if (canRun)
            {
                if (cpm.ccpRunning)
                {
                    // CCP has control: accumulate typed characters, run commands.
                    if (CCPTick(cpu, cpm))
                        cpm.ccpRunning = false; // .COM loaded, hand off to CPU
                }
                else if (cpu->PC == 0x0000)
                {
                    // Warm-boot vector hit: program returned to 0x0000.
                    if (cpm.ccpMode)
                    {
                        cpm.ccpRunning = true;
                        cpm.ccpPrompted = false;
                    }
                    else
                    {
                        cpm.running = false;
                    }
                }
                else if (cpu->PC == 0x0005 || cpu->PC == BDOS_ADDR)
                {
                    BDOSCall(cpu, cpm);
                    // In CCP mode, fn 0 (System Reset) sets ccpRunning instead of
                    // clearing running — handled inside BDOS_SystemReset.
                    dbg.resetStuckCounter();
                }
                else if (cpu->PC >= BIOS_ADDR &&
                         cpu->PC < (uint16_t)(BIOS_ADDR + 16 * 3) &&
                         (cpu->PC - BIOS_ADDR) % 3 == 0)
                {
                    BIOSCall(cpu, cpm);
                    dbg.resetStuckCounter();
                }
                else
                {
                    // Check breakpoint before executing.
                    int bpAddr = HexToByte(dbg.pauseLine);
                    if (dbg.breakpointActive && cpu->PC == bpAddr)
                    {
                        dbg.notHalted = false;
                        dbg.breakpointActive = false;
                    }
                    else
                    {
                        uint8_t op = cpu->memory[cpu->PC];
                        dbg.logInstruction(cpu);
                        if (useZ80)
                            ExecuteZ80OpCode(op, static_cast<zilogZ80 *>(cpu));
                        else
                            ExecuteOpCode(op, cpu);
                        throttleCycles += OPCODE_CYCLES[op];
                        dbg.tickStuck(cpu->PC, OPCODE_CYCLES[op]);
                        if (dbg.stuckDetected)
                            dbg.notHalted = false;

                        if (dbg.targetMHz > 0.0)
                        {
                            unsigned long long now = GetCurrentTime100ns();
                            double elapsed = (now - throttleEpoch) * 1e-7;
                            uint64_t allowed = (uint64_t)(dbg.targetMHz * 1e6 * elapsed);
                            if (throttleCycles > allowed)
                            {
                                double ahead = (throttleCycles - allowed) / (dbg.targetMHz * 1e6);
                                unsigned int us = (unsigned int)(ahead * 1e6);
                                if (us > 1000)
                                    us = 1000;
                                if (us > 0)
                                    usleep(us);
                            }
                            // Reset epoch every 10 s to avoid overflow
                            if (now - throttleEpoch > 100000000ULL)
                            {
                                throttleEpoch = now;
                                throttleCycles = 0;
                            }
                        }
                    }
                }

                if (dbg.runOnce)
                {
                    dbg.notHalted = false;
                    dbg.runOnce = false;
                }
            }

            currentTime = GetCurrentTime100ns();
            if ((currentTime - lastGuiUpdate) >= (unsigned long long)(10000000 / 60))
            {
                SerialTick(cpm);
                glfwPollEvents();
                glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGUIFrameCPM(cpm, cpu, dbg);
                glfwSwapBuffers(window);
                lastGuiUpdate = currentTime;
            }
        }

        SerialClose(cpm);
        g_serialState = nullptr;

        // Auto-save NVRAM on exit if enabled.
        if (dbg.nvramAutoSave)
            SaveCPMState(cpu, cpm, std::string(dbg.nvramPath));

        GraphicsCleanup(VAO, shaderProgram);
        return 0;
    }

    // ── Altair 8800 mode ─────────────────────────────────────────────────────
    if (config.mode == EmulatorMode::ALTAIR)
    {
        cpu->arcadeMode = false;

        CPMState cpm;
        cpm.serial.consoleMode = config.serialConsole;
        cpm.serial.port = config.serialPort ? config.serialPort : 2525;
        cpm.serial.baud = config.serialBaud;
        cpm.serial.fifoPath = config.serialFifoPath;
        g_serialState = &cpm;

        CPMDebugState dbg;
        dbg.notHalted = true;
        dbg.stuckDetectionEnabled = false; // BASIC polls port 0x00 in a tight loop

        unsigned long long throttleEpoch = GetCurrentTime100ns();
        uint64_t throttleCycles = 0;

        while (!glfwWindowShouldClose(window))
        {
            if (dbg.notHalted || dbg.runOnce)
            {
                int bpAddr = HexToByte(dbg.pauseLine);
                if (dbg.breakpointActive && cpu->PC == bpAddr)
                {
                    dbg.notHalted = false;
                    dbg.breakpointActive = false;
                }
                else
                {
                    uint8_t op = cpu->memory[cpu->PC];
                    dbg.logInstruction(cpu);
                    ExecuteOpCode(op, cpu);
                    throttleCycles += OPCODE_CYCLES[op];

                    if (dbg.targetMHz > 0.0)
                    {
                        unsigned long long now = GetCurrentTime100ns();
                        double elapsed = (now - throttleEpoch) * 1e-7;
                        uint64_t allowed = (uint64_t)(dbg.targetMHz * 1e6 * elapsed);
                        if (throttleCycles > allowed)
                        {
                            double ahead = (throttleCycles - allowed) / (dbg.targetMHz * 1e6);
                            unsigned int us = (unsigned int)(ahead * 1e6);
                            if (us > 1000) us = 1000;
                            if (us > 0) usleep(us);
                        }
                        if (now - throttleEpoch > 100000000ULL)
                        {
                            throttleEpoch = now;
                            throttleCycles = 0;
                        }
                    }
                }

                if (dbg.runOnce)
                {
                    dbg.notHalted = false;
                    dbg.runOnce = false;
                }
            }

            currentTime = GetCurrentTime100ns();
            if ((currentTime - lastGuiUpdate) >= (unsigned long long)(10000000 / 60))
            {
                SerialTick(cpm);
                glfwPollEvents();
                glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGUIFrameCPM(cpm, cpu, dbg);
                glfwSwapBuffers(window);
                lastGuiUpdate = currentTime;
            }
        }

        SerialClose(cpm);
        g_serialState = nullptr;
        GraphicsCleanup(VAO, shaderProgram);
        return 0;
    }

    // ── MSX mode ─────────────────────────────────────────────────────────────
    if (config.mode == EmulatorMode::MSX)
    {
        zilogZ80 *z80 = static_cast<zilogZ80 *>(cpu);
        cpu->arcadeMode = false;

        MSXState msx;
        memset(msx.keyMatrix, 0xFF, sizeof(msx.keyMatrix)); // all keys released
        msx.vdp.Reset();
        MSXInitMemory(msx, cpu);

        if (!config.msxCart1.empty() &&
            !MSXLoadCartridge(msx, cpu, 1, exeDir + config.msxCart1, config.msxCart1Addr))
            std::cerr << "MSX: failed to load cart1: " << exeDir + config.msxCart1 << std::endl;
        if (!config.msxCart1Ext.empty() &&
            !MSXLoadCartridge(msx, cpu, 1, exeDir + config.msxCart1Ext, config.msxCart1ExtAddr))
            std::cerr << "MSX: failed to load cart1_ext: " << exeDir + config.msxCart1Ext << std::endl;
        if (!config.msxCart2.empty() &&
            !MSXLoadCartridge(msx, cpu, 2, exeDir + config.msxCart2, config.msxCart2Addr))
            std::cerr << "MSX: failed to load cart2: " << exeDir + config.msxCart2 << std::endl;

        // Port handlers for MSX (overrides the empty registration done above)
        ClearPortHandlers();
        RegisterMSXPorts(msx, cpu);

        // Keyboard callback via window user pointer
        glfwSetWindowUserPointer(window, &msx);
        glfwSetKeyCallback(window, [](GLFWwindow *w, int key, int /*sc*/, int action, int /*mods*/) {
            MSXState *m = static_cast<MSXState *>(glfwGetWindowUserPointer(w));
            if (m) MSXKeyCallback(*m, key, action);
        });

        // Cycles to run per VBlank frame (approximate — 8080 cycle table is close enough for Z80)
        uint64_t cyclesPerFrame = (uint64_t)((float)config.cpuHz / config.interruptHz);

        // Frame-rate limiter: 100ns units
        unsigned long long frameInterval = (unsigned long long)(1e7f / config.interruptHz);
        unsigned long long lastFrameTime = GetCurrentTime100ns();

        // Pixel buffer and texture for TMS9918A output
        static uint8_t msxPixels[256 * 192 * 3];
        unsigned int msxTexture;
        glGenTextures(1, &msxTexture);
        unsigned int msxVAO = InitializeVAOMsxScreen();

        while (!glfwWindowShouldClose(window) && msx.running)
        {
            // Execute one frame's worth of Z80 instructions
            uint64_t ran = 0;
            while (ran < cyclesPerFrame)
            {
                uint8_t op = cpu->memory[cpu->PC];
                ExecuteZ80OpCode(op, z80);
                ran += OPCODE_CYCLES[op]; // cycle table approximation
            }

            // VBlank: set INT bit in VDP status register
            msx.vdp.status |= 0x80;

            // Trigger maskable INT if VDP INT enable (R1 bit 5) and IFF1
            if ((msx.vdp.regs[1] & 0x20) && z80->IFF1)
                ISR(cpu, 0x0038, z80);

            // Frame-rate limiting (target 50 or 60 Hz)
            unsigned long long now = GetCurrentTime100ns();
            long long remaining = (long long)frameInterval - (long long)(now - lastFrameTime);
            if (remaining > 10000) // > 1 ms remaining
                usleep((unsigned int)(remaining * 9 / 100)); // sleep ~90% of remaining (100ns→µs)
            lastFrameTime = GetCurrentTime100ns();

            // Render VDP frame
            glfwPollEvents();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            msx.vdp.RenderFrame(msxPixels);

            int fbW = 0, fbH = 0;
            glfwGetFramebufferSize(window, &fbW, &fbH);
            int scale = std::max(1, std::min(fbW / 256, fbH / 192));
            int viewW = 256 * scale;
            int viewH = 192 * scale;
            glViewport((fbW - viewW) / 2, (fbH - viewH) / 2, viewW, viewH);
            DrawMSXScreen(shaderProgram, msxVAO, msxTexture, msxPixels);

            // Minimal ImGui frame (no debug panel yet)
            glViewport(0, 0, fbW, fbH);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);
        }

        glDeleteTextures(1, &msxTexture);
        glDeleteVertexArrays(1, &msxVAO);
        GraphicsCleanup(VAO, shaderProgram);
        return 0;
    }

    // ── Arcade mode ──────────────────────────────────────────────────────────
    bool notHalted = false;
    bool runOnce = false;
    bool runFrame = true;

    int opcode = 0;
    int firstByte = 0;
    int secondByte = 0;

    int totalCycles = 0;

    // Derive timing from GameConfig so any arcade machine can override defaults.
    float arcadeFps       = config.interruptHz;
    float arcadeScanlines = (float)config.arcadeScanlines;
    float arcadeFreq      = (float)config.cpuHz;
    float arcadePeriod    = 1.0f / arcadeFps;
    float cycleperscanline = (arcadePeriod / arcadeScanlines) * arcadeFreq;

    long long int oneInstructionCycle = (long long int)(1e7f / arcadeFps);
    unsigned long long timePerFrame   = (unsigned long long)(1e7f / arcadeFps);

    unsigned int texture;
    glGenTextures(1, &texture);

    bool haltatInterrupt = true;

    bool midInterruptTriggered = false;
    bool cleared = false;
    uint8_t previous = 0;
    bool runNext = false;

    uint16_t previousCoin = 0;
    uint16_t previousSwitch = 0;

    // program loop
    while (!glfwWindowShouldClose(window))
    {
        // update times
        currentTime = GetCurrentTime100ns();

        if (oneInstructionCycle <= 10000)
        {
            oneInstructionCycle = 0;
        }

        // wait for next frame or instruction
        if (runFrame)
        {
            if (((currentTime - invadersUpdate) >= timePerFrame) && notHalted)
            {
                runNext = true;
                invadersUpdate = currentTime;
                totalCycles = (int)(cycleperscanline * arcadeScanlines);
            }
        }
        else
        {
            if (((currentTime - invadersUpdate) >= oneInstructionCycle) && notHalted)
            {
                runNext = true;
                invadersUpdate = currentTime;
                totalCycles = cpu->cycles + 1;
            }
        }

        // if time for next frame, execute opcodes and ISRs
        if (runNext)
        {
            // if(((currentTime - clockCycle) >= (unsigned long long) (oneInstructionCycle)) && notHalted){
            while ((int(cpu->cycles) < totalCycles) && notHalted)
            {

                runNext = false;

                PauseOnLineInt = HexToByte(PauseOnLine);
                if ((cpu->PC == PauseOnLineInt) && breakpointActive)
                {
                    notHalted = false;
                }

                if (breakpointActive)
                {
                    breakpointActive = false;
                    notHalted = false;
                }

                float halfFrameCycles = cycleperscanline * (arcadeScanlines * 0.5f);
                float fullFrameCycles = cycleperscanline * arcadeScanlines;

                if (!midInterruptTriggered && cpu->interrupts &&
                    cpu->cyclesInterrupt >= halfFrameCycles)
                {
                    ISR(cpu, config.rstMidFrame, useZ80 ? static_cast<zilogZ80 *>(cpu) : nullptr);
                    midInterruptTriggered = true;
                }

                if (midInterruptTriggered && cpu->interrupts &&
                    cpu->cyclesInterrupt >= fullFrameCycles)
                {
                    ISR(cpu, config.rstEndFrame, useZ80 ? static_cast<zilogZ80 *>(cpu) : nullptr);
                    midInterruptTriggered = false;
                    // mantém excesso de ciclos (evita drift)
                    cpu->cyclesInterrupt -= fullFrameCycles;
                    if (cpu->cyclesInterrupt < 0)
                        cpu->cyclesInterrupt = 0;
                }

                if (useZ80)
                    ExecuteZ80OpCode(cpu->memory[cpu->PC], static_cast<zilogZ80 *>(cpu));
                else
                    ExecuteOpCode(cpu->memory[cpu->PC], cpu);
                clockCycle = currentTime;

                // log instructions
                previousInstructions[(currentInstruction * 4)] = cpu->PC;
                previousInstructions[(currentInstruction * 4) + 1] = cpu->memory[cpu->PC];
                previousInstructions[(currentInstruction * 4) + 2] = cpu->memory[cpu->PC + 1];
                previousInstructions[(currentInstruction * 4) + 3] = cpu->memory[cpu->PC + 2];

                currentInstruction += 1;

                if (currentInstruction >= MAXINSTRUCTIONS)
                {
                    currentInstruction = 0;
                }

                if (runOnce)
                {
                    notHalted = false;
                    runOnce = false;
                }
            }
            cpu->cycles = 0;
        }

        // render GUI and process inputs

        // currentTime >>=
        if ((currentTime - lastGuiUpdate) >= (unsigned long long)((10000000 / 20)))
        {
            processInput(window, cpu); // input

            // render
            glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            DrawScreen(cpu, shaderProgram, VAO, texture, config.vramStart, config.vramEnd, config.screenW, config.screenH);

            ImGUIFrame(oneInstructionCycle, notHalted, runOnce, runFrame, cpu, PauseOnLine, breakpointActive, MAXINSTRUCTIONS, currentInstruction, previousInstructions);

            // glfw: swap buffers and poll IO events (keys pressed/released, mouse moved etc.)
            // -------------------------------------------------------------------------------
            glfwSwapBuffers(window);
            glfwPollEvents();

            // std::cout << currentTime - lastGuiUpdate << std::endl;
            lastGuiUpdate = currentTime;
        }
    }

    GraphicsCleanup(VAO, shaderProgram);

    return 0;
}
