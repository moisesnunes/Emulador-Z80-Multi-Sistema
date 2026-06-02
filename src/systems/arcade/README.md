# Arcade

Placas arcade ficam separadas de computadores e consoles.

O alvo inicial desta familia e preservar Space Invaders sem misturar suas
regras de hardware com MSX, CP/M ou consoles.

- `invaders`: Intel 8080, ROMs em `0x0000-0x1fff`, VRAM bitmap em
  `0x2400-0x3fff`, portas de input 1/2 e shift-register nas portas 2/3/4.
