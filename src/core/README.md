# Core

Componentes compartilhados do emulador.

Planejado:

- `cpu/z80`: Z80 isolado do mapa de memoria da maquina.
- `cpu/i8080`: Intel 8080 preservado para arcade/CPM/Altair.
- `bus`: interfaces de memoria e portas.
- `video`: VDPs e framebuffers compartilhados.
- `audio`: PSG e mixers.
- `input`: teclado, joystick e gamepad.
- `media`: carregamento de ROM, disco e cartucho.

Arquivos iniciais:

- `machine.h`: contrato comum para MSX, CP/M, SG-1000, Master System e arcade.
- `bus/z80_bus.h`: contrato de barramento usado pelo Z80.
- `video_frame.h`: descricao simples de framebuffer entregue ao frontend.
- `system_catalog.*`: lista inicial de sistemas disponiveis por familia.

CPU Z80:

- `zilogZ80` ainda vive na raiz durante a migracao, mas ja aceita
  `core::Z80Bus` via `AttachBus`.
- O teste `make z80_bus_smoke` valida fetch, leitura/escrita de memoria e
  `IN/OUT` pelo bus, alem da entrada basica de interrupcao maskable.
- MSX, SG-1000 e Master System ja possuem uma instancia privada de `zilogZ80`
  ligada ao proprio `Z80Bus`; `make z80_machine_smoke` valida esse caminho com
  uma escrita simples em RAM e um handler de VBlank.
- CP/M tambem ja possui uma maquina no novo core, carregando `.COM` em `0x0100`
  e interceptando BDOS minimo em `0x0005` para saida de terminal.
- `ExecuteZ80Next` retorna ciclos aproximados consumidos e
  `ExecuteZ80ForCycles` roda a CPU ate um orçamento de ciclos; as maquinas novas
  usam `cpu_hz / 60` em vez de um numero fixo de instruções por frame.
- Space Invaders ja possui uma maquina arcade no novo core, usando `intel8080`
  diretamente e mantendo suas regras de hardware fora dos computadores/consoles.

Interrupcao de video:

- `core::Tms9918a::interruptLine()` fica ativa quando o status de VBlank esta
  setado e `R1 bit 5` habilita interrupcao.
- As maquinas Z80 chamam `zilogZ80::MaskableInterrupt()` no fim do frame quando
  essa linha esta ativa.
