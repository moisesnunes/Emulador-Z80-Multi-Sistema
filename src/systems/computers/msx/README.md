# MSX

Implementacao nova e separada do MSX legado em `msx_machine.*`.

Objetivo desta pasta:

- seguir uma estrutura de maquina/bus parecida com a do ares;
- manter BIOS, cartucho, expansao e RAM como slots logicos;
- expor portas VDP, PSG, PPI/slot e teclado pelo contrato `Z80Bus`;
- evitar dependencia de GLFW, ImGui ou do executavel antigo.

Estado atual:

- MSX1 parcial;
- cartucho linear simples em `0x4000` ou auto `0x8000` para ROM de 16 KB com
  header `AB`;
- sem mappers MSX avancados;
- CPU ainda sera conectada em uma etapa seguinte.
