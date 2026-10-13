# SwitchU 2.10.0

The logs can be sent to the developer straight from the console, the top of the single-row view no longer puts profiles on top of the buttons, and the daemon asks for normal performance again when an application closes.

## English

### Logs

- New in Settings > System, below "Save logs for copying": **Send logs to the developer**. It closes the current logs, packs the archived `menu-*.log` and `daemon-*.log` into a zip and sends it over the internet, so a problem can be looked at without taking the card out.
- It asks first. The logs list your games, play times and user nicknames, and nothing leaves the console until you confirm.
- The newest logs go first; when they do not all fit in one message, the oldest are the ones left out.
- The option exists only in the releases of this fork. A build made from the source by somebody else has no key for the endpoint and does not show it.

### Single-row view

- With four accounts and the "add user" tile, the profile bar ran over Settings and the media button landed on Controllers. The profiles and the two buttons beside them are now centred in the space between the two groups of applets.
- From the sixth account on the avatars shrink to fit, down to the eight accounts a console can hold. The grid view is unchanged.

### Performance

- The menu once came back from a long install session drawing three times slower, and stayed that way: what a CPU boost left on by the application looks like. When an application closes, the daemon now asks for the normal mode again and writes a `[perf-config]` line to `daemon.log` with the configuration before and after; another one is written each time the menu comes up. Not yet confirmed on a console -- if the menu still turns slow after a game or a homebrew, that log says why.

---

## Português

Os logs podem ser enviados ao desenvolvedor direto do console, o topo do modo linha não põe mais os perfis em cima dos botões, e o daemon volta a pedir o desempenho normal quando um aplicativo fecha.

### Logs

- Novo em Configurações > Sistema, abaixo de "Salvar logs para cópia": **Enviar logs para o desenvolvedor**. Fecha os logs atuais, junta os `menu-*.log` e `daemon-*.log` arquivados num zip e envia pela internet, para um problema ser investigado sem tirar o cartão.
- Ele pergunta antes. Os logs listam seus jogos, tempos de jogo e apelidos dos usuários, e nada sai do console sem a sua confirmação.
- Os logs mais novos vão primeiro; quando não cabem todos numa mensagem, os mais antigos ficam de fora.
- A opção só existe nas releases deste fork. Uma build feita do código-fonte por outra pessoa não tem a chave do endpoint e não mostra a opção.

### Modo linha

- Com quatro contas e o botão de "adicionar usuário", a barra de perfis passava por cima de Configurações e o botão de mídia caía em cima de Controles. Os perfis e os dois botões ao lado agora ficam centralizados no espaço entre os dois grupos de applets.
- A partir da sexta conta os avatares diminuem para caber, até as oito contas que um console comporta. O modo grade não mudou.

### Desempenho

- O menu voltou uma vez de uma sessão longa de instalação desenhando três vezes mais devagar, e ficou assim: o jeito de um boost de CPU deixado ligado pelo aplicativo. Quando um aplicativo fecha, o daemon agora pede o modo normal de novo e grava uma linha `[perf-config]` no `daemon.log` com a configuração antes e depois; outra é gravada cada vez que o menu sobe. Ainda não confirmado no console -- se o menu continuar lento depois de um jogo ou homebrew, esse log diz o porquê.
