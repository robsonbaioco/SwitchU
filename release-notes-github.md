# SwitchU 2.16.1

The first boot after a CNX pack update no longer sits on a black screen for minutes.

## English

Updating the CNX pack empties `/config`, SwitchU's settings included, and the next boot brings them back from `/backup/SwitchU`. Until now that copy ran before HOME appeared: on one console it was 1 GB and three and a half minutes of black screen, long enough to think the console had hung.

- **A progress screen while the settings come back.** HOME starts at once and shows "Restoring your settings", with a progress bar, the files and megabytes copied, and a reminder not to turn the console off. When the settings are back, HOME (or the tutorial, if it was never finished) opens by itself.
- **Downloaded artwork follows behind HOME.** SteamGridDB images and game artwork are copied while you already use the menu, with a small "Restoring artwork" notice at the top; the grid reloads when they are all back. If the console is turned off first, the rest comes back on the next boot.
- **A much smaller backup.** Caches SwitchU rebuilds on its own (decoded SteamGridDB images, preview caches, half-finished theme installs) and files rewritten at every boot are no longer backed up, and the ones already in the backup are removed from it on the first normal boot.
- The settings file now always comes back, even if some other file fails to copy. Holding it back made the tutorial start over and the next boot back up the empty settings over the real ones.
- The daemon logs any moment its main loop stalls for more than a second, and which step took the time: one boot's log showed HOME frozen for 48 seconds waiting on it.

After a pack update the launcher itself still has to be turned back on in SwitchU Manager: the pack deletes the HOME override on purpose. A suggestion to keep launchers like SwitchU across pack updates is open at [CostelaCNX/CNX#286](https://github.com/CostelaCNX/CNX/issues/286).

Not tested on a console yet. To see the progress screen, rename `sdmc:/config/SwitchU` and restart.

---

## Português

A primeira inicialização depois de atualizar o pacote CNX não fica mais minutos numa tela preta.

Atualizar o pacote CNX esvazia a pasta `/config`, inclusive as configurações do SwitchU, e a inicialização seguinte as traz de volta de `/backup/SwitchU`. Até agora essa cópia rodava antes de o HOME aparecer: num console foram 1 GB e três minutos e meio de tela preta, tempo suficiente para achar que o console tinha travado.

- **Uma tela de progresso enquanto as configurações voltam.** O HOME abre na hora e mostra "Restaurando suas configurações", com barra de progresso, os arquivos e megabytes copiados e um aviso para não desligar o console. Quando as configurações voltam, o HOME (ou o tutorial, se ele nunca foi concluído) abre sozinho.
- **As artes baixadas vêm depois, com o HOME já aberto.** As imagens do SteamGridDB e as artes dos jogos são copiadas enquanto você já usa o menu, com um aviso pequeno "Restaurando artes do backup" no topo; a grade recarrega quando tudo voltar. Se o console for desligado antes, o resto volta na inicialização seguinte.
- **Um backup bem menor.** Caches que o SwitchU refaz sozinho (imagens decodificadas do SteamGridDB, caches de prévia, instalações de tema pela metade) e arquivos reescritos a cada inicialização deixaram de ir para o backup, e os que já estavam lá são apagados na primeira inicialização normal.
- O arquivo de configurações agora sempre volta, mesmo que outro arquivo falhe na cópia. Retê-lo fazia o tutorial recomeçar e a inicialização seguinte gravar as configurações vazias por cima das verdadeiras no backup.
- O daemon registra no log qualquer momento em que o loop principal trava por mais de um segundo, e qual etapa demorou: o log de uma inicialização mostrou o HOME congelado por 48 segundos esperando por ele.

Depois de atualizar o pacote, o launcher em si ainda precisa ser reativado no SwitchU Manager: o pacote apaga o override do HOME de propósito. Uma sugestão para preservar launchers como o SwitchU nas atualizações está aberta em [CostelaCNX/CNX#286](https://github.com/CostelaCNX/CNX/issues/286).

Ainda não testado num console. Para ver a tela de progresso, renomeie `sdmc:/config/SwitchU` e reinicie.
