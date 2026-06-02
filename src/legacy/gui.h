#ifndef GUI_H
#define GUI_H

#include <string>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "intel8080.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui_memory_editor.h"

// Forward declaration — full definition is in cpm_bios.h, included by GUI.cpp.
struct CPMState;

void RegisterCPMTerminalCallbacks(GLFWwindow *window, CPMState *cpmState);

GLFWwindow *InitGUI(const std::string &title);
void framebuffer_size_callback(GLFWwindow *window, int width, int height);
unsigned int InitializeShader();
unsigned int InitializeVAO();
// Quad em tela cheia com UV "normais" (sem rotação do gabinete Space Invaders).
unsigned int InitializeVAOMsxScreen();

void ImGUIFrame(long long &oneInstructionCycle, bool &notHalted, bool &runOnce, bool &runFrame, intel8080 *cpu, char *PauseOnLine,
                bool &breakpointActive, int MAXINSTRUCTIONS, int currentInstruction, int *previousInstructions);

void RenderDebugData(long long &oneInstructionCycle, bool &notHalted, bool &runOnce, bool &runFrame, intel8080 *cpu, char *PauseOnLine,
                     bool &breakpointActive, int MAXINSTRUCTIONS, int *previousInstructions);

void DrawScreen(intel8080 *cpu, unsigned int shaderProgram, unsigned int VAO, unsigned int texture,
                int vramStart, int vramEnd, int screenW, int screenH);

// Render a pre-built 256×192 RGB pixel buffer (from the TMS9918A) to the quad.
void DrawMSXScreen(unsigned int shaderProgram, unsigned int VAO, unsigned int texture,
                   const uint8_t *rgb256x192);

void GraphicsCleanup(unsigned int VAO, unsigned int shaderProgram);

// ── CP/M debug state ──────────────────────────────────────────────────────────
#include "cpm_debug_state.h"

// Render the 80×24 virtual terminal into an ImGui window and push keyboard
// events captured this frame into cpm.terminal.inputQueue.
void DrawTerminal(CPMState &cpm);

// Full ImGui frame for CP/M mode: terminal + debug panel side by side.
// dbg is read/written by the debug panel and obeyed by the main loop.
void ImGUIFrameCPM(CPMState &cpm, intel8080 *cpu, CPMDebugState &dbg);

#endif
