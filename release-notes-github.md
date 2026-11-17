# SwitchU 2.16.0

A port from PoloNX's SwitchU (up to c4416d4, 2026-11-15) and a few of ncarvalho99's OmniLaunch fixes, plus a closer look at sleep: the console seemed to only turn the screen off instead of really sleeping.

## English

### Sleep

- **The menu now gets out of the way before the console sleeps.** It pauses its music and stops drawing, and the system waits a moment for it before going to sleep. A menu that kept playing and presenting could keep the console from settling into sleep, with the screen dark and the battery draining.
- **Sleeping right after waking** (within the ten seconds the lock screen stays up) used to leave the menu drawing into the sleep. It now goes straight to black.
- **The logs now say whether the console really slept.** On wake they record how long it was away, how much of that time the system stayed awake, and the battery drop per hour, with a verdict. If sleep still looks wrong to you, sleep for ten minutes or more, wake the console and send the logs from Settings > System.

### Controllers

- **A single Joy-Con held sideways works in every menu**, its four buttons as A/B/X/Y, SL/SR as L/R and the stick turned to match.
- **GameCube and other external controllers** can drive HOME.

### Display and performance

- **Docking and undocking with HOME open** resizes the picture at once: 1920x1080 on the TV, 1280x720 handheld (handheld used the TV size before). Auto-sleep follows the right plan (TV or handheld) after the switch.
- **HOME draws at 30 frames per second after two seconds without input**, and back to 60 on the next press. This was meant to be there already but never kicked in.
- The SoC temperature is read on firmware 14 and later; the performance line in the log shows the GPU clock and temperature.
- The docked performance mode is kept when a game closes.

### Battery

- The **Batteries** tile and Quick Settings show the same charge, and the controller rings use your Joy-Cons' real colours (also when docked).

### Music

- **Music picks up where it stopped** when you come back to HOME from a game. Turn it off in Theme Shop > Sounds. The jump to the saved position happens off screen, so HOME stays responsive.

### Lock screen

- **Transitions:** None, Curtain, Dissolve (default, as before), Zoom or Wipe, with a soft wave over the clock and a breathing vignette. Settings > Sleep > Lock screen. Going to sleep always locks instantly.

### Themes and art

- **Manual themes:** a theme copied by hand to `switch/SwitchU/themes` shows up under Installed next to the Theme Shop ones (the shop copy wins if both have the same id). Automatic cleanup never touches that folder.
- **SteamGridDB backgrounds:** choose their quality (Off, Low, Medium, High) and blur in SwitchU Menu > SteamGridDB.
- **2x3 tiles** get their own high-resolution poster.

### Smaller fixes

- The HUD clock and HTTPS keep working when HOME starts before the system's time service; log lines show the uptime when there is no clock yet.
- Bluetooth audio search falls back to the daemon when the system refuses the menu.
- Resuming a suspended game reuses its launch splash.
- In edit mode, a moved title lands where you drop it.

None of this has run on a console yet. Both build variants compile.

---

## Português

Um port do SwitchU do PoloNX (até c4416d4, 15/11/2026) e de algumas correções do OmniLaunch do ncarvalho99, mais um olhar atento à suspensão: o console parecia só apagar a tela em vez de dormir de verdade.

### Suspensão

- **O menu agora sai da frente antes do console dormir.** Ele pausa a música e para de desenhar, e o sistema espera um instante por ele antes de suspender. Um menu que continuava tocando e desenhando podia impedir o console de dormir de verdade, com a tela apagada e a bateria descendo.
- **Suspender logo depois de acordar** (nos dez segundos em que a tela de bloqueio fica à mostra) deixava o menu desenhando dentro da suspensão. Agora ele vai direto para o preto.
- **Os logs agora dizem se o console dormiu de verdade.** Ao acordar, registram quanto tempo ele ficou fora, quanto desse tempo o sistema continuou acordado e a queda de bateria por hora, com um veredito. Se a suspensão ainda parecer errada, suspenda por dez minutos ou mais, acorde o console e envie os logs em Configurações > Sistema.

### Controles

- **Um Joy-Con sozinho de lado funciona em todos os menus**, com os quatro botões como A/B/X/Y, SL/SR como L/R e o direcional girado para combinar.
- **Controles de GameCube e outros externos** podem comandar o HOME.

### Tela e desempenho

- **Encaixar e tirar da base com o HOME aberto** ajusta a imagem na hora: 1920x1080 na TV, 1280x720 no portátil (antes o portátil usava o tamanho da TV). A suspensão automática segue a regra certa (TV ou portátil) depois da troca.
- **O HOME desenha a 30 quadros por segundo depois de dois segundos sem mexer**, e volta a 60 no próximo toque. Isso já devia existir, mas nunca funcionava.
- A temperatura do chip é lida no firmware 14 em diante; a linha de desempenho do log mostra o clock da GPU e a temperatura.
- O modo de desempenho da base é mantido quando um jogo fecha.

### Bateria

- O bloco **Baterias** e as Configurações rápidas mostram a mesma carga, e os anéis dos controles usam as cores reais dos seus Joy-Cons (também na base).

### Música

- **A música continua de onde parou** quando você volta de um jogo para o HOME. Desligue em Theme Shop > Sons. O salto para a posição salva acontece fora da tela, e o HOME continua respondendo.

### Tela de bloqueio

- **Transições:** Nenhuma, Cortina, Dissolver (padrão, como antes), Zoom ou Varrer, com uma onda suave sobre o relógio e uma vinheta que respira. Configurações > Suspensão > Tela de bloqueio. Ao suspender, o bloqueio é sempre instantâneo.

### Temas e arte

- **Temas manuais:** um tema copiado à mão para `switch/SwitchU/themes` aparece em Instalados junto com os da Theme Shop (se os dois tiverem o mesmo id, vale o da loja). A limpeza automática nunca mexe nessa pasta.
- **Fundos do SteamGridDB:** escolha a qualidade (Desligada, Baixa, Média, Alta) e o desfoque em Menu SwitchU > SteamGridDB.
- **Blocos 2x3** ganham pôster próprio em alta resolução.

### Correções menores

- O relógio do HUD e o HTTPS continuam funcionando quando o HOME abre antes do serviço de hora do sistema; as linhas do log mostram o tempo ligado quando ainda não há relógio.
- A busca de áudio Bluetooth passa pelo daemon quando o sistema recusa o menu.
- Retomar um jogo suspenso reaproveita a imagem de abertura dele.
- No modo de edição, um título movido cai onde você soltou.

Nada disso rodou num console ainda. As duas variantes compilam.
