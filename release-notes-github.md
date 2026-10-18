# SwitchU 2.13.0

Play a single game with the internet off, without putting the whole console in Airplane Mode. Also: the Vibration setting shows the console's real switch, renaming a game steers its SteamGridDB search, and the Theme Shop stops offering themes that could never download.

## English

### Play offline

- New in the game dossier: **Play offline: On/Off**. When a marked game starts, SwitchU switches the console's internet off; when it closes (or another game starts), the internet comes back.
- Only the internet goes off — the network half of Airplane Mode. Bluetooth and NFC stay on, so wireless controllers keep working. A wired connection through the dock is not affected.
- If Airplane Mode was already on, nothing changes and nothing is turned back on afterwards. If the console loses power mid-game, the internet is restored on the next boot.
- While the game is suspended in the background, the menu is offline too (Theme Shop, artwork, update check).

### Fixes

- **Vibration** in Settings > Controllers is now an on/off switch tied to the console's own "Controller Vibration" setting. It used to show the vibration volume, which stays at 100% when vibration is turned off, so it read "on" while no game vibrated.
- **Rename** now always changes the SteamGridDB search term. Before, it only did when the console reported no name at all, so a game SteamGridDB could not find under its reported name stayed without artwork. Each artwork search in the picker also writes what it found to the log.
- **Theme Shop**: ncarvalho99's server lists about sixty themes but refuses every download from this fork (HTTP 403). The shop now reads his public GitHub mirror instead — 8 themes, all installable — alongside PoloNX's catalogue.

---

## Português

Jogue um único jogo sem internet, sem colocar o console inteiro em modo avião. Também: a opção Vibração mostra o interruptor real do console, renomear um jogo muda a busca do SteamGridDB, e a Loja de Temas para de oferecer temas que nunca baixavam.

### Jogar sem internet

- Novo no dossiê do jogo: **Jogar sem internet: Sim/Não**. Quando um jogo marcado abre, o SwitchU desliga a internet do console; quando ele fecha (ou outro jogo abre), a internet volta.
- Só a internet desliga — a parte de rede do modo avião. Bluetooth e NFC continuam ligados, então os controles sem fio funcionam. Cabo de rede no dock não é afetado.
- Se o modo avião já estava ligado, nada muda e nada é religado depois. Se o console desligar no meio do jogo, a internet volta no próximo boot.
- Enquanto o jogo estiver suspenso em segundo plano, o menu também fica sem internet (Loja de Temas, artes, busca de atualização).

### Correções

- **Vibração** em Configurações > Controles agora é um interruptor liga/desliga ligado à opção "Vibração dos controles" do próprio console. Antes mostrava o volume de vibração, que fica em 100% com a vibração desligada, então aparecia "ligada" enquanto nenhum jogo vibrava.
- **Renomear** agora sempre muda o termo de busca do SteamGridDB. Antes só mudava quando o console não informava nome nenhum, então um jogo que o SteamGridDB não achava pelo nome informado ficava sem arte. Cada busca de arte no seletor também grava no log o que encontrou.
- **Loja de Temas**: o servidor do ncarvalho99 lista uns sessenta temas mas recusa todo download deste fork (HTTP 403). A loja agora lê o espelho público dele no GitHub — 8 temas, todos instaláveis — junto com o catálogo do PoloNX.
