# VeloCache

VeloCache este un server key-value în C++17 pentru macOS, cu RESP2, persistență AOF și replicare asincronă master/slave. O singură buclă de evenimente cu `kqueue` gestionează socket-uri TCP nonblocking, fără thread per client.

Serverul și executabilele de test activează fast I/O pentru streams C++ prin `std::ios_base::sync_with_stdio(false)` și `std::cin.tie(nullptr)`. Mesajele de pornire și sincronizare folosesc flush explicit pentru a fi vizibile imediat.

Comenzile implementate sunt `PING`, `SET`, `GET` și `SET ... EX seconds`. Cheile și valorile RESP pot conține spații, newline și octeți NUL. TTL-ul este păstrat la restart și la replicare.

## Compilare

Serverul folosește biblioteca standard C++ și API-urile POSIX/macOS. Sunt necesare Command Line Tools și un compilator C++17.

```bash
make
```

Comanda echivalentă:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread main.cpp storage.cpp client_handler.cpp event_loop.cpp server.cpp resp.cpp -o velocache
```

## Pornirea masterului și a unui slave

În primul terminal:

```bash
./velocache --bind 127.0.0.1 --port 6379 --aof master.aof
```

În al doilea terminal:

```bash
./velocache --bind 127.0.0.1 --port 6380 --aof slave.aof --replicaof 127.0.0.1 6379
```

Fiecare proces folosește propriul fișier AOF. Slave-ul afișează `Replica synchronized` după instalarea snapshot-ului complet.

| Opțiune | Implicit | Utilizare |
| --- | --- | --- |
| `--bind HOST` | `0.0.0.0` | adresa IPv4 pe care ascultă serverul |
| `--port PORT` | `6379` | portul local |
| `--aof PATH` | `velocache.aof` | jurnalul persistent |
| `--replicaof HOST PORT` | absentă | pornește procesul ca slave |
| `--help` | — | afișează opțiunile |

Oprește procesul cu `Ctrl+C`. La repornire, folosește aceeași cale `--aof` pentru a recupera datele.

## RESP2

Cererile sunt array-uri RESP2 de bulk strings. Serverul numără lungimile în octeți și așteaptă un cadru complet înainte să execute comanda. Răspunsurile păstrează valorile exact, inclusiv caracterele de control.

| Comandă | Răspuns RESP2 |
| --- | --- |
| `PING` | `+PONG\r\n` |
| `PING message` | bulk string cu mesajul |
| `SET key value` | `+OK\r\n` |
| `SET key value EX seconds` | `+OK\r\n` |
| `GET key` pentru o cheie existentă | `$<bytes>\r\n<value>\r\n` |
| `GET key` pentru o cheie absentă sau expirată | `$-1\r\n` |
| comandă invalidă | `-ERR ...\r\n` |
| scriere pe slave | `-READONLY ...\r\n` |
| citire de pe slave în timpul sincronizării | `-LOADING ...\r\n` |

O valoare goală are răspunsul `$0\r\n\r\n`, distinct de o cheie absentă. Numele comenzilor și flag-ul `EX` acceptă litere mici sau mari. TTL-ul trebuie să fie un număr întreg pozitiv. Un nou `SET` fără `EX` elimină expirarea anterioară.

Conexiunile RESP rămân deschise pentru mai multe comenzi. Pipelining-ul permite trimiterea mai multor cereri înainte de citirea răspunsurilor, care sunt livrate în ordine. Cererile fragmentate între mai multe citiri TCP sunt acumulate. Erorile de comandă păstrează conexiunea; un cadru RESP invalid produce o eroare și închiderea conexiunii după trimiterea răspunsurilor deja pregătite.

Serverul implementează subsetul RESP2 necesar acestor comenzi. Nu implementează RESP3, negocierea `HELLO`, autentificarea, tranzacțiile, Pub/Sub sau celelalte comenzi Redis. Protocolul intern de replicare este separat și rămâne bazat pe cadre text cu checksum.

Referință: [specificația oficială RESP](https://redis.io/docs/latest/develop/reference/protocol-spec/).

### Clientul TypeScript

`client.ts` folosește exclusiv modulul nativ `net`, fără dependințe externe. Construiește cereri RESP2 și decodează răspunsuri fragmentate, respectând lungimile în octeți UTF-8. Deschide un socket pentru fiecare apel și îl distruge după răspuns; timeout-ul de inactivitate este de cinci secunde.

Cu masterul pornit:

```bash
node client.ts
```

Exemplul testează `PING`, `SET`, `GET` și expirarea după `EX 1`. Comanda a fost verificată cu Node.js 26, care execută acest TypeScript nativ.

Clasa poate fi importată fără să pornească exemplul:

```typescript
import { VeloClient } from './client.ts';

const client = new VeloClient(6379, '127.0.0.1');
await client.set('mesaj', 'Salut\nVeloCache', 10);
console.log(await client.get('mesaj'));
```

API-ul public păstrează `Promise<string>`; o cheie absentă este reprezentată prin șirul `'(nil)'`, iar o eroare RESP respinge Promise-ul.

### Testare cu redis-cli

Dacă ai deja `redis-cli`, selectează modul RESP2:

```bash
redis-cli -2 -h 127.0.0.1 -p 6379 PING
redis-cli -2 -h 127.0.0.1 -p 6379 SET demo "salvat pe master"
redis-cli -2 -h 127.0.0.1 -p 6380 GET demo
redis-cli -2 -h 127.0.0.1 -p 6379 SET temporar "expir în două secunde" EX 2
```

Opțiunea `-2` este descrisă în [documentația oficială redis-cli](https://redis.io/docs/latest/manual/cli/). Compatibilitatea depinde de folosirea comenzilor implementate.

### Comenzile text existente

Conexiunile care încep cu o comandă text păstrează protocolul inițial: o comandă terminată cu newline, un răspuns text și apoi închiderea conexiunii. Parsarea cu `stringstream` și curățarea caracterelor finale `\n`/`\r` sunt păstrate.

```bash
printf 'SET demo salvat pe master\n' | nc 127.0.0.1 6379
printf 'GET demo\n' | nc 127.0.0.1 6380
printf 'SET demo modificat pe slave\n' | nc 127.0.0.1 6380
```

După propagarea asincronă, citirea de pe slave întoarce `salvat pe master`. Scrierea de pe slave întoarce `ERR Read only replica`. Folosește RESP pentru chei sau valori care conțin newline ori octeți NUL.

## Persistență AOF

Fiecare scriere conține o secvență, cheia, valoarea și expirarea absolută în milisecunde Unix. Cheia și valoarea sunt codificate hex, iar un checksum FNV-1a de 64 de biți verifică integritatea înregistrării. Header-ul versionat este `VCAOF1`.

Serverul scrie integral înregistrarea și execută `fsync` înainte de `OK`. La restart, reconstruiește starea din jurnal; timpul petrecut cu serverul oprit este inclus în TTL. `GET` verifică expirarea imediat. O ultimă înregistrare incompletă este trunchiată; o înregistrare completă coruptă oprește pornirea cu o eroare explicită.

Fișierul `<aof>.lock` împiedică folosirea aceluiași jurnal de două procese. Snapshot-ul primit de slave este scris într-un fișier temporar, sincronizat și înlocuit prin `rename`, cu sincronizarea directorului, înainte să devină vizibil. Erorile de persistență opresc procesul înainte să confirme o nouă scriere.

Pentru un test manual, setează o cheie, oprește masterul, pornește-l cu aceeași cale `--aof` și citește cheia. `make clean` păstrează fișierele AOF.

## Replicare asincronă

Masterul acceptă scrieri și poate avea mai multe noduri slave. Fiecare slave deschide o conexiune persistentă și cere `SYNC`; masterul trimite `SNAP_BEGIN`, intrările și `SNAP_END`, apoi mutațiile `UPDATE` în ordine.

Slave-ul validează checksum-ul și secvențele. Instalează snapshot-ul numai când transferul este complet și persistă actualizările în propriul AOF. Cheile și valorile binare sunt păstrate. TTL-ul este transferat ca termen absolut și presupune ceasuri de sistem sincronizate între noduri.

La deconectare, datele existente rămân pe disc, iar citirile produc `LOADING` în RESP sau `ERR Replica syncing` în modul text până la resincronizare. `PING` rămâne disponibil. Reconectarea încearcă la fiecare 500 ms și cere un snapshot nou. Heartbeat-urile verifică secvența o dată pe secundă; lipsa activității timp de cinci secunde declanșează reconectarea.

`OK` confirmă jurnalul masterului. Citirile imediate de pe slave pot vedea o stare anterioară până la propagare. Nu există promovare automată, alegerea unui master sau confirmarea scrierii de către slave.

## Fișierele proiectului

| Fișiere | Responsabilitate |
| --- | --- |
| `storage.h`, `storage.cpp` | date, TTL, codec intern, AOF, snapshot și lock |
| `resp.h`, `resp.cpp` | parsarea cererilor și codificarea răspunsurilor RESP2 |
| `client_handler.h`, `client_handler.cpp` | executarea comenzilor text și RESP |
| `event_loop.h`, `event_loop.cpp` | adapterul nativ kqueue |
| `server.h`, `server.cpp` | accept, citiri/scrieri parțiale, pipelining și replicare |
| `main.cpp` | opțiuni CLI, semnale și pornire |
| `client.ts` | client RESP2 nativ Node.js și exemplu |
| `tests/` | teste de storage, protocol și integrare |

## Limite

Cheile, valorile și fiecare bulk string au maximum 1 MiB. O cerere RESP poate avea maximum 64 de argumente și `4 MiB + 256` octeți; un header de lungime RESP are maximum 32 de octeți, inclusiv markerul și CRLF.

Bufferul de ieșire al unei conexiuni și transferul unui snapshot au limita de 64 MiB. O replică prea lentă sau un snapshot care depășește limita produce deconectare și o nouă încercare. Sunt acceptate maximum 4096 de conexiuni, în limita descriptorilor disponibili. Conexiunile publice inactive sunt închise după 30 de secunde.

Socket-urile sunt nonblocking, dar `fsync`, recuperarea AOF și construirea snapshot-urilor sunt sincrone. Un disc lent sau un snapshot mare poate întârzia celelalte conexiuni. AOF crește cu fiecare scriere; compactarea periodică a jurnalului masterului nu este implementată.

Această versiune rulează pe macOS și nu include autentificare sau TLS.

## Teste

Sunt necesare Python 3 și, pentru verificarea clientului, Node.js cu suport pentru executarea TypeScript.

```bash
make test
```

Testele folosesc directoare temporare și porturi libere. Acoperă:

- RESP2, pipelining, conexiuni persistente, cadre fragmentate și închiderea parțială a socket-ului;
- valori goale, chei absente, UTF-8, CRLF și octeți NUL;
- argumente și TTL invalide, limite de cadru și erori de protocol;
- persistență după `SIGKILL`, TTL absolut și repararea cozii AOF incomplete;
- checksum-uri corupte, secvențe și snapshot-uri întrerupte;
- replicare, modul read-only, reconectare și clientul TypeScript;
- conexiuni parțiale simultane și epuizarea temporară a descriptorilor.
