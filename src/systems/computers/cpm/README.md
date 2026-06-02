# CP/M

Primeira maquina CP/M no novo core.

- RAM plana de 64 KB.
- Programas `.COM` carregam em `0x0100`.
- A CPU usada e `zilogZ80` conectada a `core::Z80Bus`.
- BDOS minimo interceptado em `0x0005` para smoke/debug inicial:
  - `C=0`: encerra;
  - `C=2`: imprime caractere em `E`;
  - `C=9`: imprime string em `DE` ate `$`.

Esta maquina ainda nao substitui o CP/M legado completo; ela existe para
validar a estrutura nova `src/systems/computers`.
