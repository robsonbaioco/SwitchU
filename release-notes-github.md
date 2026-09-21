# SwitchU 2.5.5

A Joy-Con on its own can drive the menu, any player's controller can drive it, and Change Grip/Order can hand player 1 to somebody else.

## English

### A Joy-Con held sideways

- A single left Joy-Con could move around the menu and confirm nothing: SwitchU never told the system that a Joy-Con is held sideways here, so the system reported it upright, and upright a left Joy-Con has a d-pad, a stick, SL/SR, L/ZL and − -- no action buttons whatsoever. The console's own menu asks for the sideways orientation, which is what turns those four direction buttons into A/B/X/Y. SwitchU now asks for it too.
- One case is not covered by that, and never was on the console either: half of a pair that is still registered as a pair. The system hands it the upright layout no matter how it is held, and there are no action buttons. Register it as a player of its own in Change Grip/Order -- SL+SR -- and it works sideways.

### Any player's controller

- The menu listened to player 1 and to the console in handheld mode, and to nothing else. A second controller could not move the cursor. It now reads all eight player slots, the way a home menu should.

### Change Grip/Order

- It opened with whichever controller had opened it already seated in player 1, and that one could never be replaced: every other controller disconnected and could be reordered, player 1 could not. SwitchU was asking the applet to take the current connections over. It now opens from nothing, the way the console does, so the order can actually be redone.
- The applet is also handed the controller configuration of whatever launches it, and the daemon had never published one -- it asked the system for a setting it had never set, and passed on the empty answer. It now publishes the full set of controller styles and the sideways orientation before opening the applet, so a lone Joy-Con can be registered there. A failure to read that configuration used to stop the applet from opening at all; it no longer does.

### Logs

- The menu log records which controllers are connected, in which style, and whether a Joy-Con pair is missing a half -- once, and again whenever it changes. A controller that cannot press A can now be diagnosed from a log instead of from memory.

---

## Português

Um Joy-Con sozinho consegue usar o menu, o controle de qualquer jogador consegue usar o menu, e o Mudar a Ordem consegue passar o jogador 1 para outro controle.

### Um Joy-Con na horizontal

- Um Joy-Con esquerdo sozinho andava pelo menu e não confirmava nada: o SwitchU nunca avisou ao sistema que aqui o Joy-Con é segurado na horizontal, então o sistema o reportava na vertical -- e na vertical o Joy-Con esquerdo tem direcional, alavanca, SL/SR, L/ZL e − e nenhum botão de ação. O menu do próprio console pede a orientação horizontal, que é o que transforma aqueles quatro direcionais em A/B/X/Y. O SwitchU passa a pedir também.
- Um caso continua de fora, e sempre esteve também no console: a metade de um par que ainda está registrada como par. O sistema entrega o layout vertical a ela independentemente de como você segura, e não há botões de ação. Registre-a como jogador próprio no Mudar a Ordem -- SL+SR -- e ela funciona na horizontal.

### O controle de qualquer jogador

- O menu escutava o jogador 1 e o console no modo portátil, e mais nada. Um segundo controle não movia o cursor. Agora lê os oito lugares de jogador, como um menu principal deve fazer.

### Mudar a Ordem

- Ele abria com o controle que o tinha aberto já sentado no jogador 1, e esse nunca podia ser trocado: todos os outros desconectavam e podiam ser reordenados, o jogador 1 não. O SwitchU pedia ao applet que assumisse as conexões atuais. Agora ele abre do zero, como no console, e a ordem pode de fato ser refeita.
- O applet também recebe a configuração de controles de quem o abre, e o daemon nunca tinha publicado uma -- pedia ao sistema um ajuste que jamais fizera, e repassava a resposta vazia. Agora ele publica o conjunto completo de estilos de controle e a orientação horizontal antes de abrir o applet, para que um Joy-Con sozinho possa ser registrado ali. Uma falha na leitura dessa configuração impedia o applet de abrir; não impede mais.

### Logs

- O log do menu registra quais controles estão conectados, em qual estilo, e se falta uma metade de um par de Joy-Con -- uma vez, e de novo sempre que mudar. Um controle que não consegue apertar A passa a ser diagnosticável por log, e não por memória.
