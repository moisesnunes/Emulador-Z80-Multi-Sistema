
#include "gui.h"
#include "cpm_bios.h"

#include <iostream>
#include <cstring>
#include <cmath>
#include "shaderProgam.h"
// GUI headers
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui_memory_editor.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "cpm_debug_state.h"

static MemoryEditor mem_edit;
static CPMState *g_cpmState = nullptr;

#include "cpm_debug_ui.cpp"

// Callback para caracteres normais (espaço, letras, números, etc)
static void glfw_char_callback(GLFWwindow *window, unsigned int codepoint)
{
    if (!g_cpmState)
        return;

    // Aceita caracteres ASCII imprimíveis (0x20 até 0x7E)
    if (codepoint >= 0x20 && codepoint <= 0x7E)
    {
        g_cpmState->terminal.inputQueue.push_back((uint8_t)codepoint);
    }
}

// Callback para teclas especiais (Enter, Backspace, Setas, etc)
static void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    if (!g_cpmState)
        return;

    // Só processar quando tecla é pressionada/repetida (não solta)
    if (action == GLFW_RELEASE)
        return;

    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    // Ctrl+A até Ctrl+Z → códigos 0x01-0x1A
    if (ctrl && key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
    {
        uint8_t code = (uint8_t)(key - GLFW_KEY_A + 1);
        g_cpmState->terminal.inputQueue.push_back(code);
        return;
    }

    // Teclas especiais
    switch (key)
    {
    case GLFW_KEY_ENTER:                                 // Enter
    case GLFW_KEY_KP_ENTER:                              // Keypad Enter
        g_cpmState->terminal.inputQueue.push_back(0x0D); // CR
        break;
    case GLFW_KEY_BACKSPACE:                             // Backspace
        g_cpmState->terminal.inputQueue.push_back(0x08); // BS
        break;
    case GLFW_KEY_DELETE:                                // Delete
        g_cpmState->terminal.inputQueue.push_back(0x7F); // DEL
        break;
    case GLFW_KEY_TAB:                                   // Tab
        g_cpmState->terminal.inputQueue.push_back(0x09); // HT
        break;
    case GLFW_KEY_ESCAPE:                                // Escape
        g_cpmState->terminal.inputQueue.push_back(0x1B); // ESC
        break;
    // Setas como códigos WordStar
    case GLFW_KEY_UP: // Seta cima  → ^E
        g_cpmState->terminal.inputQueue.push_back(0x05);
        break;
    case GLFW_KEY_DOWN: // Seta baixo → ^X
        g_cpmState->terminal.inputQueue.push_back(0x18);
        break;
    case GLFW_KEY_LEFT: // Seta esq   → ^S
        g_cpmState->terminal.inputQueue.push_back(0x13);
        break;
    case GLFW_KEY_RIGHT: // Seta dir   → ^D
        g_cpmState->terminal.inputQueue.push_back(0x04);
        break;
    case GLFW_KEY_PAGE_UP: // PgUp → ^R (página anterior)
        g_cpmState->terminal.inputQueue.push_back(0x12);
        break;
    case GLFW_KEY_PAGE_DOWN: // PgDn → ^C (página seguinte)
        g_cpmState->terminal.inputQueue.push_back(0x03);
        break;
    case GLFW_KEY_HOME: // Home → ^QS (início da linha)
        g_cpmState->terminal.inputQueue.push_back(0x11);
        g_cpmState->terminal.inputQueue.push_back(0x13);
        break;
    case GLFW_KEY_END: // End  → ^QD (fim da linha)
        g_cpmState->terminal.inputQueue.push_back(0x11);
        g_cpmState->terminal.inputQueue.push_back(0x04);
        break;
    default:
        break;
    }
}

void RegisterCPMTerminalCallbacks(GLFWwindow *window, CPMState *cpmState)
{
    g_cpmState = cpmState;
    glfwSetCharCallback(window, glfw_char_callback);
    glfwSetKeyCallback(window, glfw_key_callback);
}

GLFWwindow *InitGUI(const std::string &title)
{
    // glewInit();
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(1000, 800, title.c_str(), NULL, NULL);
    if (window == NULL)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return NULL;
    }
    glfwMakeContextCurrent(window);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    // glad: load all OpenGL function pointers
    // ---------------------------------------
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return NULL;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig fontCfg;
    fontCfg.SizePixels = 13.0f;
    io.Fonts->AddFontDefault(&fontCfg);
    // Setup Platform/Renderer bindings
    ImGui_ImplGlfw_InitForOpenGL(window, false);
    ImGui_ImplOpenGL3_Init("#version 130");
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    return window;
}

// glfw: whenever the window size changed (by OS or user resize) this callback function executes
// ---------------------------------------------------------------------------------------------
void framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    // make sure the viewport matches the new window dimensions; note that width and
    // height will be significantly larger than specified on retina displays.
    glViewport(0, 0, width, height);
}

unsigned int InitializeShader()
{
    // build and compile our shader program
    // ------------------------------------
    // vertex shader
    unsigned int vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
    glCompileShader(vertexShader);
    // check for shader compile errors
    int success;
    char infoLog[512];
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        std::cerr << "ERROR::SHADER::VERTEX::COMPILATION_FAILED\n"
                  << infoLog << std::endl;
    }
    // fragment shader
    unsigned int fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);
    // check for shader compile errors
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        std::cerr << "ERROR::SHADER::FRAGMENT::COMPILATION_FAILED\n"
                  << infoLog << std::endl;
    }
    // link shaders
    unsigned int shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);
    // check for linking errors
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success)
    {
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        std::cerr << "ERROR::SHADER::PROGRAM::LINKING_FAILED\n"
                  << infoLog << std::endl;
    }
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return shaderProgram;
}

unsigned int InitializeVAO()
{ // float x, float y, float width, float height, float rotation){
    // set up vertex data (and buffer(s)) and configure vertex attributes
    // ------------------------------------------------------------------
    // float vertices[] = {
    //    -0.5f, -0.5f, 0.0f, // left
    //     0.5f, -0.5f, 0.0f, // right
    //     0.0f,  0.5f, 0.0f  // top
    //};

    float vertices[] = {
        // vertex x,y,z      //texture u, v
        0.2f, 0.9f, 0.0f, 1.0f, 0.0f,   // top left t1
        0.9f, 0.9f, 0.0f, 1.0f, 1.0f,   // top right t1
        0.2f, -0.52f, 0.0f, 0.0f, 0.0f, // bottom left t1
        0.2f, -0.52f, 0.0f, 0.0f, 0.0f, // bottom left t2
        0.9f, 0.9f, 0.0f, 1.0f, 1.0f,   // top right t2
        0.9f, -0.52f, 0.0f, 0.0f, 1.0f  // bottom right t2
    };

    /*
        float vertices[] = {
            //vertex x,y,z      //texture u, v
            -0.535f,    1.61f,  0.0f,   1.0f, 0.0f, //top left t1
             1.635f,    1.61f,  0.0f,   1.0f, 1.0f, //top right t1
            -0.535f,   -1.23f, 0.0f,   0.0f, 0.0f, //bottom left t1
            -0.535f,   -1.23f, 0.0f,   0.0f, 0.0f, //bottom left t2
             1.635f,    1.61f,  0.0f,   1.0f, 1.0f, //top right t2
             1.635f,   -1.23f, 0.0f,   0.0f, 1.0f  //bottom right t2
        };
    */
    /*
        float vertices[] = {
            //vertex x,y,z      //texture u, v
            -1.0f,  1.0f, 0.0f,   0.0f, 1.0f, //top left t1
             1.0f,  1.0f, 0.0f,   1.0f, 1.0f, //top right t1
            -1.0f, -1.0f, 0.0f,   0.0f, 0.0f, //bottom left t1
            -1.0f, -1.0f, 0.0f,   0.0f, 0.0f, //bottom left t2
             1.0f,  1.0f, 0.0f,   1.0f, 1.0f, //top right t2
             1.0f, -1.0f, 0.0f,   1.0f, 0.0f  //bottom right t2
        };
    */
    // glm::mat4 ortho = glm::mat4(1.0f);
    // ortho = glm::ortho(0.0f, 1920.0f, 0.0f, 1080.0f, -1.0f, 100.0f);
    // glm::mat4 trans = glm::mat4(1.0f);
    // trans = glm::translate(trans, glm::vec3(x, y, 0.0f));
    // trans = glm::rotate(trans, glm::radians(rotation), glm::vec3(0.0f, 0.0f, 1.0f));
    // trans = glm::scale(trans, glm::vec3(0.5, 0.5, 0.0));

    unsigned int VBO, VAO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    // bind the Vertex Array Object first, then bind and set vertex buffer(s), and then configure vertex attributes(s).
    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    // note that this is allowed, the call to glVertexAttribPointer registered VBO as the vertex attribute's bound vertex buffer object so afterwards we can safely unbind
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // You can unbind the VAO afterwards so other VAO calls won't accidentally modify this VAO, but this rarely happens. Modifying other
    // VAOs requires a call to glBindVertexArray anyways so we generally don't unbind VAOs (nor VBOs) when it's not directly necessary.
    glBindVertexArray(0);

    // uncomment this call to draw in wireframe polygons
    // glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    return VAO;
}

unsigned int InitializeVAOMsxScreen()
{
    // NDC tela cheia; UV alinhado ao glTexImage2D: primeira linha do buffer = base da textura (v=0).
    // O buffer MSX é y=0 no topo do CRT → precisa v=0 no topo da janela.
    float vertices[] = {
        -1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 0.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 0.0f, 1.0f, 0.0f,
        1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
    };

    unsigned int VBO, VAO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    return VAO;
}

void RenderDebugData(long long &oneInstructionCycle, bool &notHalted, bool &runOnce, bool &runFrame, intel8080 *cpu, char *PauseOnLine, bool &breakpointActive, int MAXINSTRUCTIONS, int *previousInstructions)
{
    int opcode = cpu->memory[cpu->PC];
    int firstByte = cpu->memory[cpu->PC + 1];
    int secondByte = cpu->memory[cpu->PC + 2];

    ImGui::Begin("Register Info");

    // PC / SP em amarelo/dourado
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f),
                       "PC: %04X    SP: %04X", cpu->PC, cpu->SP);
    ImGui::Separator();

    // Instrução atual destacada em amarelo
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                       ">> %04X: %02X %02X %02X   %s",
                       cpu->PC, opcode, firstByte, secondByte, DISSAMBLER_STATES[opcode]);

    ImGui::Separator();

    // Registradores
    ImGui::Text("Reg A: %02X", cpu->A);
    ImGui::Text("Reg B: %02X", cpu->B);
    ImGui::Text("Reg C: %02X", cpu->C);
    ImGui::Text("Reg D, E: %02X %02X", cpu->D, cpu->E);
    ImGui::Text("Reg H, L: %02X %02X", cpu->H, cpu->L);

    ImGui::Separator();

    // Flags coloridas: verde=set, cinza=clear
    auto flagColor = [](bool on) -> ImVec4
    {
        return on ? ImVec4(0.20f, 1.00f, 0.20f, 1.0f)
                  : ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
    };
    ImGui::Text("Flags:");
    ImGui::SameLine();
    ImGui::TextColored(flagColor(cpu->sf), "[S]");
    ImGui::SameLine();
    ImGui::TextColored(flagColor(cpu->zf), "[Z]");
    ImGui::SameLine();
    ImGui::TextColored(flagColor(cpu->acf), "[AC]");
    ImGui::SameLine();
    ImGui::TextColored(flagColor(cpu->pf), "[P]");
    ImGui::SameLine();
    ImGui::TextColored(flagColor(cpu->cf), "[C]");
    ImGui::NewLine();

    ImGui::Separator();

    // Interrupções: verde=habilitado, vermelho=desabilitado
    ImGui::Text("Interrupts:");
    ImGui::SameLine();
    ImGui::TextColored(
        cpu->interrupts ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                        : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
        "%s", cpu->interrupts ? "ENABLED" : "DISABLED");

    ImGui::Separator();

    ImGui::Text("CycleTime: %lld mS", (oneInstructionCycle / 10000));

    ImGui::Text("Pause on Line:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputText("##pauseline", PauseOnLine, 5, ImGuiInputTextFlags_CharsHexadecimal);

    if (ImGui::Button("Breakpoint", ImVec2(90, 0)))
    {
        std::cout << "Breakpoint" << std::endl;
        breakpointActive ^= 1;
    }
    if (breakpointActive)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.1f, 1.0f), "@ 0x%s", PauseOnLine);
    }

    if (ImGui::Button("Reset", ImVec2(65, 0)))
    {
        cpu->PC = 0;
        for (int address = 0x2000; address <= 0xFFFF; address++)
            cpu->WriteMem((uint16_t)address, 0);
        for (int address = 0; address <= MAXINSTRUCTIONS; address++)
            previousInstructions[address] = 0;
    }

    if (ImGui::Button(notHalted ? "  Pause  " : "   Run   ", ImVec2(90, 0)))
        notHalted ^= 1;

    ImGui::SameLine();
    if (ImGui::Button("Step", ImVec2(65, 0)))
    {
        notHalted = true;
        runOnce = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("ISR", ImVec2(50, 0)))
        cpu->interrupts ^= 1;

    if (ImGui::Button("Cycle Up"))
    {
        oneInstructionCycle *= (unsigned long long)2;
        if (oneInstructionCycle == 0)
            oneInstructionCycle = 10000000;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cycle Down"))
        oneInstructionCycle /= (unsigned long long)2;

    ImGui::SameLine();
    if (ImGui::Button("Frame/Instr"))
        runFrame ^= 1;

    ImGui::Separator();

    // Barra de status: verde=running, laranja=paused
    ImGui::TextColored(
        notHalted ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                  : ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
        "%-8s", notHalted ? "RUNNING" : "PAUSED");

    mem_edit.DrawWindow("Memory Edit", cpu->memory, 0xFFFF);

    ImGui::End();
}

// read vram and print to screen
// read ram
// create texture map from vram
// output to graphics system

void DrawScreen(intel8080 *cpu, unsigned int shaderProgram, unsigned int VAO, unsigned int texture,
                int startVRAM, int endVRAM, int width, int height)
{

    // unsigned char *display = new unsigned char[(endVRAM - startVRAM)*24];
    unsigned char *display = new unsigned char[width * height * 3];

    int VRAMLength = (endVRAM - startVRAM);

    for (int i = 0; i < VRAMLength; i++)
    {
        unsigned int currentByte = cpu->memory[startVRAM + i];
        for (int k = 0; k < 8; k++)
        {
            int indexOffset = ((i * 8) + (k)) * 3;
            if (((currentByte >> k) & 0x01) == 0)
            {
                display[indexOffset + 0] = 0x00;
                display[indexOffset + 1] = 0x00;
                display[indexOffset + 2] = 0x00;
            }
            else
            {
                display[indexOffset + 0] = 0x00;
                display[indexOffset + 1] = 0xC0;
                display[indexOffset + 2] = 0xC0;
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); // set texture wrapping to GL_REPEAT (default wrapping method)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    // set texture filtering parameters
    // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, height, width, 0, GL_RGB, GL_UNSIGNED_BYTE, display);
    glGenerateMipmap(GL_TEXTURE_2D);

    glUseProgram(shaderProgram);
    glBindVertexArray(VAO); // seeing as we only have a single VAO there's no need to bind it every time, but we'll do so to keep things a bit more organized
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// Upload a pre-rendered 256×192 RGB frame (from the TMS9918A) and draw it.
void DrawMSXScreen(unsigned int shaderProgram, unsigned int VAO, unsigned int texture,
                   const uint8_t *rgb256x192)
{
    glUseProgram(shaderProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 256, 192, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb256x192);
    glUniform1i(glGetUniformLocation(shaderProgram, "inputTexture"), 0);
    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void ImGUIFrame(long long &oneInstructionCycle, bool &notHalted, bool &runOnce, bool &runFrame, intel8080 *cpu, char *PauseOnLine, bool &breakpointActive, int MAXINSTRUCTIONS, int currentInstruction, int *previousInstructions)
{
    // feed inputs to dear imgui, start new frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    RenderDebugData(oneInstructionCycle, notHalted, runOnce, runFrame, cpu, PauseOnLine, breakpointActive, MAXINSTRUCTIONS, previousInstructions);

    ImGui::Begin("Last Instructions");

    for (int k = 0; k < MAXINSTRUCTIONS; k++)
    {
        int index = currentInstruction - k - 1;
        if (index < 0)
            index = MAXINSTRUCTIONS + index;

        if (k == 0)
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                               "> %04X %02X %02X %02X  %s",
                               previousInstructions[(index * 4)],
                               previousInstructions[(index * 4) + 1],
                               previousInstructions[(index * 4) + 2],
                               previousInstructions[(index * 4) + 3],
                               DISSAMBLER_STATES[previousInstructions[(index * 4) + 1]]);
        else
            ImGui::Text("  %04X %02X %02X %02X  %s",
                        previousInstructions[(index * 4)],
                        previousInstructions[(index * 4) + 1],
                        previousInstructions[(index * 4) + 2],
                        previousInstructions[(index * 4) + 3],
                        DISSAMBLER_STATES[previousInstructions[(index * 4) + 1]]);
    }

    ImGui::End();

    ImGui::Begin("Important Stuff");
    ImGui::Text("Credits: %02X", cpu->memory[0x20EB]);
    ImGui::Text("CoinSwitch: %02X", cpu->memory[0x20EA]);
    ImGui::Text("GameMode: %02X", cpu->memory[0x20EF]);
    ImGui::Text("Suspend: %02X", cpu->memory[0x20E9]);
    ImGui::Text("Port1: %02X", cpu->IOPorts[1]);
    ImGui::Text("Port2: %02X", cpu->IOPorts[2]);

    ImGui::End();

    // Render dear imgui into screen
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ── CP/M terminal rendering ───────────────────────────────────────────────────

void DrawTerminal(CPMState &cpm)
{
    // Estilo preto e branco — fundo preto com texto branco
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.00f, 0.00f, 0.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.20f, 0.20f, 0.20f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.10f, 0.10f, 0.10f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.50f, 0.50f, 0.50f, 1.00f));

    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::Begin("CP/M 2.2  [Intel 8080]", nullptr,
                 ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoScrollbar);

    ImVec2 origin = ImGui::GetCursorScreenPos();

    for (int row = 0; row < TerminalState::ROWS; row++)
    {
        const char *start = cpm.terminal.buffer[row];
        ImGui::TextUnformatted(start, start + TerminalState::COLS);
    }

    // Cursor piscante (bloco branco)
    float blink = fmodf((float)ImGui::GetTime() * 2.0f, 1.0f);
    if (blink < 0.70f)
    {
        ImVec2 charSize = ImGui::CalcTextSize("A");
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float cx = origin.x + cpm.terminal.cursorX * charSize.x;
        float cy = origin.y + cpm.terminal.cursorY * lineH;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(cx, cy),
            ImVec2(cx + charSize.x, cy + charSize.y),
            IM_COL32(220, 220, 220, 210));
    }

    // Status bar
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.70f, 1.0f),
                       " %c:  User:%d  |  Queue:%zu  |  CCP:%s  |  Cursor:(%d,%d) ",
                       'A' + cpm.currentDrive,
                       cpm.currentUser,
                       cpm.terminal.inputQueue.size(),
                       cpm.ccpRunning ? "ON" : "OFF",
                       cpm.terminal.cursorX,
                       cpm.terminal.cursorY);

    ImGui::End();
    ImGui::PopStyleColor(5);
}

static void DrawPrinterWindow(CPMState &cpm)
{
    if (!cpm.printerWindowOpen)
        return;
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Impressora Virtual (LPT)", &cpm.printerWindowOpen))
    {
        ImGui::End();
        return;
    }

    // Scrollable read-only text with auto-scroll when new content arrives.
    static size_t lastLen = 0;
    bool newContent = cpm.printerBuffer.size() != lastLen;
    lastLen = cpm.printerBuffer.size();

    ImGui::BeginChild("##lptscroll", ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()), false);
    ImGui::TextUnformatted(cpm.printerBuffer.c_str(),
                           cpm.printerBuffer.c_str() + cpm.printerBuffer.size());
    if (newContent)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (ImGui::Button("Limpar"))
    {
        cpm.printerBuffer.clear();
        lastLen = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Abrir arquivo"))
    {
        std::string path = !cpm.printerPath.empty()
                               ? cpm.printerPath
                               : (!cpm.diskDirs[0].empty() ? cpm.diskDirs[0] : ".") + "/CPM.LST";
        system(("xdg-open " + path + " &").c_str());
    }
    ImGui::End();
}

void ImGUIFrameCPM(CPMState &cpm, intel8080 *cpu, CPMDebugState &dbg)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    DrawTerminal(cpm);
    DrawCPMDebugger(cpu, cpm, dbg);
    DrawPrinterWindow(cpm);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GraphicsCleanup(unsigned int VAO, unsigned int shaderProgram)
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // optional: de-allocate all resources once they've outlived their purpose:
    // ------------------------------------------------------------------------
    glDeleteVertexArrays(1, &VAO);
    // glDeleteBuffers(1, &VBO);
    glDeleteProgram(shaderProgram);

    // glfw: terminate, clearing all previously allocated GLFW resources.
    // ------------------------------------------------------------------
    glfwTerminate();
}
