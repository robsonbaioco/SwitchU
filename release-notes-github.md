# SwitchU 2.15.1

Fixes the black screen 2.15.0 left on every console without Frame Tap.

## English

- **HOME no longer hangs on a black screen at start-up.** 2.15.0 asked the system for Frame Tap's service before drawing anything; on a console without Frame Tap that request never returns, so the menu waited forever. The menu now checks that the service exists first.
- **The boot guard now catches a menu that hangs, not only one that crashes.** A menu that has not drawn its first frame 60 seconds after starting (or after the console wakes) is ended and counted as a start-up failure, so the usual recovery follows: safe mode, then rolling a fresh update back to the previous version, then the stock HOME menu.

If 2.15.0 left your console on a black screen: hold Power for 12 seconds, then copy this zip to the root of the SD card from a computer.

---

## Português

Corrige a tela preta que a 2.15.0 deixava em todo console sem o Frame Tap.

- **O HOME não trava mais em tela preta ao iniciar.** A 2.15.0 pedia ao sistema o serviço do Frame Tap antes de desenhar qualquer coisa; num console sem o Frame Tap esse pedido nunca volta, e o menu ficava esperando para sempre. Agora o menu confere antes se o serviço existe.
- **A proteção de boot agora pega um menu que trava, não só um que fecha.** Um menu que não desenhou o primeiro quadro 60 segundos depois de abrir (ou depois que o console acorda) é encerrado e contado como falha de inicialização, e a recuperação de sempre entra: modo seguro, depois a volta de uma atualização recente para a versão anterior, depois o HOME original.

Se a 2.15.0 deixou seu console em tela preta: segure o Power por 12 segundos e copie este zip para a raiz do cartão SD pelo computador.
