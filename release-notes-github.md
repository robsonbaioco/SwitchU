# SwitchU 2.5.9

SwitchU keeps a copy of its configuration where a pack's clean install cannot reach it, and puts it back by itself.

## English

### A backup that survives a clean install

- The CNX Updater's clean install deletes every folder at the root of the card except a short list, and `config/SwitchU` is not on it. That folder holds everything SwitchU knows about you -- settings, the SteamGridDB key, folders and game order, themes, artwork -- so a console that went through one came back at the first-run tutorial with all of it gone.
- `backup` is on the list the clean install spares. At every boot, before the menu starts, the daemon now copies what changed in `config/SwitchU` to `backup/SwitchU`: small files are compared byte for byte, artwork and theme media by size, and every file goes in by rename, so a power cut leaves the previous copy. Caches, logs and update staging are left out; they rebuild themselves.
- When a boot finds `config/SwitchU/config.json` missing and the backup has one, it restores the backup instead, before the menu reads anything. The menu comes up as it was, without the tutorial. `config.json` is copied last in both directions, so an interrupted copy is simply redone at the next boot.
- The backup never deletes anything: a theme removed from SwitchU stays in it. Uninstalling SwitchU removes the backup too.
- The first boot on this version copies the whole folder, artwork included, and takes a little longer once. The daemon's log says how long (`[backup] ... in N ms`).

### What the CNX Updater does to SwitchU

- Before a firmware update the CNX Updater looks for a HOME menu theme in `atmosphere/contents/0100000000001000`, which is also where SwitchU lives, and offers only to delete it or to cancel. Every firmware update through it therefore switches SwitchU off. SwitchU Manager's **Update** or **Repair installation** puts it back, and with this release the configuration returns with it if it was lost. The README describes both cases.

---

## Português

O SwitchU guarda uma cópia da configuração onde a instalação limpa de um pack não alcança, e a coloca de volta sozinho.

### Um backup que sobrevive à instalação limpa

- A instalação limpa do CNX Updater apaga todas as pastas da raiz do cartão exceto uma lista curta, e `config/SwitchU` não está nela. Essa pasta guarda tudo o que o SwitchU sabe sobre você -- configurações, a key do SteamGridDB, pastas e ordem dos jogos, temas, artes -- então um console que passou por ela voltava no tutorial inicial sem nada disso.
- `backup` está na lista que a instalação limpa poupa. A cada boot, antes de o menu abrir, o daemon agora copia o que mudou em `config/SwitchU` para `backup/SwitchU`: arquivos pequenos são comparados byte a byte, artes e mídia de temas pelo tamanho, e cada arquivo entra por renomeação, então uma queda de energia deixa a cópia anterior. Caches, logs e arquivos de update ficam de fora; eles se refazem sozinhos.
- Quando um boot encontra `config/SwitchU/config.json` faltando e o backup tem um, ele restaura o backup, antes de o menu ler qualquer coisa. O menu sobe como estava, sem tutorial. O `config.json` é copiado por último nos dois sentidos, então uma cópia interrompida é refeita no boot seguinte.
- O backup nunca apaga nada: um tema removido do SwitchU continua nele. Desinstalar o SwitchU remove o backup também.
- O primeiro boot nesta versão copia a pasta inteira, artes incluídas, e demora um pouco mais uma vez. O log do daemon diz quanto (`[backup] ... in N ms`).

### O que o CNX Updater faz com o SwitchU

- Antes de atualizar o firmware, o CNX Updater procura um tema do menu HOME em `atmosphere/contents/0100000000001000`, que é também onde o SwitchU fica, e só oferece apagar ou cancelar. Toda atualização de firmware por ele, portanto, desliga o SwitchU. O **Update** ou o **Repair installation** do SwitchU Manager o colocam de volta, e com esta versão a configuração volta junto se tiver sido perdida. O README descreve os dois casos.
