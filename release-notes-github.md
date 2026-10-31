# SwitchU 2.15.0

A large port from PoloNX's SwitchU (up to 727435a, 2026-10-30): game posters in the single-row view, icon packs with an online shop, a reorganized Theme Shop, and a Battery Saver mode with a round of memory savings. Everything the fork already had stays.

## English

### Posters in the single-row view

- **Dynamic Line** can show SteamGridDB's tall 2:3 posters with the game's logo, instead of square icons. Turn it on or off, and choose where SteamGridDB art appears (grid, single row, folders), in **SwitchU Menu > SteamGridDB**.
- A poster is the same picture the fork already used as the **Cover** (same file in `config/SwitchU/covers`), so nothing is downloaded twice.
- **Folders** show a preview of the games inside, or a custom icon or poster of their own (Folder Options). Each folder can open as Grid, Single row or Auto.
- **ZL/ZR** jump between sections in the single row. Switching between grid and single row is animated.

### Tiles

- Games can be **1x1, 2x1, 4x2, 2x2 or 2x3**. 2x1 and 4x2 use SteamGridDB's wide art, 2x3 uses the poster.
- Moving a large tile onto others now trades places or packs them into the freed space, instead of refusing.
- The launch zoom matches the tile's shape, and the blue selection fits posters and wide tiles.
- Game Options can remove each kind of art (hero, logo, icon, poster, wide).

### Icon packs

- **SwitchU Menu > Icons** switches the icons of the side bar and HOME buttons: Default, the new Atmos pack, packs that come with a theme, and packs from the online shop (saved in `switch/SwitchU/icons`).
- Colour packs can be tinted (hue, saturation, lightness) per pack; icons can adapt their contrast to the theme.

### Theme Shop

- **Themes** and **Icons** each have an Installed shelf and a Shop shelf; options are split into **Sounds** and **Interface**.
- The fork's Animated Themes, Music, SteamGridDB, Update and Uninstall tabs and its theme catalogues are all still there.
- The catalogue is refreshed every time the shop opens (the SD copy is used offline).
- A theme song that is really AAC renamed to `.mp3` is skipped instead of freezing the menu.

### Battery Saver and memory

- **Battery Saver** (Quick Settings or SwitchU Menu > Interface) runs HOME with fewer artworks loaded and less GPU memory.
- Always on: the screen shown when you come back from a game is kept at half resolution and released after the fade; folder art and SteamGridDB caches are capped; the single-row background loads at 640x360.
- The CPU boost some games leave on (FastLoad) is switched off when the game closes, so HOME no longer feels slow after them.
- Frame Tap: when a Frame Tap viewer is connected, HOME switches to the light path by itself.

### Optional MTP sysmodule

- PoloNX's **SwitchU-MTP** (USB file transfer, based on Atmosphère's haze) is built as a separate optional package. It is **not** in the release zips and not part of the menu.

### Not taken

- PoloNX's limit on relaunching the menu after graphics failures (it would stop the fork's safe mode from engaging), the profile carousel, the Quick Settings music player, and the cursor-motion and page-transition options.

None of this has run on a console yet. If a view misbehaves, Minus goes back to the Grid; Settings > System > Send logs sends what is needed.

---

## Português

Um porte grande do SwitchU do PoloNX (até o 727435a, 30/10/2026): pôsteres dos jogos na visualização em linha, pacotes de ícones com loja online, uma Loja de Temas reorganizada e um modo Economia de Bateria com várias economias de memória. Tudo o que o fork já tinha continua.

### Pôsteres na visualização em linha

- A **Linha Dinâmica** pode mostrar os pôsteres verticais 2:3 do SteamGridDB com o logo do jogo, no lugar dos ícones quadrados. Ligue ou desligue, e escolha onde a arte do SteamGridDB aparece (grade, linha, pastas), em **Menu SwitchU > SteamGridDB**.
- O pôster é a mesma imagem que o fork já usava como **Capa** (mesmo arquivo em `config/SwitchU/covers`), então nada é baixado duas vezes.
- As **pastas** mostram uma prévia dos jogos de dentro, ou um ícone ou pôster próprio (Opções da pasta). Cada pasta pode abrir em Grade, Linha ou Automático.
- **ZL/ZR** pulam entre seções na linha. A troca entre grade e linha é animada.

### Blocos

- Os jogos podem ter **1x1, 2x1, 4x2, 2x2 ou 2x3**. 2x1 e 4x2 usam a arte larga do SteamGridDB, 2x3 usa o pôster.
- Mover um bloco grande para cima de outros agora troca de lugar ou acomoda os outros no espaço liberado, em vez de recusar.
- O zoom ao abrir segue o formato do bloco, e a seleção azul se ajusta a pôsteres e blocos largos.
- As Opções do jogo podem remover cada tipo de arte (fundo, logo, ícone, pôster, larga).

### Pacotes de ícones

- **Menu SwitchU > Ícones** troca os ícones da barra lateral e dos botões do HOME: Padrão, o novo pacote Atmos, pacotes que vêm com um tema e pacotes da loja online (salvos em `switch/SwitchU/icons`).
- Pacotes coloridos podem ser tingidos (matiz, saturação, luminosidade) por pacote; os ícones podem ajustar o contraste ao tema.

### Loja de Temas

- **Temas** e **Ícones** têm cada um uma prateleira de Instalados e uma de Loja; as opções foram divididas em **Sons** e **Interface**.
- As abas do fork (Temas Animados, Música, SteamGridDB, Atualizar e Desinstalar) e os catálogos de temas continuam todos lá.
- O catálogo é atualizado toda vez que a loja abre (sem internet, usa a cópia do SD).
- Uma música de tema que na verdade é AAC renomeado para `.mp3` é pulada em vez de travar o menu.

### Economia de Bateria e memória

- **Economia de Bateria** (Configurações rápidas ou Menu SwitchU > Interface) roda o HOME com menos artes carregadas e menos memória de GPU.
- Sempre ativo: a tela mostrada na volta de um jogo fica em meia resolução e é liberada depois do fade; as artes de pasta e os caches do SteamGridDB têm limite; o fundo da linha carrega em 640x360.
- O boost de CPU que alguns jogos deixam ligado (FastLoad) é desligado quando o jogo fecha, e o HOME não fica mais lento depois deles.
- Frame Tap: com um visualizador do Frame Tap conectado, o HOME passa sozinho para o modo leve.

### Sysmodule MTP opcional

- O **SwitchU-MTP** do PoloNX (transferência de arquivos por USB, baseado no haze do Atmosphère) é compilado como pacote opcional à parte. Ele **não** está nos zips da release nem faz parte do menu.

### O que ficou de fora

- O limite do PoloNX para reabrir o menu depois de falhas gráficas (impediria o modo seguro do fork de entrar), o carrossel de perfis, o player de música das Configurações rápidas e as opções de movimento do cursor e transição de página.

Nada disso rodou em console ainda. Se uma visualização se comportar mal, o Minus volta para a Grade; Configurações > Sistema > Enviar logs manda o que for preciso.
