# SwitchU 2.14.0

Play offline no longer keeps the menu offline while its game is suspended, Send logs now carries Atmosphère's crash reports, and two rendering fixes from upstream.

## English

### Play offline only while you play

- A game set to play offline had the wireless turned off at launch and back on only when it **closed**. Left suspended under the menu, it kept the whole console offline: the update check, the Theme Shop and Send logs all failed for as long as it stayed suspended. That is why a console with such a game suspended for days never saw 2.13.3.
- Now **HOME gives the wireless back** while the game waits under the menu, and **resuming the game turns it off again** before the game is on screen.
- If you switch Airplane Mode on yourself from the menu in between, that choice is yours: the wireless stays off and closing the game will not turn it back on.
- On 2.13.0 to 2.13.3, close a game set to play offline before updating.

### Send logs includes crash reports

- **Settings > System > Send logs** now also sends the newest Atmosphère reports: up to 6 from `atmosphere/crash_reports` and 4 from `atmosphere/fatal_reports`. They name the program that crashed and where, which a log cut off mid-frame cannot. They have their own share of the archive, so the logs still fit.
- The README named the fatal reports folder wrong (`fatal_errors`); it is `fatal_reports`.

### Rendering

- **Text cache:** each new piece of text was stored twice, and once the cache was full it grew past its limit instead of reusing a slot. Each text now takes one slot and its memory is counted correctly. From ncarvalho99's OmniLaunch.
- One redundant GPU queue flush per frame is gone.
- **Waking from sleep:** the menu rebuilds its swapchain on wake, which PoloNX found going stale across sleep and freezing the menu on its last frame. The fork had not shown it yet; this keeps it that way. A `[power] console awake: swapchain rebuilt` line goes to the log.

---

## Português

Jogar sem internet não deixa mais o menu sem internet enquanto o jogo está suspenso, o envio de logs passa a levar os relatórios de crash do Atmosphère, e duas correções de renderização vindas do upstream.

### Sem internet só enquanto você joga

- Um jogo marcado para jogar sem internet desligava o Wi-Fi ao abrir e só religava quando ele **fechava**. Suspenso debaixo do menu, deixava o console inteiro sem internet: a busca de atualização, a Loja de Temas e o envio de logs falhavam enquanto ele ficasse suspenso. Foi por isso que um console com um jogo assim suspenso por dias nunca viu a 2.13.3.
- Agora o **HOME devolve o Wi-Fi** enquanto o jogo espera debaixo do menu, e **voltar ao jogo desliga de novo** antes de o jogo aparecer na tela.
- Se você ligar o Modo Avião no menu nesse meio-tempo, a escolha é sua: o Wi-Fi fica desligado e fechar o jogo não religa.
- Na 2.13.0 a 2.13.3, feche o jogo marcado para jogar sem internet antes de atualizar.

### O envio de logs leva os relatórios de crash

- **Configurações > Sistema > Enviar logs** agora também manda os relatórios mais recentes do Atmosphère: até 6 de `atmosphere/crash_reports` e 4 de `atmosphere/fatal_reports`. Eles dizem qual programa travou e onde, coisa que um log cortado no meio de um quadro não diz. Eles têm uma parte própria do arquivo, então os logs continuam cabendo.
- O README dava o nome errado da pasta de relatórios fatais (`fatal_errors`); é `fatal_reports`.

### Renderização

- **Cache de textos:** cada texto novo era guardado duas vezes, e com o cache cheio ele passava do limite em vez de reaproveitar uma vaga. Agora cada texto ocupa uma vaga e a memória dele é contada certo. Vem do OmniLaunch do ncarvalho99.
- Saiu um envio repetido da fila da GPU a cada quadro.
- **Ao acordar da suspensão:** o menu recria a swapchain ao acordar, que o PoloNX viu ficar inválida durante a suspensão e congelar o menu no último quadro. O fork ainda não tinha mostrado isso; assim continua. Uma linha `[power] console awake: swapchain rebuilt` vai para o log.
