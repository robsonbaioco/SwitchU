# SwitchU 2.13.2

**Fixes 2.13.1, which does not boot.** 2.13.1 shrank the daemon's heap to 1 MB, and Horizon only accepts heap sizes in multiples of 2 MB: the daemon (SwitchU's replacement for the HOME menu) aborted before it could run anything, and the console stopped at error 2128-0100 (program 0100000000000023) on every boot. 2.13.1 has been pulled back to a pre-release. 2.13.2 uses 2 MB, which still gives 6 MB back to the System pool. Everything else in 2.13.1 is included.

## English

### If your console stops at 2128-0100 after 2.13.1

The console cannot reach SwitchU or the Homebrew Menu, so the files go onto the SD card from a PC:

1. Turn the console off (hold POWER for 12 seconds) and put the SD card in a PC — or boot hekate and use **Tools > USB Tools > SD Card** to mount it over USB.
2. Extract **SwitchU-2.13.2.zip** to the root of the SD card, overwriting the files (`atmosphere/` and `switch/` folders).
3. Boot normally.

### What is in 2.13.2

- Daemon heap: 2 MB (was 8 MB up to 2.13.0, 1 MB in the broken 2.13.1).
- From 2.13.1: Vibration in Quick Settings, L3 fallback and logging, SteamGridDB backdrop top-up, the "Restore default" notice.

---

## Português

**Corrige a 2.13.1, que não inicia.** A 2.13.1 reduziu o heap do daemon para 1 MB, e o Horizon só aceita tamanhos de heap múltiplos de 2 MB: o daemon (o substituto do SwitchU para o menu HOME) abortava antes de rodar qualquer coisa, e o console parava no erro 2128-0100 (programa 0100000000000023) em todo boot. A 2.13.1 voltou a ser pré-release. A 2.13.2 usa 2 MB, o que ainda devolve 6 MB ao pool System. Todo o resto da 2.13.1 está incluído.

### Se o seu console parou no 2128-0100 depois da 2.13.1

O console não chega ao SwitchU nem ao Homebrew Menu, então os arquivos vão para o cartão SD pelo PC:

1. Desligue o console (segure POWER por 12 segundos) e coloque o cartão SD no PC — ou entre no hekate e use **Tools > USB Tools > SD Card** para montá-lo pelo USB.
2. Extraia o **SwitchU-2.13.2.zip** na raiz do cartão SD, substituindo os arquivos (pastas `atmosphere/` e `switch/`).
3. Inicie normalmente.

### O que tem na 2.13.2

- Heap do daemon: 2 MB (era 8 MB até a 2.13.0, 1 MB na 2.13.1 quebrada).
- Da 2.13.1: Vibração nas configurações rápidas, contorno e logs do L3, fundo do SteamGridDB completado, aviso do "Restaurar padrão".
