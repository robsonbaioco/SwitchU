# SwitchU 2.13.3

The console now reconnects to the internet by itself after the connection drops, like the stock HOME menu.

## English

### Internet reconnects on its own

- The Switch only keeps a network connection while some program asks for it. The stock HOME menu keeps such a request open all the time, which is why the stock console reconnects after a drop. SwitchU replaces that menu and never asked, so after a drop — with no game running that wanted the internet — nothing asked to reconnect, and you had to reconnect by hand.
- The daemon now keeps a **persistent network request** open, as the stock menu does: when the connection drops, the system holds the request and keeps trying to bring the network back.
- Every change in that request and in the connection status is written to the log (`[net]`), so the next logs show when the connection dropped and how long it took to come back.
- **Play offline** keeps working: the request just waits while the game's wireless is off and resumes when it comes back on.

---

## Português

O console agora reconecta à internet sozinho depois que a conexão cai, como o menu HOME original.

### A internet reconecta sozinha

- O Switch só mantém uma conexão de rede enquanto algum programa pede por ela. O menu HOME original deixa um pedido desses aberto o tempo todo, e é por isso que o console original reconecta depois de uma queda. O SwitchU substitui esse menu e nunca pedia, então depois de uma queda — sem nenhum jogo aberto querendo internet — ninguém pedia para reconectar, e era preciso reconectar à mão.
- O daemon agora mantém um **pedido de rede persistente**, como o menu original: quando a conexão cai, o sistema segura o pedido e continua tentando trazer a rede de volta.
- Cada mudança nesse pedido e no estado da conexão vai para o log (`[net]`), então os próximos logs mostram quando a conexão caiu e quanto tempo levou para voltar.
- **Jogar sem internet** continua funcionando: o pedido só espera enquanto o Wi-Fi do jogo está desligado e volta quando ele religa.
