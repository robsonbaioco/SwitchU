# SwitchU 2.5.8

SwitchU Manager can install updates again, including the file it runs from.

## English

### Updating from SwitchU Manager

- Every update started from SwitchU Manager failed with "Unable to back up sdmc:/switch/SwitchU-Manager/SwitchU-Manager.nro" and was rolled back. The Manager read its font from inside its own .nro and held that file open for as long as it ran. Horizon will not rename an open file, and replacing a file means renaming it. Nothing was damaged -- the rollback put every file back -- but nothing was installed either, the repair added in 2.5.7 included.
- The Manager now reads the font into memory when it starts, and lets go of its own file before an install begins, so an update replaces the Manager like any other file.
- The fix lives in the Manager that runs the update, so a 2.5.6 or 2.5.7 Manager still cannot replace itself. This release therefore also carries **SwitchU.zip**, the same payload without the Manager's .nro. Those Managers choose it on their own, so "Update to v2.5.8" works from them: it installs the menu and the daemon, puts the HOME menu override back if it was missing, and leaves the old Manager in place. The Manager catches up with the next update from SwitchU's own Update tab, or by copying the complete archive to the card.
- **For a manual install, download SwitchU-2.5.8.zip**, the complete archive. SwitchU.zip is only there for the old Managers.
- With SwitchU running, the menu holds its own font open in `switch/SwitchU`, so an update from the Manager fails the same way and rolls back. Update from the Update tab in SwitchU instead; the Manager is for when SwitchU is off or missing.

---

## Português

O SwitchU Manager volta a conseguir instalar updates, inclusive o arquivo de onde ele mesmo roda.

### Atualizar pelo SwitchU Manager

- Todo update iniciado pelo SwitchU Manager falhava com "Unable to back up sdmc:/switch/SwitchU-Manager/SwitchU-Manager.nro" e era desfeito. O Manager lia a fonte de dentro do próprio .nro e mantinha esse arquivo aberto enquanto rodava. O Horizon não renomeia um arquivo aberto, e substituir um arquivo é renomeá-lo. Nada era danificado -- o rollback devolvia todos os arquivos -- mas nada era instalado também, incluindo o reparo que entrou na 2.5.7.
- Agora o Manager lê a fonte para a memória ao abrir e solta o próprio arquivo antes de começar uma instalação, então um update substitui o Manager como qualquer outro arquivo.
- A correção fica no Manager que roda o update, então um Manager 2.5.6 ou 2.5.7 continua sem conseguir se substituir. Por isso esta release também traz o **SwitchU.zip**, o mesmo pacote sem o .nro do Manager. Esses Managers escolhem ele sozinhos, então "Update to v2.5.8" funciona a partir deles: instala o menu e o daemon, recoloca o override do menu HOME se ele estava faltando e deixa o Manager antigo onde está. O Manager se atualiza no próximo update feito pela aba Update do próprio SwitchU, ou copiando o pacote completo para o cartão.
- **Para instalar manualmente, baixe o SwitchU-2.5.8.zip**, o pacote completo. O SwitchU.zip só existe para os Managers antigos.
- Com o SwitchU rodando, o menu mantém a própria fonte aberta em `switch/SwitchU`, então um update pelo Manager falha do mesmo jeito e é desfeito. Nesse caso, atualize pela aba Update do SwitchU; o Manager é para quando o SwitchU está desligado ou faltando.
