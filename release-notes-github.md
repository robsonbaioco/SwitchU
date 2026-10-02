# SwitchU 2.9.0

The menu half of ncarvalho99's 2.6.5: icons can go to any empty slot, the SteamGridDB options get a tab of their own with an opacity setting, and the sidebar answers to a tap. The daemon also starts logging power requests.

## English

### Grid

- While rearranging, an icon can be dropped on any empty slot of any page; it no longer has to follow the last one.
- Folder entries with no title behind them are removed, and the cursor no longer lands on the second half of a wide widget when moving up or down.

### SteamGridDB

- The SteamGridDB options moved out of System Settings into a **SteamGridDB** tab in the SwitchU menu, after Music: show artwork, the API key and the scan for missing artwork.
- New there: **artwork opacity**, from 0 to 100%, for the hero and logo behind the menu. It starts at 50%, which is a little stronger than before in the grid and a little lighter in the single-row view.
- The **RAWG** and **IGDB** keys for the dossier's metascore and time to beat moved to the same tab, under Game details.

### Touch

- A tap on a sidebar button or on the power icon now opens it.

### Power

- The daemon writes a `[power] request` line to `daemon.log` before it restarts or shuts the console down, and if the request is refused it goes back to work instead of staying frozen. The restart path itself is unchanged.

### Not taken from 2.6.5

- His change to how the console restarts, and the update bridge to his OmniLauncher repository. This fork keeps updating from its own releases.

---

## Português

A parte do menu da 2.6.5 do ncarvalho99: os ícones podem ir para qualquer espaço vazio, as opções do SteamGridDB ganham uma aba própria com ajuste de opacidade, e a barra lateral responde ao toque. O daemon também passa a registrar os pedidos de energia.

### Grade

- Ao reorganizar, um ícone pode ser solto em qualquer espaço vazio de qualquer página; não precisa mais vir logo depois do último.
- Entradas de pasta sem título por trás são removidas, e o cursor não para mais na segunda metade de um widget largo ao subir ou descer.

### SteamGridDB

- As opções do SteamGridDB saíram das Configurações do Sistema e foram para uma aba **SteamGridDB** no menu SwitchU, depois de Música: exibir as artes, a chave de API e a busca de artes que faltam.
- Novo ali: **opacidade das artes**, de 0 a 100%, para a arte de fundo e o logo atrás do menu. Começa em 50%, um pouco mais forte que antes na grade e um pouco mais leve na linha única.
- As chaves **RAWG** e **IGDB**, da nota e do tempo de jogo no dossiê, foram para a mesma aba, em Detalhes do jogo.

### Toque

- Tocar num botão da barra lateral ou no ícone de energia agora abre o que ele faz.

### Energia

- O daemon grava uma linha `[power] request` no `daemon.log` antes de reiniciar ou desligar o console, e se o pedido for recusado ele volta a funcionar em vez de ficar travado. O caminho de reinício em si não mudou.

### O que não veio da 2.6.5

- A mudança dele na forma de reiniciar o console e a ponte de atualização para o repositório OmniLauncher dele. Este fork continua atualizando pelas próprias releases.
