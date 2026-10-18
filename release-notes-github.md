# SwitchU 2.12.1

A fix for the menu's own internet access, broken since 2.11.0: the Theme Shop, the update check and Send logs all failed with "no connection" while the console's Wi-Fi itself worked. The SwitchU Menu icon is back to the color palette.

## English

### Internet in the menu works again

- Since 2.11.0 the automatic clock sync ran a few seconds after boot, before anything else needed the network, and switched the console's socket service on without ever switching it off. libnx does not count those calls, so every later HTTP request — Theme Shop catalog and downloads, the update check, SteamGridDB, game details and Send logs — failed with `0x00000F59` (already initialized) for the rest of the session. The console's Wi-Fi, the clock sync and the connection applet were never affected.
- Both now share one reference-counted socket runtime: the first user switches it on, the last one switches it off. The clock sync holds it (and its NIFM session) only for the query.
- **Updating from 2.11.0 or 2.12.0:** the menu's own updater cannot download this release, since it is the thing that was broken. Open **SwitchU Manager** from the Homebrew Menu and update from there (it has its own network code), or copy the zip's `atmosphere/` and `switch/` folders to the SD card over MTP or a card reader.

### Icon

- The SwitchU Menu entry in the sidebar and the XMB view uses the color-palette icon again; 2.11.0 had replaced it with a Joy-Con artwork.

---

## Português

Uma correção para a internet do próprio menu, quebrada desde a 2.11.0: a Loja de Temas, a busca de atualização e o Enviar logs falhavam com "sem conexão" enquanto o Wi-Fi do console funcionava. O ícone do SwitchU Menu voltou a ser a paleta de cores.

### A internet do menu volta a funcionar

- Desde a 2.11.0 a sincronização automática do relógio rodava alguns segundos depois do boot, antes de qualquer outra coisa precisar da rede, e ligava o serviço de sockets do console sem nunca desligar. A libnx não conta essas chamadas, então toda requisição HTTP seguinte — catálogo e downloads da Loja de Temas, busca de atualização, SteamGridDB, detalhes de jogos e Enviar logs — falhava com `0x00000F59` (já inicializado) até o fim da sessão. O Wi-Fi do console, a sincronização do relógio e o applet de conexão nunca foram afetados.
- Os dois agora dividem um runtime de sockets com contagem de referência: o primeiro a usar liga, o último desliga. A sincronização do relógio segura o runtime (e a sessão do NIFM) só durante a consulta.
- **Atualizando da 2.11.0 ou 2.12.0:** o atualizador do menu não consegue baixar esta versão, porque é justamente ele que estava quebrado. Abra o **SwitchU Manager** pelo Homebrew Menu e atualize por lá (ele tem o próprio código de rede), ou copie as pastas `atmosphere/` e `switch/` do zip para o cartão SD por MTP ou leitor de cartão.

### Ícone

- A entrada SwitchU Menu na barra lateral e na visualização XMB voltou a usar o ícone da paleta de cores; a 2.11.0 tinha trocado por uma arte de Joy-Con.
