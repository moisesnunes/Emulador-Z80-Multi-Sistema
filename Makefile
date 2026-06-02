CXX      = g++
CC       = gcc
SDL3_CFLAGS  := $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LDFLAGS := $(shell pkg-config --libs sdl3 2>/dev/null)

CXXFLAGS = -std=c++17 -g -MMD -MP -I./include/ -I./include/GL/ -I./include/imgui/ -I./lib/imgui/ -I./src -I./src/legacy -I./ $(SDL3_CFLAGS)
CFLAGS   = -g -MMD -MP -I./include/ -I./include/GL/
LDFLAGS  = -lglfw -lGL -ldl -lpthread

TARGET         = Emulator
DESKTOP_TARGET = EmulatorDesktop
BUILD_DIR      = build

CXX_SRCS = src/legacy/emulador.cpp \
           hexbyte.cpp \
           intel8080.cpp \
           zilogZ80.cpp \
           alu.cpp \
           src/legacy/gui.cpp \
           src/legacy/input.cpp \
           game_config.cpp \
           src/legacy/cpm_bios.cpp \
           src/legacy/cpm_ccp.cpp \
           src/legacy/cpm_debug_state.cpp \
           src/legacy/msx_machine.cpp \
           lib/imgui/imgui.cpp \
           lib/imgui/imgui_draw.cpp \
           lib/imgui/imgui_tables.cpp \
           lib/imgui/imgui_widgets.cpp \
           lib/imgui/imgui_demo.cpp \
           lib/imgui/imgui_impl_opengl3.cpp \
           lib/imgui/imgui_impl_glfw.cpp

C_SRCS = src/glad.c

OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CXX_SRCS)) \
       $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS))

DESKTOP_CXX_SRCS = src/app/desktop/main.cpp \
                   src/app/desktop/application.cpp \
                   src/app/desktop/gui.cpp \
                   src/app/desktop/emu_frontend.cpp \
                   src/core/machine_factory.cpp \
                   src/core/media/binary_loader.cpp \
                   src/core/system_catalog.cpp \
                   src/core/video/tms9918a.cpp \
                   src/systems/arcade/invaders/invaders_machine.cpp \
                   src/systems/computers/cpm/cpm_machine.cpp \
                   src/systems/computers/msx/msx_ares_machine.cpp \
                   src/systems/consoles/sg1000/sg1000_machine.cpp \
                   src/systems/consoles/mastersystem/master_system_machine.cpp \
                   intel8080.cpp \
                   zilogZ80.cpp \
                   alu.cpp \
                   game_config.cpp \
                   hexbyte.cpp \
                   lib/imgui/imgui.cpp \
                   lib/imgui/imgui_draw.cpp \
                   lib/imgui/imgui_tables.cpp \
                   lib/imgui/imgui_widgets.cpp \
                   lib/imgui/imgui_impl_opengl3.cpp

DESKTOP_C_SRCS = src/glad.c

DESKTOP_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/desktop/%.o,$(DESKTOP_CXX_SRCS)) \
               $(patsubst %.c,$(BUILD_DIR)/desktop/%.o,$(DESKTOP_C_SRCS))

DESKTOP_LDFLAGS = -lGL -ldl -lpthread $(SDL3_LDFLAGS)

# ── diagnostic test runner (headless, no GUI) ─────────────────────────────────
DIAG_RUNNER  = tests/diag_runner
DIAG_OBJS    = $(BUILD_DIR)/tests/diag_runner.o \
               $(BUILD_DIR)/intel8080.o \
               $(BUILD_DIR)/zilogZ80.o \
               $(BUILD_DIR)/alu.o \
               $(BUILD_DIR)/game_config.o \
               $(BUILD_DIR)/hexbyte.o

Z80_BUS_SMOKE = tests/z80_bus_smoke
Z80_BUS_OBJS  = $(BUILD_DIR)/tests/z80_bus_smoke.o \
                $(BUILD_DIR)/intel8080.o \
                $(BUILD_DIR)/zilogZ80.o \
                $(BUILD_DIR)/alu.o \
                $(BUILD_DIR)/game_config.o \
                $(BUILD_DIR)/hexbyte.o

Z80_MACHINE_SMOKE = tests/z80_machine_smoke
Z80_MACHINE_OBJS  = $(BUILD_DIR)/tests/z80_machine_smoke.o \
                    $(BUILD_DIR)/src/core/media/binary_loader.o \
                    $(BUILD_DIR)/src/core/video/tms9918a.o \
                    $(BUILD_DIR)/src/systems/arcade/invaders/invaders_machine.o \
                    $(BUILD_DIR)/src/systems/computers/cpm/cpm_machine.o \
                    $(BUILD_DIR)/src/systems/computers/msx/msx_ares_machine.o \
                    $(BUILD_DIR)/src/systems/consoles/sg1000/sg1000_machine.o \
                    $(BUILD_DIR)/src/systems/consoles/mastersystem/master_system_machine.o \
                    $(BUILD_DIR)/intel8080.o \
                    $(BUILD_DIR)/zilogZ80.o \
                    $(BUILD_DIR)/alu.o \
                    $(BUILD_DIR)/game_config.o \
                    $(BUILD_DIR)/hexbyte.o

DIAG_COMS    = tests/roms/8080PRE.COM tests/roms/8080EXM.COM
Z80_COMS     = tests/roms/zexdoc.com tests/roms/zexall.com
DEPFILES     = $(OBJS:.o=.d) \
               $(DESKTOP_OBJS:.o=.d) \
               $(DIAG_OBJS:.o=.d) \
               $(Z80_BUS_OBJS:.o=.d) \
               $(Z80_MACHINE_OBJS:.o=.d)

.PHONY: all clean desktop diag_runner z80_bus_smoke z80_machine_smoke test-8080 test-z80 test-z80-all

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $^ -o $@ $(LDFLAGS)

desktop: $(DESKTOP_TARGET)

$(DESKTOP_TARGET): $(DESKTOP_OBJS)
	$(CXX) $^ -o $@ $(DESKTOP_LDFLAGS)

# Build the headless runner
diag_runner: $(DIAG_RUNNER)

$(DIAG_RUNNER): $(DIAG_OBJS)
	$(CXX) $^ -o $@

z80_bus_smoke: $(Z80_BUS_SMOKE)
	$(Z80_BUS_SMOKE)

$(Z80_BUS_SMOKE): $(Z80_BUS_OBJS)
	$(CXX) $^ -o $@

z80_machine_smoke: $(Z80_MACHINE_SMOKE)
	$(Z80_MACHINE_SMOKE)

$(Z80_MACHINE_SMOKE): $(Z80_MACHINE_OBJS)
	$(CXX) $^ -o $@

$(BUILD_DIR)/tests/diag_runner.o: tests/diag_runner.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/z80_bus_smoke.o: tests/z80_bus_smoke.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/z80_machine_smoke.o: tests/z80_machine_smoke.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Run diagnostics (build runner first, then execute each ROM)
test-8080: $(DIAG_RUNNER)
	@echo "=== 8080PRE (sanity) ===" && \
	  $(DIAG_RUNNER) tests/roms/8080PRE.COM 50 || true
	@echo "=== 8080EXM (exerciser) ===" && \
	  $(DIAG_RUNNER) tests/roms/8080EXM.COM 25000 || true

# Z80 exerciser — zexdoc tests documented instructions (~46B cycles)
test-z80: $(DIAG_RUNNER)
	@echo "=== zexdoc (Z80 documented) ===" && \
	  $(DIAG_RUNNER) tests/roms/zexdoc.com z80 50000 || true

# Full Z80 exerciser — zexall includes undocumented instructions (~46B cycles)
test-z80-all: $(DIAG_RUNNER)
	@echo "=== zexall (Z80 all) ===" && \
	  $(DIAG_RUNNER) tests/roms/zexall.com z80 50000 || true

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/desktop/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/desktop/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(DESKTOP_TARGET) $(DIAG_RUNNER) $(Z80_BUS_SMOKE) $(Z80_MACHINE_SMOKE)

-include $(DEPFILES)
