# SwitchU 2.5.7

SwitchU Manager can put SwitchU back after an Atmosphere update removes it.

## English

### Repairing after an Atmosphere update

- SwitchU takes the place of the HOME menu through one file, `atmosphere/contents/0100000000001000/exefs.nsp`. Packs that update Atmosphere and the firmware, the CNX Pack among them, clear `atmosphere/contents`, and that file goes with it. The console then boots into Nintendo's HOME menu, and SwitchU Manager could only say that the override was not found and that the operation was blocked. The Manager's update button said "Up to date", which was true and did not help.
- When the file is missing and the latest release on GitHub is the version already installed, the button now reads **Repair installation**. It downloads that release again and installs it the same way an update does: the archive is checked first, every file goes in by rename, and any failure puts the old files back. The file is restored enabled, your configuration and themes are left alone, and a restart finishes it. When a newer release exists, the usual update restores the file too.

---

## Português

O SwitchU Manager consegue colocar o SwitchU de volta depois que uma atualização do Atmosphere o remove.

### Reparar depois de atualizar o Atmosphere

- O SwitchU ocupa o lugar do menu HOME por meio de um único arquivo, `atmosphere/contents/0100000000001000/exefs.nsp`. Packs que atualizam o Atmosphere e o firmware, como o CNX Pack, limpam `atmosphere/contents`, e esse arquivo vai junto. O console então inicia no menu HOME da Nintendo, e o SwitchU Manager só conseguia dizer que o override não foi encontrado e que a operação estava bloqueada. O botão de atualização do Manager dizia "Up to date", o que era verdade e não ajudava.
- Quando o arquivo está faltando e a release mais recente no GitHub é a versão já instalada, o botão passa a dizer **Repair installation**. Ele baixa essa release de novo e a instala do mesmo jeito que um update: o arquivo é verificado antes, cada arquivo entra por renomeação e qualquer falha devolve os arquivos antigos. O arquivo volta ativado, sua configuração e seus temas ficam intactos, e um reinício conclui. Quando existe uma release mais nova, o update normal também devolve o arquivo.

