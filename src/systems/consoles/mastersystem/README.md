# Master System

Implementacao inicial adaptada para o novo core.

Estado atual:

- mapper Sega basico com bancos de 16 KB;
- registradores de mapper em `0xfffc-0xffff`;
- RAM interna de 8 KB espelhada em `0xc000-0xffff`;
- portas principais de VDP, PSG e controles;
- VDP esta em modo compatibilidade/stub e sera substituido por um VDP SMS real.
