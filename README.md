# Emulador-Z80-Multi-Sistema

Emulador de maquinas e sistemas que usam o Z80, organizado em torno de um core compartilhado e maquinas separadas por familia.

## Estrutura

- `src/core`: interfaces comuns, bus Z80, video, catalogo e factory.
- `src/systems/computers`: computadores como CP/M e MSX.
- `src/systems/consoles`: consoles como SG-1000 e Master System.
- `src/systems/arcade`: placas arcade, como Space Invaders.
- `src/app/desktop`: frontend SDL3/ImGui do novo caminho.
- `src/legacy`: emulador antigo preservado durante o refactor.

## Build

```sh
make desktop
make z80_bus_smoke
make z80_machine_smoke
```

O alvo legado continua disponivel com:

```sh
make all
```
