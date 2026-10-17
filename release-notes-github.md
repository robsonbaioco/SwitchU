# SwitchU 2.11.0

Nine home views instead of two — Flow, Shelf and Deck in 3D, a fullscreen Cover pager, an XMB bar, a Niagara-style list and Metro live tiles join the grid and the dynamic line. The menu now fills the whole TV when docked, protects itself against a theme that crashes it at start-up, verifies and can roll back its own updates, and picks up a pile of fixes from the open PoloNX pull requests.

## English

### Nine launcher views

- **Minus** cycles the view on the HOME screen; the full selector lives in SwitchU Menu > Options > **Launcher View**. Grid stays first in the cycle, so it is always one press away if a view misbehaves.
- **Flow (3D)**: a coverflow of game cases, with box art on the front when a vertical cover exists and the game icon otherwise.
- **Shelf (3D)**: an Xbox 360-style library row with reflections.
- **Deck (3D)**: a card ribbon with the hero artwork behind it.
- **Cover**: one fullscreen cover at a time, paged with a slide.
- **XMB**: a PSP/PS3 cross-media bar — applets in the columns, games in their own column.
- **List (Niagara)**: a vertical list with a details card — publisher, version, storage, play stats and a synopsis (fetched from the public nlib API, no key needed).
- **Metro (Live Tiles)**: a tile wall in the sLaunch/Windows Phone manner. **Y** cycles a tile between 1x1, 2x1 and 2x2; the sizes are remembered per game.
- The 3D views and Cover use vertical covers (600x900) from SteamGridDB when your API key is set: the scraper now also fetches box covers, and the game's artwork menu gains a **Cover** slot.
- Folders render their icon grid in every view; opening one always lands on its first title.
- All of this comes from ncarvalho99's OmniLaunch (GPL-3), adapted back to SwitchU: same views, without his rename, update feed, client-key services or video wallpapers. Nothing here has been tested on a console yet.

### Fills the TV when docked

- Docked, the menu drew a 720p image in the middle of the 1080p output with black borders all around. The display layer is now sized to 1920x1080 and the image scaled to it, like the stock HOME menu (PoloNX #154). `menu.log` writes a `[menu] layer ... size 1920x1080 rc=0x0` line.

### Safe mode

- A menu that dies repeatedly while starting (a broken theme, a bad asset) used to leave the console looping. After three start-up crashes the menu now comes up in **safe mode**: built-in theme, built-in sounds, no music, nothing third-party parsed — in memory only, your config.json is not rewritten. A dialog explains it and offers to leave safe mode (restart).
- If safe mode itself keeps dying, the daemon renames the qlaunch override away (reversible) and reboots into the stock HOME menu rather than leaving a dead console.
- The guard state survives power loss; powering off normally within seconds of a menu start does not count as a crash.

### Updates verify themselves and can roll back

- The in-app updater refuses a release whose ZIP carries no SHA-256 digest on GitHub, and verifies the downloaded archive against the digest before anything is staged.
- Applying an update now keeps the files it replaces in `config/SwitchU/update/rollback` behind a journal. A power cut mid-apply is repaired or rolled back at the next boot, and if the updated menu crash-loops, the previous version is restored before any stock fallback. The backup is dropped the first time the new menu proves healthy.

### Settings

- **Timezone** in Settings > System is now a Change button: the console's own timezone list in a searchable picker, written back to the system.
- es-419, pt-PT, fr-CA and en-GB consoles no longer fall back to English: each reads the closest language that ships. Spanish is complete (every key en-US has), and the Manager gains a Spanish translation.
- SteamGridDB searches that die in TLS now say that a wrong console clock breaks HTTPS and point at Settings > System, instead of showing a blank error.

### Fixes from the open PoloNX pull requests

- Plus+Minus (voice guidance) no longer demands both buttons on the same frame (#139).
- Right on a selector or action row in Options no longer acts like A (#157).
- The battery widget no longer shows 0 % with no charge icon right after boot (#149).
- The tutorial body text survives the font cache filling up (#143).
- Bluetooth connect/disconnect/unpair run off the UI thread — an out-of-range headset no longer freezes the menu (#138).
- A second HOME press with nothing left to dismiss resumes the suspended game, like the stock menu (#150).
- The analog stick steps between icons more calmly than the D-pad; circling it no longer skips across the grid (#158).
- A-Z, Recent and the other sorted views place larger tiles first-fit instead of shrinking them to 1x1 (#151).
- Moving a title into a folder no longer insert-shifts everything after it; music no longer skips when something calls play twice (#133).
- HOME over a forwarder closes it instead of freezing the console on its last frame; retail games still suspend (#161).
- After a fast return the visible page's icons upload in a burst instead of one per frame (#152).

### Under the hood

- The daemon only rewrites its app catalog when the content actually changed (fingerprint), the widget-store save and the activity-log prefetch left the start-up path, and the SteamGridDB backdrop re-resolves only when the focused title changed.
- Every start that is not the cold boot now takes the fast-return path, suspended title or not.

---

## Português

Nove modos de exibição em vez de dois — Flow, Shelf e Deck em 3D, capas em tela cheia, uma barra XMB, uma lista estilo Niagara e os blocos dinâmicos do Metro se juntam à grade e à linha dinâmica. O menu agora preenche a TV inteira na dock, se protege de um tema que derruba a inicialização, verifica e consegue desfazer as próprias atualizações, e traz uma pilha de correções dos pull requests abertos do PoloNX.

### Nove modos de exibição

- **Minus** troca o modo na tela HOME; o seletor completo fica em Menu SwitchU > Opções > **Modo de exibição**. A Grade é sempre a primeira do ciclo, a um toque de distância se algum modo se comportar mal.
- **Flow (3D)**: um coverflow de caixinhas de jogo, com a capa na frente quando existe capa vertical e o ícone do jogo quando não.
- **Shelf (3D)**: a estante estilo Xbox 360, com reflexos.
- **Deck (3D)**: uma fita de cartões com a arte de fundo do jogo atrás.
- **Capa**: uma capa em tela cheia por vez, com paginação deslizante.
- **XMB**: a barra do PSP/PS3 — applets nas colunas, jogos numa coluna própria.
- **Lista (Niagara)**: lista vertical com um cartão de detalhes — publicadora, versão, armazenamento, estatísticas de jogo e uma sinopse (da API pública nlib, sem chave).
- **Metro (Blocos Dinâmicos)**: uma parede de blocos ao estilo sLaunch/Windows Phone. **Y** alterna o bloco entre 1x1, 2x1 e 2x2; os tamanhos ficam gravados por jogo.
- Os modos 3D e a Capa usam capas verticais (600x900) do SteamGridDB quando a sua chave de API está configurada: o scraper agora também baixa capas, e o menu de arte do jogo ganha um espaço **Capa**.
- As pastas desenham sua grade de ícones em todos os modos; abrir uma sempre foca o primeiro título.
- Tudo isso vem do OmniLaunch do ncarvalho99 (GPL-3), adaptado de volta ao SwitchU: os mesmos modos, sem a troca de nome, o feed de atualização, os serviços com chave dele ou os papéis de parede em vídeo. Nada disso foi testado num console ainda.

### Preenche a TV na dock

- Na dock, o menu desenhava uma imagem 720p no meio da saída 1080p, com bordas pretas em volta. A camada de vídeo agora é dimensionada para 1920x1080 e a imagem é escalada, como no menu HOME original (PoloNX #154). O `menu.log` registra uma linha `[menu] layer ... size 1920x1080 rc=0x0`.

### Modo seguro

- Um menu que morre repetidamente ao iniciar (um tema quebrado, um recurso ruim) deixava o console em loop. Depois de três quedas na inicialização, o menu agora sobe em **modo seguro**: tema padrão, sons padrão, sem música, nada de terceiros é lido — só em memória, o seu config.json não é reescrito. Um aviso explica e oferece sair do modo seguro (reinicia).
- Se o próprio modo seguro continuar morrendo, o daemon renomeia o override do qlaunch (reversível) e reinicia no menu HOME original, em vez de deixar um console morto.
- O estado da proteção sobrevive a quedas de energia; desligar normalmente logo depois de o menu abrir não conta como queda.

### Atualizações verificadas e com rollback

- O atualizador do menu recusa uma release cujo ZIP não tem o SHA-256 publicado no GitHub, e confere o arquivo baixado contra ele antes de preparar qualquer coisa.
- Aplicar uma atualização agora guarda os arquivos substituídos em `config/SwitchU/update/rollback`, com um diário. Uma queda de energia no meio é reparada ou desfeita no boot seguinte, e se o menu atualizado entrar em loop de queda, a versão anterior volta antes de qualquer retorno ao menu original. A cópia é descartada na primeira vez que o menu novo se prova saudável.

### Configurações

- O **Fuso horário** em Configurações > Sistema agora tem um botão Alterar: a lista de fusos do próprio console num seletor com busca, gravada de volta no sistema.
- Consoles em es-419, pt-PT, fr-CA e en-GB não caem mais para o inglês: cada um lê o idioma mais próximo que existe. O espanhol está completo (todas as chaves do en-US), e o Manager ganhou tradução para espanhol.
- Buscas do SteamGridDB que morrem no TLS agora dizem que um relógio errado quebra o HTTPS e apontam para Configurações > Sistema, em vez de um erro em branco.

### Correções dos pull requests abertos do PoloNX

- Plus+Minus (leitura de voz) não exige mais os dois botões no mesmo frame (#139).
- Direita numa lista ou ação das Opções não age mais como o A (#157).
- A bateria não aparece mais com 0 % e sem ícone de carga logo depois do boot (#149).
- O texto do tutorial sobrevive ao cache de fontes encher (#143).
- Conectar/desconectar/desparear Bluetooth roda fora da thread da interface — um fone fora de alcance não congela mais o menu (#138).
- Um segundo HOME sem nada para fechar volta ao jogo suspenso, como no menu original (#150).
- O analógico anda entre os ícones com mais calma que o D-pad; girar ele não pula mais pela grade (#158).
- A-Z, Recentes e os outros modos ordenados encaixam os blocos maiores inteiros em vez de encolher para 1x1 (#151).
- Mover um título para dentro de uma pasta não empurra mais tudo que vem depois; a música não dá mais um pulo quando algo chama play duas vezes (#133).
- HOME por cima de um forwarder fecha o forwarder em vez de congelar o console no último frame; jogos de verdade continuam suspendendo (#161).
- Depois de uma volta rápida, os ícones da página visível sobem em rajada em vez de um por frame (#152).

### Por baixo do capô

- O daemon só reescreve o catálogo de apps quando o conteúdo mudou de fato (impressão digital), a gravação dos widgets e a pré-carga do registro de atividades saíram do caminho de inicialização, e o fundo do SteamGridDB só se recalcula quando o título focado mudou.
- Toda inicialização que não é o boot frio agora usa o caminho de volta rápida, com ou sem título suspenso.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
