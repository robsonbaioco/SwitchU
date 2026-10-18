# SwitchU 2.13.1

Vibration joins Quick Settings, a game whose SteamGridDB scan saved only a logo now gets its backdrop, "Restore default" says when there is nothing to restore, and the daemon gives 7 MB back to the memory pool games share with the system.

## English

### Quick Settings

- New **Vibration** switch, the same setting as Settings > Controllers > Vibration.
- L3 opens Quick Settings even when focus was left outside the menu tree after returning from a game (the same fix Plus already had). Every L3 press is now logged with where focus was and what, if anything, blocked it, for the cases where it still does not open.

### Artwork

- The SteamGridDB scan treated a game with only a logo as done, so a backdrop missing from an earlier attempt never arrived. Seen on a downgraded Zelda: it matched at full score but stayed with no backdrop. The scan now tops up a missing backdrop like it already did for logos and covers. Run SwitchU Menu > SteamGridDB > scan once to fill them in.
- **Restore default** (Customize / Active artwork, and inside the Gallery) now says when the game has no custom cover or background from the Gallery, instead of reporting a restore that changed nothing. SteamGridDB artwork is changed in its own picker.

### Memory (issue #2)

- The daemon's heap goes from 8 MB to 1 MB. Its high-water mark never passed 8 KB in 45 samples from a console, including game launches and an update install. Those 7 MB come out of the System pool, the one every sysmodule shares with the services a game starts.

---

## Português

A Vibração entra nas Configurações rápidas, um jogo cuja busca do SteamGridDB salvou só o logo agora recebe o fundo, o "Restaurar padrão" avisa quando não há nada para restaurar, e o daemon devolve 7 MB ao pool de memória que os jogos dividem com o sistema.

### Configurações rápidas

- Novo interruptor de **Vibração**, a mesma opção de Configurações > Controles > Vibração.
- O L3 abre as Configurações rápidas mesmo quando o foco ficou fora do menu depois de voltar de um jogo (a mesma correção que o + já tinha). Todo aperto do L3 agora vai para o log, com onde estava o foco e o que bloqueou, se algo bloqueou, para os casos em que ainda não abrir.

### Artes

- A busca do SteamGridDB considerava pronto um jogo só com logo, então um fundo que faltou numa tentativa anterior nunca chegava. Visto num Zelda com downgrade: casava com nota máxima e continuava sem fundo. A busca agora completa o fundo que falta, como já fazia com logo e capa. Rode a busca em Menu SwitchU > SteamGridDB uma vez para preencher.
- **Restaurar padrão** (Personalizar / Arte ativa, e dentro da Galeria) agora avisa quando o jogo não tem capa ou fundo personalizado da Galeria, em vez de dizer que restaurou sem mudar nada. A arte do SteamGridDB se troca no seletor dela.

### Memória (issue #2)

- O heap do daemon passa de 8 MB para 1 MB. O pico de uso nunca passou de 8 KB em 45 medições de um console, incluindo lançamentos de jogos e a instalação de uma atualização. Esses 7 MB saem do pool System, o que todos os sysmodules dividem com os serviços que um jogo inicia.
