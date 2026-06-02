# SG-1000

Implementacao inicial adaptada para o novo core.

Estado atual:

- ROM em `0x0000-0x7fff`, com espelhamento para ROMs menores;
- area de expansao em `0x8000-0xbfff`;
- RAM interna de 1 KB espelhada em `0xc000-0xffff`;
- portas de VDP TMS9918A e controles basicos;
- PSG ainda e apenas latch/stub.
