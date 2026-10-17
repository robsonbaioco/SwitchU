# SwitchU 2.12.0

The Settings backlog closes: the console language, the TV audio format and the TV screen size are editable and apply on the spot, a confirmed action cleans up leftover theme folders, and the menu and daemon get leaner — the wallpaper's frames share big GPU blocks instead of one each, the five overlay render targets only exist once an overlay opens, and sounds decode when first played. The three remaining open questions now write their whole answer into the logs.

## English

### Settings

- **Console Language** has a Change button: the console's own language list in the searchable picker, with a confirmation, written with `setsysSetLanguageCode`. It applies at the next restart, and the dialog says so.
- **Speaker Auto-Mute** also applies immediately on firmware 13+ (audctl command 30 — libnx has no wrapper, so it is a one-byte raw call). The saved flag still covers the next boot.
- **TV Audio Output** applies immediately too (`audctlSetAudioOutputMode` on the TV target), besides being saved.
- **TV Screen Size (underscan)** is a live slider now; it had been written and commented out since 2.5.0. Only a TV can show the effect.

### Clean up theme files

- New in Settings > Storage (issue #3): scans `config/SwitchU/themes` when pressed, offers **only** folders holding no readable `theme.json`, shows their names and total size, and deletes after a confirmation, committing the card. Valid themes, the active theme, the built-ins and `theme_presets.ini` cannot be touched by construction.

### Memory (issue #2 plan)

- The menu's heap ladder tops at **384 MB** instead of 416 — the ~32 MB stay in the applet pool for the system.
- The animated wallpaper's frames now live in an **8 MB-chunk arena** instead of one GPU memory block per frame: a 300-frame sequence drops from 300 driver blocks to ~25, and the whole arena is freed at once when the theme changes. (600 individual blocks once crashed the console mid-load; this takes the class of problem away.)
- The five full-resolution offscreen targets (dialog and settings backdrops, the sharp blur pair, the frozen scene) are created **on the first overlay use** instead of at boot: ~18 MB back in any session that never opens one.
- The daemon slims down: main-thread stack 1 MB → 512 KB and the stratosphere heap 2 MB → 1 MB. Its 8 MB main heap waits for the high-water log (`heap=…KB peak=…KB`, written since 2.5.6) before shrinking.
- Sound effects decode on first play instead of all at preset load, and `menu.json` now declares the applet memory pool it actually lives in.

### The three open questions log their own answers

- **Wi-Fi**: the toggle writes a `[settings-wifi]` line with the live NIFM radio state before and after the saved flag, plus the connection status — one look says whether the radio drops immediately or only at the next boot.
- **Auto-sleep**: every change writes a `[settings-sleep]` transaction, and the daemon logs the plans it applies at boot — against the timestamped sleep requests, one test says whether a new delay counts immediately.
- **Old themes**: the installed-themes scan logs its result whenever it changes, orphan folders marked; a theme "deleted" before fork.11 coming back shows up by name, and deleting it again was already logged.
- After testing on a console, **Settings > System > Send logs to the developer** carries all of it.

---

## Português

A fila de Configurações fecha: o idioma do console, o formato de áudio da TV e o tamanho da tela são editáveis e valem na hora, uma ação confirmada limpa pastas de tema órfãs, e o menu e o daemon ficam mais magros — os quadros do papel de parede dividem blocos grandes de GPU em vez de um cada, os cinco alvos de overlay só existem depois que um overlay abre, e os sons decodificam no primeiro uso. As três perguntas abertas que restavam agora escrevem a resposta inteira nos logs.

### Configurações

- **Idioma do console** ganhou um botão Alterar: a lista de idiomas do próprio console no seletor com busca, com confirmação, gravada com `setsysSetLanguageCode`. Vale a partir do próximo reinício, e o diálogo avisa.
- **Auto-mute do alto-falante** também aplica na hora no firmware 13+ (comando 30 do audctl — a libnx não tem wrapper, então é uma chamada bruta de um byte). O flag salvo continua valendo para o próximo boot.
- **Saída de áudio da TV** também aplica na hora (`audctlSetAudioOutputMode` no alvo da TV), além de salvar.
- **Tamanho da tela (underscan)** virou um slider de verdade; estava escrito e comentado desde a 2.5.0. Só uma TV mostra o efeito.

### Limpar arquivos de tema

- Novo em Configurações > Armazenamento (issue #3): varre `config/SwitchU/themes` ao tocar, oferece **só** pastas sem um `theme.json` legível, mostra os nomes e o tamanho total, e apaga depois da confirmação, com commit do cartão. Temas válidos, o tema ativo, os embutidos e o `theme_presets.ini` são intocáveis por construção.

### Memória (plano da issue #2)

- A escada de heap do menu agora para em **384 MB** em vez de 416 — os ~32 MB ficam no pool de applets para o sistema.
- Os quadros do papel de parede animado agora vivem numa **arena de blocos de 8 MB** em vez de um bloco de GPU por quadro: uma sequência de 300 quadros cai de 300 blocos no driver para ~25, e a arena inteira é liberada de uma vez na troca de tema. (600 blocos individuais já derrubaram o console no meio do carregamento; isso elimina a classe do problema.)
- Os cinco alvos offscreen de resolução cheia (fundos de diálogo e configurações, o par de desfoque, a cena congelada) nascem **no primeiro uso de um overlay** em vez de no boot: ~18 MB de volta em qualquer sessão que não abre nenhum.
- O daemon emagrece: stack da thread principal 1 MB → 512 KB e heap do stratosphere 2 MB → 1 MB. O heap principal de 8 MB espera o log de marca-d'água (`heap=…KB peak=…KB`, gravado desde a 2.5.6) antes de encolher.
- Os efeitos sonoros decodificam no primeiro uso em vez de todos na carga do preset, e o `menu.json` passa a declarar o pool de memória de applet em que ele realmente vive.

### As três perguntas abertas escrevem a própria resposta

- **Wi-Fi**: o interruptor grava uma linha `[settings-wifi]` com o estado vivo do rádio (NIFM) antes e depois do flag salvo, mais o status de conexão — uma olhada diz se o rádio cai na hora ou só no próximo boot.
- **Suspensão automática**: cada mudança grava uma transação `[settings-sleep]`, e o daemon registra no boot os planos que aplicou — contra os pedidos de sleep com carimbo de hora, um teste diz se o atraso novo vale na hora.
- **Temas antigos**: a varredura de Instalados registra o resultado sempre que ele muda, com as pastas órfãs marcadas; um tema "apagado" antes da fork.11 que volta aparece pelo nome, e apagá-lo de novo já era registrado.
- Depois de testar no console, **Configurações > Sistema > Enviar logs** carrega tudo isso.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
