# SwitchU 2.8.0

Animations throughout the menu and a theme that follows the time of day, from PoloNX's repository, plus names for Switch 2 Edition titles and a fresh icon after a reinstall.

## English

### Animations

- While a game is being moved, every other tile wiggles, and the moving icon glides to its new slot instead of jumping there.
- Folders zoom out of their tile when they open and fly back into it when they close; the folder name rises out of the panel.
- Page changes are animated in the Wii U style, and so is the switch into the single-row view. On a grid with a single page, trying to go further gives a small bump.
- From PoloNX/SwitchU#106 by PtitLegume. The folder name is still a button while a folder is open: A renames it and UP from the top row reaches it.

### Automatic theme

- A new **Automatic Theme** entry in the Theme Shop options switches between a day theme and a night theme, either at fixed hours or at sunrise and sunset where you are. For the latter the console asks ip-api.com once for an approximate location from its public IP; nothing is sent unless you choose that mode. The theme is checked again whenever the console wakes. Also from #106.

### Names and icons

- A Switch 2 Edition can keep its name outside the ordinary control data, so it showed its title id on the grid. The daemon now looks in the extra data slots when nothing else names a title (firmware 19.0.0 and later). From #111 by tomvita.
- Uninstalling a game from SwitchU drops its cached name and icon at once, so reinstalling a version with a different icon shows the new one. From #118 by bshurikan.

---

## Português

Animações pelo menu inteiro e um tema que acompanha a hora do dia, do repositório do PoloNX, além de nomes para títulos Switch 2 Edition e ícone novo depois de reinstalar.

### Animações

- Enquanto um jogo é movido, os outros ícones tremem, e o ícone movido desliza até o lugar novo em vez de pular.
- As pastas saem do próprio ícone ao abrir e voltam para ele ao fechar; o nome da pasta sobe de dentro do painel.
- A troca de página é animada no estilo do Wii U, e a passagem para a linha única também. Numa grade de uma página só, tentar ir além dá um pequeno tranco.
- Do PoloNX/SwitchU#106, de PtitLegume. O nome da pasta continua sendo um botão com a pasta aberta: o A renomeia e o ↑ a partir da primeira fileira chega nele.

### Tema automático

- Uma entrada nova, **Tema automático**, nas opções da loja de temas, alterna entre um tema de dia e um de noite, em horários fixos ou no nascer e pôr do sol onde você está. Para isso o console pergunta uma vez ao ip-api.com uma localização aproximada pelo IP público; nada é enviado se você não escolher esse modo. O tema é conferido de novo sempre que o console acorda. Também do #106.

### Nomes e ícones

- Um Switch 2 Edition pode guardar o nome fora dos dados comuns do jogo, e por isso aparecia com o title id na grade. O daemon agora procura nos espaços extras quando nada mais dá nome ao jogo (firmware 19.0.0 em diante). Do #111, de tomvita.
- Desinstalar um jogo pelo SwitchU apaga na hora o nome e o ícone guardados, então reinstalar uma versão com outro ícone mostra o novo. Do #118, de bshurikan.
