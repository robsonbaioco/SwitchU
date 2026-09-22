# SwitchU 2.5.6

An update installs in one restart instead of two, and the console can now say which build it is actually running.

## English

### One restart to install an update

- An update was only half installed by the restart that applied it. The daemon is what unpacks an update, and the daemon is inside the update: whichever copy of it the console loaded at boot is the copy that unpacks its replacement, so it could never be the new one. The menu it launched afterwards was, being read from the card once the files were already in place. Every update therefore ran a new menu against a daemon one release behind, for the whole session, and only the next boot put them back in step -- silently, with nothing anywhere saying so.
- The menu now puts the daemon in place itself, before the restart. It is the one file in the payload that nothing holds open, it is 640 KB, and the extractor swaps every file in by renaming it over its target, so an interrupted write leaves the running daemon untouched. The console comes up on the new daemon, which unpacks the menu half -- the half it cannot touch while the font is open -- and goes straight into it. One restart, both halves on the same version.
- If any part of that fails, the boot that applies the update ends in a second restart instead, which is the old behaviour made deliberate. There is no path where the two halves are left out of step.
- **Installing this release still takes two restarts**, because the code that installs it is 2.5.5's. From the next release on, one is enough.

### Saying which build is running

- The daemon logs its version at startup. The menu has always shown its own on the About tab, and while these two were out of step for a whole session there was nothing anywhere that said which was which.
- The menu's log no longer rotates every time the menu starts -- which is every time a game is closed. Five files used to cover five restarts: on one console, thirty-five minutes, with the session being asked about hours outside them. It now appends until a file reaches 512 KB, so the same five hold hours. The rotate-logs action still rotates whatever the size, since that is what it is for.
- The System memory probe now prints the heap's high-water mark alongside what is allocated at that instant. Every sample ever collected read zero allocated, which says only that those moments are quiet ones; the daemon reserves eight megabytes out of a pool with ten free, and the high-water mark is the figure a smaller reservation has to be sized against. A sample is taken right after an update is unpacked, too -- the largest allocation this process makes.

---

## Português

Um update se instala em um reinício em vez de dois, e o console agora sabe dizer qual build está realmente rodando.

### Um reinício para instalar um update

- Um update só era instalado pela metade pelo reinício que o aplicava. Quem desempacota um update é o daemon, e o daemon está dentro do update: a cópia dele que o console carregou no boot é a que desempacota a própria substituta, então nunca podia ser a nova. O menu que ela lançava em seguida, sim, porque é lido do cartão depois que os arquivos já estão no lugar. Todo update, portanto, rodava um menu novo contra um daemon uma versão atrás, pela sessão inteira, e só o boot seguinte colocava os dois em dia -- em silêncio, sem nada em lugar nenhum dizendo isso.
- Agora o menu coloca o daemon no lugar ele mesmo, antes do reinício. É o único arquivo do pacote que ninguém mantém aberto, tem 640 KB, e o extrator troca cada arquivo renomeando por cima do destino, de modo que uma gravação interrompida deixa o daemon em uso intacto. O console sobe já com o daemon novo, que desempacota a metade do menu -- a que ele não pode tocar enquanto a fonte está aberta -- e entra direto nela. Um reinício, as duas metades na mesma versão.
- Se qualquer parte disso falhar, o boot que aplica o update termina num segundo reinício, que é o comportamento antigo tornado deliberado. Não há caminho em que as duas metades fiquem fora de sincronia.
- **Instalar esta versão ainda leva dois reinícios**, porque quem a instala é o código da 2.5.5. Da próxima em diante, um basta.

### Dizer qual build está rodando

- O daemon registra a versão dele no log ao iniciar. O menu sempre mostrou a dele na aba Sobre, e enquanto os dois ficaram fora de sincronia por uma sessão inteira não havia nada que dissesse qual era qual.
- O log do menu deixa de rotacionar a cada início do menu -- que é toda vez que um jogo é fechado. Cinco arquivos cobriam cinco reinícios: num console, trinta e cinco minutos, com a sessão sobre a qual se perguntava horas fora deles. Agora ele anexa até o arquivo chegar a 512 KB, e os mesmos cinco cobrem horas. A ação de rotacionar logs continua rotacionando de qualquer tamanho, que é para isso que ela serve.
- A sonda de memória do pool System passa a imprimir a marca d'água do heap junto do que está alocado naquele instante. Toda amostra já coletada marcava zero alocado, o que diz apenas que aqueles momentos são tranquilos; o daemon reserva oito megabytes de um pool com dez livres, e a marca d'água é o número contra o qual uma reserva menor tem de ser dimensionada. Uma amostra é tirada logo depois de desempacotar um update, também -- a maior alocação que esse processo faz.
