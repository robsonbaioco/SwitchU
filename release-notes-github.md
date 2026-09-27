# SwitchU 2.7.0

What PoloNX's own repository gained since his 1.2.0 comes to this fork: coming back from a game is immediate, a suspended game resumes with a short fade, and folders get styles and a cover.

## English

### Back to HOME at once

- When you leave for a game, the menu saves the frame on screen with its page, folder and focus. When HOME brings it back, that frame is shown straight away while the menu rebuilds behind it, and the live menu fades in on the same page, folder and focused game. From PoloNX/SwitchU#104 by bshurikan.
- Resuming a suspended game plays a short fade to black instead of the full launch animation, and the outline of a suspended game pulses harder so it is easier to spot. From #105, also by bshurikan.
- The saved frame is half resolution, 0.9 MB, and is left out of the configuration backup.

### Folders

- Six new folder styles next to Classic -- Simple, Minimal, Tab, Ring, Manila and Label -- and a **Show cover** option that puts the folder's first game on the tile. Both are in the folder options and apply to every folder. Classic is still this fork's glass folder, so nothing changes until you pick another style. From #100 by bshurikan.
- Moving a game into a folder from page 2 onwards no longer freezes the moving icon in the corner (#100).
- Inside a folder, dropping a game on another swaps the two instead of leaving a blank tile and shifting the rest (#102).
- A tap outside the icons closes an open folder, as B does (#108).

### Memory

- The hero and logo of wide game cards are kept only for the page on screen, and released when you leave it. They used to stay loaded for the whole grid, so their cost grew with every wide card in the library. From PoloNX's #88.

### What was not brought in

- The rest of PoloNX's #88 and #98 change how his daemon's main loop waits; this fork's daemon works differently and does not have the delay #98 fixes. His #110 was already here through ncarvalho99's version.
- PR #106 (animations, by PtitLegume) is still open and partly in progress. It will be looked at again at the next release.

---

## Português

O que o repositório do próprio PoloNX ganhou depois da 1.2.0 chega a este fork: voltar de um jogo é imediato, um jogo suspenso volta com um fade curto, e as pastas ganham estilos e capa.

### De volta ao HOME na hora

- Quando você sai para um jogo, o menu guarda a tela como estava, com a página, a pasta e o foco. Quando o HOME o traz de volta, essa tela aparece na hora enquanto o menu se reconstrói por trás, e o menu de verdade aparece com um fade na mesma página, pasta e jogo focado. Do PoloNX/SwitchU#104, de bshurikan.
- Retomar um jogo suspenso faz um fade curto para preto em vez da animação completa de abertura, e o contorno do jogo suspenso pulsa mais forte, para ficar fácil de ver. Do #105, também de bshurikan.
- A tela guardada tem metade da resolução, 0,9 MB, e fica fora do backup da configuração.

### Pastas

- Seis estilos novos de pasta ao lado do Clássico -- Simples, Mínimo, Aba, Anel, Manila e Etiqueta -- e a opção **Mostrar capa**, que coloca o primeiro jogo da pasta no ícone. Os dois ficam nas opções da pasta e valem para todas. O Clássico continua sendo a pasta de vidro deste fork, então nada muda até você escolher outro estilo. Do #100, de bshurikan.
- Mover um jogo para uma pasta a partir da página 2 não trava mais o ícone em movimento no canto (#100).
- Dentro de uma pasta, soltar um jogo sobre outro troca os dois de lugar em vez de deixar um espaço vazio e empurrar o resto (#102).
- Tocar fora dos ícones fecha a pasta aberta, como o B (#108).

### Memória

- A arte e o logo dos cartões largos de jogo ficam carregados só para a página na tela, e são liberados quando você sai dela. Antes ficavam carregados para a grade inteira, e o custo crescia com cada cartão largo da biblioteca. Do #88 do PoloNX.

### O que não entrou

- O resto do #88 e o #98 do PoloNX mudam o jeito como o laço principal do daemon dele espera; o daemon deste fork funciona de outro jeito e não tem o atraso que o #98 corrige. O #110 dele já estava aqui pela versão do ncarvalho99.
- O PR #106 (animações, de PtitLegume) continua aberto e com partes em andamento. Ele será olhado de novo na próxima release.
