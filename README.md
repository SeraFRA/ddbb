# progettodatabase

[![ci](https://github.com/SeraFRA/ddbb/actions/workflows/ci.yml/badge.svg)](https://github.com/SeraFRA/ddbb/actions/workflows/ci.yml)

[English](README.en.md) | Italiano

**Un database in C puro, nato da un array di valori contigui e cresciuto fino a un motore con persistenza, indice appreso, memoria semantica — e un layer MCP che lo rende uno strumento chiamabile dagli agenti.**

Progetto didattico per autodidatti: ogni file è un capitolo autonomo e compilabile, verificato con test automatici e **numeri misurati contro la teoria** — mai promessi, sempre misurati. Zero librerie esterne: solo C11 e la stdlib. La storia è per interi commit: ogni capitolo ha il suo post mortem nel messaggio di commit.

---

## Come è nato: la collaborazione

Questo repo è anche il verbale di una collaborazione fra un autore umano e un'AI di programmazione (Mistral Vibe). La prima richiesta non è stata "sistemami il codice" ma *"dammi degli hint per il debug, fammi imparare qualcosa"*: il contratto era capire, non avere. Da lì il metodo, rimasto uguale per dieci capitoli:

1. **l'autore porta la direzione come metafora** — mai come specifica;
2. **l'AI traduce la metafora in meccanica** (nome della tecnica, matematica, controparte nel mondo reale);
3. **la meccanica va sotto misura**: numeri confrontati con la teoria, sempre;
4. **i test sono rigidi e vincolano anche l'AI**: la demo esce con EXIT≠0 se un contratto salta, e ogni bug dell'AI finisce in un post mortem scritto nel punto esatto del codice dove è successo.

Le metafore sono state il vero motore del design. Le principali:

| l'autore ha detto | tradotta in meccanica, è diventata | nel capitolo |
|---|---|---|
| "un dipendente che si ricorda dove mette i prodotti negli scaffali, e li trova grazie alla probabilità delle volte prima" | **indice learned**: la previsione della posizione sostituisce la partenza a caso; l'invariante è che chi cambia la sua "memoria" ricostruisce la tabella | `database_learned.c` |
| "un database che si comporti come la memoria umana: ogni cosa collegata e vicina in uno spazio semantico probabilistico" | **MinHash + banding LSH**: lo spazio semantico è l'insieme dei bigrammi posizionali; due chiavi sono "vicine" se condividono pezzi; la probabilità d'incontro è la firma | `database_semantic.c` |
| "un db che non sia puramente determinista: dia risposte probabili, rispondendo alle domande con le sue entry" | la **confidenza dichiarata**: hit esatto → 1.0; refuso → top-k col valore, gruppo dei pari-merito quando la verità è ambigua, silenzio quando non c'è niente da dire | `db_chiedi` / `motore_chiedi` |
| "i dati che scadono non vanno controllati a ogni lettura" | **timing wheel**: la scadenza smette di essere un calcolo e diventa un luogo dell'anello | `database_ring.c` |

Il punto onesto: l'AI ha proposto anche capitoli per conto suo (Bloom/quotient, persistenza), ma le intuizioni che hanno girato il progetto verso il probabilistico sono tutte dell'autore — ed è lì che sta il valore della collaborazione: le metafore umane hanno scelto **quali** tecniche meritavano di esistere, l'AI ha scelto **come** farle funzionare e ne ha pagato i bug, documentandoli.

Il metodo si è affilato col tempo fino all'esperimento **invariants-first** del decimo capitolo: contratto scritto prima del codice. Prima il ciclo era *codice → bug → post mortem*, per tre capitoli di fila; poi il ciclo si è invertito — 8 trappole catturate a tavolino prima di scrivere una riga, 1 al primo run, e il bug scappato ha detto esattamente quale asserzione mancava nei test. Il verdetto dell'esperimento è nel commit del decimo capitolo e nell'header del motore.

Cosa ha insegnato, in una frase: una metafora umana più un test che sa fallire valgono più di cento righe generate — perché vincolano la direzione, non solo la sintassi.

---

## La mappa dei capitoli

| # | file | what's in it | nel mondo reale si chiama | il numero da ricordare |
|---|------|--------------|---------------------------|------------------------|
| 0 | `database.c` | il punto di partenza: array contiguo di entry, il codice originale con i suoi bug didattici — lasciato intonso: è il fossile | — | — |
| 1 | `database_debug.c` | la prima sistemazione: bug di frontiera, strumenti di ispezione, marker `FIX` | debugging | — |
| 2 | `database_indexed.c` | buffer append-only + indice hash | **Bitcask** (append-only log + hash in RAM) | get O(1), catene di probing |
| 3 | `database_compact.c` | tombstone, compattazione O(n), percentili p50/p90/p99 | il **merge** degli LSM | la distribuzione batte la media |
| 4 | `database_persist.c` | CRC-32 per entry, snapshot compattati, load validato: prefisso sicuro | checksum + snapshot (SQLite, RocksDB) | 1 byte corrotto = persa solo quell'entry |
| 5 | `database_learned.c` | spline sulla CDF delle chiavi al posto dell'hash di partenza | **learned index** (Kraska 2017) | l'invariante: cambi la funzione di partenza → ricostruisci |
| 6 | `database_semantic.c` | bigrammi posizionali + MinHash + banding LSH: il db che capisce i refusi | **MinHash/LSH** (Broder 1997) | refusi ripescati nel top-8, ambiguità dichiarata |
| 7 | `database_ring.c` | la scadenza come LUOGO (anello di tick) + delete a backshift | **timing wheel** (kernel Linux) | scadenze in batch, O(1) ammortizzato |
| 8 | `database_quotient.c` | Bloom e quotient filter, la contabilità dei bit | **Bloom 1970 / Bender 2012** | falsi positivi misurati 0.777% vs 0.82% di teoria |
| 9 | `database_persist_semantic.c` | la fusione: la risposta semantica viene dal disco, col valore — con l'esperimento *invariants-first* | — | 0 sbagliate su 60000 verifiche dal disco |
| 11 | `mcp_server.c` + `mcp_stdio.c` | il layer MCP: il motore come strumento per un agente LLM — 10 tool, confidenza sempre dichiarata, `presente` batch | **MCP** (Model Context Protocol) | get p99 2.3 µs vs 10²..10³ ms del giro LLM |
| 12 | `prefetch.c` + `prefetch_hook.c` | il prefetcher speculativo: un hook pre_agent legge la traccia dei pensieri dell'agente, predice le chiavi che serviranno e le parcheggia accanto al motore con TTL in richieste; il server MCP consuma lo staging e conta i colpi | **prefetch speculativo** (quello delle CPU, applicato all'IO dell'agente) | hit-rate 1.00 sul filo intero del banco, soglia 0.50 |
| 13 | `wal.c` + `misura_fsync.c` | il write-ahead log: le intenzioni si inchiostrano una per una (fwrite+fflush+fsync) prima di toccare il motore; il replay ricostruisce tutto fra due snapshot, la coda marcia si tronca | **WAL** (Bitcask, Postgres, SQLite) | 216 µs per put durabile (p50), il replay è esatto: 212/212 |

## Il motore unico

I capitoli sono dimostrativi monofunzione; `motore.h` + `motore.c` li consolidano in **un solo sistema** con API pubblica (handle opaco) e contratto scritto:

```c
motore_t *m = motore_apri();
motore_put(m, chiave, klen, valore, vlen, ttl);     /* con TTL */
motore_get(m, chiave, klen, &vlen);                 /* o NULL: mai bugie */
motore_chiedi(m, query, qlen, k, risposte, &cand);  /* esatto -> conf 1.0;
                                                       miss -> top-k semantico
                                                       col valore */
motore_intervallo(m, da, dalen, a, alen, k, out, &tot, NULL); /* [da..a]:
                                                       l'hash non sa farlo */
motore_avanza(m, t);                                 /* l'orologio e' tuo [I8] */
motore_salva(m, "m.db");                             /* formato del capitolo 9 */
motore_audit(m, &dichiarate);                        /* l'invariante [I7] in API */
```

Il cuore architetturale è l'invariante **[I7]: ogni entry viva È la versione corrente della sua chiave** — ogni morte (delete, scadenza, sorpasso) lascia il segno nello stesso istante in cui lascia il conto. Da qui: lo scan per intervalli è una passeggiata di byte senza lookup, e la coda post-compattazione si fonde da sola.

Cosa **non** è entrato nel motore, e perché (l'obitorio): la delete a backshift (il motore ha la compattazione che ripulisce i tombstone: il backshift vince solo senza), il quotient filter (è un indice di appartenenza per chi non ha l'indice esatto), la spline lineare del capitolo 5 (qui il recinto a chiave piena è esatto per costruzione).

## Il layer MCP: il motore come strumento (capitolo 11)

Metafora: *l'agente non deve conoscere il motore, deve poterlo chiamare — e ogni risposta deve dichiarare quanto vale.* Meccanica: un server MCP (spec 2025-06-18, JSON-RPC 2.0 su stdio) sopra l'**API pubblica** del motore, senza toccarne l'interno: `mcp_stdio.c` (il guscio: una riga JSON in, una riga JSON out), `mcp_server.c` (il nucleo `mcp_gestisci`, puro e testabile in process), `json.c` (parser minimale scritto a mano: zero dipendenze, e **una fonte di verità sola** — lo usa il filo e lo usa il banco per verificare le risposte). Il contratto è [M1]..[M9], scritto prima del codice:

- **meno round-trip possibile**: `chiedi` torna chiave E valore E confidenza (mai solo le chiavi: la seconda chiamata per i valori è il bug economico); `intervallo` torna il batch più il totale; `presente` è batch (fino a 256 chiavi per chiamata).
- **la confidenza viaggia sempre**: confidenza bassa = l'agente sa che può ritentare con un'altra formulazione. Auto-correzione a un solo giro in più.
- **`presente` come pre-filtro**: "certamente no" [I9] a costo quasi zero — l'agente decide se vale la pena spendere la chiamata.
- **`audit` come tool**: il contratto rotto è visibile anche all'agente, che può non fidarsi e verificare.
- **i limiti sono la description**: la similarità è SINTATTICA (bigrammi + MinHash), non semantica — è la parte che l'LLM legge prima di chiamare.
- **l'orologio è del chiamante [I8]**: `avanza` è un tool; il server non legge mai l'orologio da muro.

I numeri del banco `demo_mcp` (misurati, gate incluso): get p50 2.1 µs / p99 2.3 µs, risposta 149 byte (~37 token); `chiedi` su refusi p50 9.5 µs, verità nel top-8 all'88%; `presente` batch-64 in 15 µs; `intervallo` su finestre di 100 in 23 µs. Un compito tipico è **un round-trip solo**: il costo di ogni interrogazione è il giro LLM (10²..10³ ms di generazione), e il layer lo minimizza. Il banco misura anche il ciclo di vita del bloom: 90.6% di "forse" sugli alieni prima della ricomposizione (filtro stantio, 3072 bit), 0/32 dopo il giro del disco — il derivato si raddrizza da solo, dichiarato non nascosto.

Posizionamento, detto onestamente: non competiamo con Mem0/Zep/Letta sul loro piano (loro sono logica di estrazione LLM-based); siamo **il piano sotto** — il substrato di storage in-process, deterministico, zero-dipendenze, TTL nativo. Il confronto giusto è con Redis/Qdrant come backend, e la nicchia è la memoria embedded/edge dove una chiamata LLM per `add()` è inaccettabile. Il costo nostro è in domanda (ogni interrogazione è generazione di token), il loro in scrittura (ogni messaggio ingerito passa da un LLM): per un agente che scrive molto e interroga selettivamente, il nostro modello è più economico di un fattore enorme.

E l'uso che chiude il cerchio: montato come MCP server in un agente (per esempio Mistral Vibe), il motore diventa nativamente chiamabile — il debugging del progetto passa dallo strumento che il progetto fornisce. Il banco resta la misura; l'agente è la testimonianza.

## Il prefetcher speculativo: la memoria che guarda i pensieri (capitolo 12)

Metafora: *la CPU prefetcha le istruzioni guardando lo stream; il database prefetcha guardando lo stream di ragionamento dell'agente.* Estrarre costa ~zero, ragionare costa caro: allora estraiamo più di quanto serve, adesso — ma in parcheggio, non in contesto: la finestra dell'LLM è la risorsa cara, ciò che è estratto non entra finché non serve.

La geometria è due processi e un file:

- **il produttore** è l'hook `pre_agent` (`prefetch_hook.c`): a ogni turno riceve il prompt, legge la traccia dei pensieri recenti dal journal della sessione, predice fino a 4 chiavi e le scrive in uno staging atomico (`<db>.prefetch`, tmp + rename); se ha qualcosa, inietta nel contesto un handle one-liner ("disponibili anche questi, confidenza 0.4/0.31/0.2") — l'agente tira solo ciò che gli serve. Fail-open totale: ogni errore dell'hook è silenzio, la sessione non si ferma mai per colpa della speculazione.
- **il consumatore** è il server MCP: a ogni richiesta consuma lo staging (una generazione, una consumazione), parcheggia le predizioni accanto al motore e le conta; una `get` riuscita su una chiave parcheggiata è un **colpo**; il tool `prefetch_stat` espone hit-rate e bilancio in byte (risparmio delle scoperte evitate meno spesa degli handle iniettati).
- **la speculazione scade da sola**: le predizioni nascono con TTL di 32 richieste e muoiono in batch O(1) ammortizzato — il prefetcher sbagliato si smaltisce da sé, speculazione con oblio incorporato. E il dedup non prefetcha ciò che l'agente ha già in traccia.

Il post mortem del capitolo, che è anche la sua lezione: la prima versione prediceva con `chiedi` del motore — ma i grammi del motore sono **posizionali** (`pos` entra nell'hash), quindi `chiedi` è un fuzzy-PREFIX, non una ricerca per tema: prediceva solo le chiavi già verbatim nella traccia, proprio quelle che il dedup scarta. La similarità per tema vive nel layer: bigrammi non posizionali, finestre di 5 token sulle ultime 64, Jaccard con soglia 0.12, scan delle chiavi via `motore_intervallo`. Effimera, mai un secondo indice persistente.

I numeri del banco `demo_prefetch` (misurati, gate incluso): `predici` su 8 KB di traccia p50 906 µs / p99 1.16 ms — **una volta per turno**, contro i 10²..10³ ms del giro LLM; `consuma` p50 61 µs a ogni richiesta; `tocca` (il colpo) ~0 µs. Il gate end-to-end attraversa il filo MCP: hit 1, spesa 120 byte, risparmio 500 byte, bilancio +380, hit-rate 1.00 contro soglia 0.50 — EXIT≠0 se un tool mente sulla confidenza o se il bilancio diverge. E il limite dichiarato: le predizioni sono "forse" (similarità sintattica sulla traccia, non semantica), l'hit-rate basso significa che la speculazione inquina più di quanto risparmia — e il dato si dichiara anche quando smentisce l'idea.

## Il write-ahead log: la memoria che sopravvive al crash (capitolo 13)

Metafora: *il diario di bordo.* La nave può affondare a ogni ondata, ma il diario si scrive riga per riga e ogni riga è inchiostrata prima della successiva: chi lo rilegge ricostruisce il viaggio fino all'ultima riga leggibile. Il dolore, numerato nel banco: ogni `salva` riscrive il file intero (64 ms a 50k chiavi) e fra due salva tutto vive solo in arena — un crash perde tutto dall'ultimo snapshot.

La meccanica è un file append-only accanto al db (`<db>.wal`), il contratto [W1]..[W6] scritto prima del codice in `wal.h`:

- **il log viene prima** [W1]: il layer scrive il record PRIMA di toccare il motore — un crash fra i due ri-applica, il contrario perderebbe la put accettata.
- **il trittico della durabilità** [W2]: `fwrite + fflush + fsync` per record. Il post mortem del capitolo è nella misura stessa: fsync senza fflush sincronizza un kernel che non ha ancora i byte (vivi nel buffer del processo) — la prima misura diceva 70 µs, quella onesta 216 µs. La decisione sulla politica di durabilità è chiusa dai numeri di `misura_fsync`, non dai gusti: fsync per put (216 µs, invisibile al ritmo dell'agente), group commit escluso perché il suo batch non si riempie MAI a quel ritmo — degenerebbe in "fsync solo a salva", cioè in non-WAL.
- **il prefisso è sicuro** [W3]: CRC-32 per record (il contratto del capitolo 4): un record rotto ferma il replay lì, si perde solo la coda non ancora inchiostrata — e la coda marcia si **tronca**, perché un record nuovo non deve mai finire dietro una coda che il replay non leggerebbe mai.
- **il tick sta nel record** [W4]: il replay ricostruisce il `nato` di ogni entry — le TTL rivivono col tempo vero, non col tempo del caricamento. Limite dichiarato: caricare lo snapshot a un tick sopra i record allunga le TTL replayate (il tick nel file PDB1 è formato v2, sta in obitorio).
- **si taglia dopo lo snapshot, mai prima** [W5]: il WAL vive fra due `salva`; un crash fra snapshot e taglio ri-applica l'ultimo batch (idempotente), il contrario lo perderebbe.
- **uno scrittore** [W6]: single-thread per contratto, come il motore.

I numeri del banco `demo_wal` (misurati, gate incluso): replay esatto di 212/212 record (200 put + 12 del, di cui uno su chiave mai esistita — legale), 239 vive ricostruite con 199 valori verificati uno a uno; la TTL replayata a tick 1 muore a tick 6 come la viva; 50 byte di coda marcia scartati, dichiarati e troncati; il taglio dopo lo snapshot azzera il segmento e il replay successivo applica solo il batch nuovo. Il prezzo: put nuda p50 0.90 µs contro put inchiostrata p50 212 µs — ratio 236x, e a 200 put il WAL costa 42 ms in tutto: la durabilità non è gratis, e il banco lo dice invece di nasconderlo. Dal filo MCP (`demo_mcp`): la put dopo il salva sopravvive al crash senza alcun salva in mezzo — il gate [W1] attraversa il filo intero.

## Compilare ed eseguire

Serve `gcc` (MinGW su Windows, gcc su Linux) e `make` (su Windows lo porta MSYS2 come `mingw32-make`; a mano: `gcc -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=199309L -O2 -o motore_demo motore.c demo_motore.c`). Niente dipendenze.

```bash
make motore_demo        # il motore + il banco di prova: gate per OGNI invariante
make mcp_server         # il server MCP su stdio: ./mcp_server [file.db [tick]]
make demo_mcp           # il banco del layer MCP: gate [M1]..[M9] + MISURA
make prefetch_hook      # l'hook pre_agent: ./prefetch_hook [file.db]
make demo_prefetch      # il banco del prefetcher: gate [P1]..[P7] + MISURA
make database_quotient  # qualunque capitolo, uno per volta
./motore_demo           # esce con EXIT!=0 se un contratto salta
```

Su Windows: il Device Guard può bloccare le exe appena compilate nella cartella del progetto (exit 126). Attendere qualche secondo e ripetere, o compilare in una directory fuori dai criteri.

Il push è protetto dalla CI: a ogni push un job ubuntu+gcc ricompila ogni capitolo e **lo fa girare come banco di prova** — un contratto che salta rende rosso il badge in testa al README. Le prime due prede della CI: il porting Linux (quattro capitoli compilavano solo su MinGW perché il ramo non-Windows usava `struct timespec` e `clock_gettime` senza includere `<time.h>`; e anche includendolo, con `-std=c11` stretto la glibc non dichiara `clock_gettime` senza `_POSIX_C_SOURCE`) e il link di `pow()`: su Linux vive in libm, che MinGW piega dentro il runtime — ecco perché in locale linkava. Ora Makefile: `_POSIX_C_SOURCE` nei CFLAGS, `-lm` negli LDLIBS.

## I numeri del motore (misurati, non promessi)

- 50000 chiavi vive su disco (2.46 MB): compattazione con ordinamento 64 ms, ricarica con ricostruzione di semantica, porte e anello 64 ms, 0 sbagliate su 60000 verifiche.
- Intervallo `u0002500..u0003499`: 833 corrispondenze == 833 del conteggio indipendente, in 865 passi; il merge con la coda regge su tre conti.
- Get esatti: p50 0.100 µs; porta Bloom: 99.22% degli alieni risolti senza toccare lo slot.
- Refusi di una cifra: 99.9% di risposte dichiarate, verità nel top-8 al 94.2%, gruppo dei pari-merito 4.9 (l'ambiguità è del refuso, e il db la dichiara invece di fingere).
- 20000 scadenze nello stesso tick: gestite in 0.1 ms **senza leggere una chiave**.

## Le lezioni di metodo

Questo repo è anche un diario di come si impara a costruire cose che non si sanno ancora costruire; capitolo per capitolo:

- **Prima misurare, poi credere.** Ogni tecnica arriva con il suo numero confrontato con la teoria (Bloom: 0.777% vs 0.82%; banding: cattura prevista vs top-k misurato).
- **Stampare non è bloccare.** Un test che stampa gli errori senza far fallire il processo non è un test: questa lezione costa due capitoli di fila nel post mortem.
- **Confronto senza soglia non è gate.** Il passaggio da "0 == 0, verde!" a "470/500" è il cuore del post mortem del motore.
- **Invariants-first.** Il contratto scritto prima del codice sposta la cattura dei bug a sinistra (nel design). Nel decimo capitolo: 8 trappole catturate a tavolino, 1 al primo run. Nel motore: il bug vero l'ha trovato un warning `-Wall` prima ancora del run.
- **L'onestà è un'API.** Confidenze dichiarate, silenzio invece dell'invenzione, "forse" del Bloom distinto dal "no" del probe.

## Struttura

```
database*.c            # i nove capitoli + l'originale: ognuno si compila da solo
motore.h / motore.c    # il motore unico (API pubblica, handle opaco)
demo_motore.c          # il banco di prova: gate a ogni invariante
json.h / json.c        # il parser JSON minimale: una fonte di verita' per il filo
mcp_server.h / mcp_server.c  # il nucleo MCP: 10 tool, contratto [M1]..[M9]
mcp_stdio.c            # il guscio sottile: stdin/stdout, un oggetto per riga
demo_mcp.c             # il banco del layer: gate e MISURA (latenze, token)
prefetch.h / prefetch.c      # il prefetcher speculativo: contratto [P1]..[P9]
prefetch_hook.c        # l'hook pre_agent: traccia -> predizione -> staging
demo_prefetch.c        # il banco del prefetcher: gate e MISURA (hit-rate, bilancio)
wal.h / wal.c          # il write-ahead log: contratto [W1]..[W6], il replay esatto
demo_wal.c             # il banco del WAL: crash simulati, coda marcia, taglio
misura_fsync.c          # la MISURA che ha chiuso la decisione sulla durabilita'
.vibe/hooks.toml       # l'hook montato: pre_agent, fail-open
Makefile               # make motore_demo | make mcp_server | make <capitolo>
.gitignore             # eseguibili e file di prova non entrano nel repo
```

## Dove proseguire

Il motore dichiara i suoi limiti nell'header di `motore.c`: single-thread, statistiche via `printf` (progetto didattico), risposte vive fino alla prossima put. I passi naturali dopo: il tick dentro il file PDB1 (formato v2, per le TTL replayate a orologio mobile), iteratori stabili, concorrenza a un estremo per volta.

## Licenza

MIT — vedi [LICENSE](LICENSE).
