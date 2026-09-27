# SwitchU 2.6.0

ncarvalho99's features from 2.5.0 to 2.6.4 come to this fork, the dossier gathers its details on the console, and games the console cannot name get their names from the internet.

## English

### From ncarvalho99's 2.6.x

- Everything he added from 2.5.0 to 2.6.4: **favorites** and a Favorites sort, the **Quick Settings** panel, the daily **Activity Log**, an **Atmosphere cheat manager** in each game's dossier, **WaraWara Plaza**, a **custom soundtrack** from your own music folder with the **Multimedia Center** and a music shop, page auto-flip while holding the d-pad, page deletion on ZL, folders that move across empty slots, clock sync over the internet at boot, a prompt to fetch artwork when a new game is installed, and names for game updates that store them compressed. Thanks to him for all of it.
- The sort order on **R** now has five modes, with Favorites before Most played. If you had Most played selected, it stays selected.
- What depended on his servers was left out or given another route. His servers answer only his own builds since 2026-09-21, so this fork does not carry his client key. The music shop searches and downloads through public YouTube fallbacks, and a server of your own (`tools/switchu_ytdl_service.py`) can be set as `ytdlBackendUrl` in `config.json`. The new-game artwork prompt appears only with a personal SteamGridDB key, since without one the download would go through his server.

### The dossier's details, gathered on the console

- The dossier's online details came from ncarvalho99's server, which now refuses this fork, so every dossier said they were unavailable. They now come straight from public sources: description, publisher, release date, genres, player count and screenshots from the nlib title API, with no key needed.
- The **metascore** (RAWG) and **time to beat** (IGDB) need keys of your own, free to create, entered in **Settings > SteamGridDB > Game details**. Without them those fields show a dash. There is no source for a user score any more.
- Marking a game as a port no longer refuses every platform: that check also asked his server.
- The dash in empty dossier fields showed as "â€"", and long text could be cut in the middle of an accented letter. Both fixed.

### Names for games the console cannot name

- A game with no usable name of its own -- a downgraded release, for example -- showed its title id on the grid and could not be found on SteamGridDB. Such games are now named from a built-in table of well-known titles or, once per game, from the nlib title API. The answer is kept on the card, and a name you give the game yourself still wins.

---

## Português

As funcionalidades do ncarvalho99 da 2.5.0 à 2.6.4 chegam a este fork, o dossiê busca os detalhes direto do console, e jogos que o console não sabe nomear ganham nome pela internet.

### Da 2.6.x do ncarvalho99

- Tudo o que ele acrescentou da 2.5.0 à 2.6.4: **favoritos** e a ordenação por favoritos, o painel de **Quick Settings**, o **registro diário de atividade**, um **gerenciador de cheats** do Atmosphere no dossiê de cada jogo, a **WaraWara Plaza**, **trilha sonora própria** a partir da sua pasta de músicas com a **Central Multimídia** e uma loja de músicas, virar página segurando o direcional, apagar página com ZL, pastas que passam por espaços vazios, relógio acertado pela internet no boot, um aviso para baixar a arte quando um jogo novo é instalado, e nomes de updates de jogos que os guardam comprimidos. Obrigado a ele por tudo isso.
- A ordenação no **R** agora tem cinco modos, com Favoritos antes de Mais jogados. Se você estava em Mais jogados, continua em Mais jogados.
- O que dependia dos servidores dele ficou de fora ou ganhou outro caminho. Os servidores dele só atendem as builds dele desde 21/09/2026, então este fork não leva a chave de cliente dele. A loja de músicas busca e baixa pelos caminhos públicos do YouTube, e um servidor seu (`tools/switchu_ytdl_service.py`) pode ser configurado como `ytdlBackendUrl` no `config.json`. O aviso de arte para jogo novo só aparece com uma key pessoal do SteamGridDB, porque sem ela o download passaria pelo servidor dele.

### Os detalhes do dossiê, buscados no console

- Os detalhes online do dossiê vinham do servidor do ncarvalho99, que agora recusa este fork, então todo dossiê dizia que eles não estavam disponíveis. Agora vêm direto de fontes públicas: descrição, publisher, data de lançamento, gêneros, número de jogadores e screenshots pela API de títulos da nlib, sem precisar de chave.
- O **metascore** (RAWG) e o **tempo para zerar** (IGDB) precisam de chaves suas, gratuitas, cadastradas em **Configurações > SteamGridDB > Detalhes dos jogos**. Sem elas, esses campos mostram um travessão. Não existe mais fonte para a nota dos usuários.
- Marcar um jogo como port não recusa mais todas as plataformas: essa verificação também consultava o servidor dele.
- O travessão dos campos vazios do dossiê aparecia como "â€"", e textos longos podiam ser cortados no meio de uma letra acentuada. Os dois foram corrigidos.

### Nomes para jogos que o console não sabe nomear

- Um jogo sem nome próprio -- uma versão com downgrade, por exemplo -- mostrava o title id na grade e não era encontrado no SteamGridDB. Agora esses jogos recebem nome de uma tabela de títulos conhecidos ou, uma vez por jogo, da API de títulos da nlib. A resposta fica guardada no cartão, e um nome que você mesmo der ao jogo continua valendo por cima.
