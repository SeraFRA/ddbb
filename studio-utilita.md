# A che serve, quanto aiuta, quanto è originale

Studio d'utilità del motore (memoria di lavoro dell'agente: key-value con
scadenza contrattuale, similarità sintattica, layer MCP, prefetcher
speculativo, lavagna gateway) — 22 novembre 2026, dopo 14 capitoli, CI
verde, e tre corse di campo con agenti veri.

Metodo delle fonti (come da regole del repo, dichiarate per quel che
valgono): ogni affermazione **sul nostro** motore viene dai banchi del
repo o dalle corse con agenti veri documentate nel README (il banco vivente; i numeri
riproducibili: i gate girano in CI a ogni push); ogni affermazione
**sugli altri** viene dalle loro pagine ufficiali, dai cataloghi
indipendenti (Glama, Mnemoverse) o dai blog tecnici con benchmark
pubblic, aggiornati a novembre 2026.

---

## 0. Il prodotto in tre righe

Un key-value in-process con scadenza **contrattuale** (timing wheel),
ricerca per **similarità sintattica** (bigrammi posizionali + MinHash),
**confidenza dichiarata in ogni risposta**, audit visibile al chiamante,
WAL crash-safe, un padrone solo per file — più il layer MCP (cap. 11),
il prefetcher che guarda la traccia di ragionamento (cap. 12), la
memoria inchiostrata (cap. 13) e la lavagna multi-agente (cap. 14).
5.534 righe di C11, zero dipendenze oltre la stdlib, un binario.

Non è né un memory server (piano A: estrazione LLM a ogni scrittura)
né un database SQL dietro MCP (piano B): è **il substrato sotto entrambi**.

---

## 1. In che casi serve

### 1.1 La memoria di lavoro di un agente di coding, che sopravvive alla chat

Il caso d'uso principe, provato sul campo per due settimane in questo
stesso repo. Un agente CLI lavora per sessioni brevi su un progetto
lungo; ciò che **non è ancora nel repo** — piani in corso, trappole
operative pagate, decisioni in bilico — vive nel motore a namespace
(`in-corso/...`, `nota/...`); quando una cosa entra nel repo la voce
**muore con un `del` esplicito** (niente duplicazione: il repo è la
verità durevole, la memoria il brogliaccio). Dato di campo: alla
riapertura di una chat nuova, l'hook ha proposto al turno 0 la chiave
`in-corso/code-review-subagent` (confidenza 0.22); una `get` = il piano
intero (3,5 KB) in una chiamata, e l'agente ha ripreso il lavoro a
velocità da primo turno.

Perché il mercato lo chiede: la nuova ondata MCP è tutta
"local-first, project-scoped memory for coding agents" (Glama, novembre
2026, 5.520 server nel catalogo). Ma i nuovi arrivati sono indici
SQLite o graft: nessuno mette la **morte** al centro del contratto.

### 1.2 Il cold start: la gobba misurata

Il prefetcher vale come ottimizzatore dell'**apertura**: nella finestra
in cui l'agente non sa ancora cosa chiedere, la memoria che si posa
davanti alla porta vale turni interi. Banco vivente (tre corse GLM
vere): 3 chiavi → hit-rate 1.00 e bilancio **+81 byte**; 40 chiavi →
pareggio (−35); 200 chiavi a nomi-template → solo rumore. Dopo, il
dedup [P4] spegne il segnale: a contesto saturo il parcheggio si
riempie di resti e vale zero — **e il grafico della gobba è pubblicato
nel README, non nascosto**.

### 1.3 Il passamano orchestratore → subagent

Misura diretta del 22-11, stesso compito (review di `json.c`), due
figli gemelli: quello col piano **servito freddo inline** ha speso
0 round-trip di scoperta e ~2,9 KB di payload; quello lasciato
selvaggio ne ha spesi **13** (8 `chiedi` tutti a vuoto, 2 `intervallo`,
3 `get`) tirando dentro **~36,8 KB** — un rapporto di **12:1** a parità
di turni e di qualità del risultato. Nota onesta: il selvaggio, col
contesto più grasso, ha pure trovato il rosso che il freddo aveva
classificato giallo (`1e999` → token `inf` sul filo, poi verificato e
fissato col gate [M11]) — il traffico extra non è sporcizia, è
anatomia della scoperta. E il passamano **via parcheggio** è fallito:
le predizioni muoiono di TTL prima che il figlio arrivi alle chiavi; la
via che funziona è il servito freddo dall'orchestratore.

### 1.4 La lavagna: memoria condivisa fra agenti

Il gateway (cap. 14, protocollo [L1]..[L6]) rende il db leggibile a N
clienti con un solo padrone [I11] — l'architettura blackboard. Il
mercato la sta chiedendo ad alta voce (dev.to: "shared memory rooms
for multi-agent collaboration"; r/AI_Agents: il pattern che "nessuno ha
ancora risolto"); la risposta diffusa è lo stesso dei memory server:
stanze virtuali dietro un servizio. La nostra è geometria minima: un
file-porta, un filo JSON, un padrone che cade ad alta voce se il
secondo tenta di scrivere — corruzione silenziosa **impossibile per
contratto**, non per hardening.

### 1.5 Edge / embedded: l'`add()` senza LLM

Ogni memory server del piano A fa passare ogni messaggio da un LLM
(centinaia di ms e token **a ogni scrittura**). Il nostro `put` è O(1)
in microsecondi. Il costo sta in **domanda** (ogni interrogazione è
generazione di token lato chiamante), non in scrittura: per un agente
che scrive molto e interroga selettivamente — il profilo esatto
dell'agente di coding — è un ordine di grandezza, e un binario C11 + un
file entrano dove un container non entra.

### 1.6 Il substrato sotto chi fa estrazione

Se la logica è "LLM che digerisce conversazioni", quella logica ha
comunque bisogno di un posto deterministico dove mettere i fatti con
scadenza, confidenza e audit. È il confronto giusto dichiarato nel
README: con Redis/Qdrant come backend, non con Mem0/Zep come logica.

---

## 2. Quanto aiuta: la contabilità

Ogni riga di questa tabella è un numero misurato, con la corsa che l'ha
prodotto. Le percentuali di guadagno non esistono qui — solo differenze.

| il compito | il numero | la corsa |
|---|---|---|
| il giro è il costo | tool p99: get 3,4 µs, chiedi 24,8 µs, presente(64) 62,1 µs, intervallo 29,4 µs — contro 10²–10³ ms di una chiamata LLM | banco [MISURA], 2226 richieste |
| risposta che pesa in contesto | 149–2567 byte medie per tool (~37–641 token): il layer misura anche i byte, non solo i µs | [MISURA] |
| cold start 3 chiavi | hit-rate 1.00, bilancio **+81 byte** (risparmio scoperte − spesa handle) | banco vivente, corse vere |
| sessione satura (40 chiavi) | pareggio −35 byte: la gobba dichiarata, non promessa ovunque | idem |
| piani template (200 chiavi) | proposte di solo rumore: il limite del sintattico, sul README | idem |
| passamano freddo vs selvaggio | 0 vs 13 round-trip; 2,9 vs 36,8 KB: **12:1** | le due corse gemelle del 22-11 |
| la scadenza è gratis | 20.000 scadenze nello stesso tick in 0,1 ms **senza leggere una chiave** | banco timing wheel |
| il crash è già pagato | ogni put inchiostrata nel WAL, replay al riavvio ([W1]..[W6]) | banco cap. 13 |
| due padroni | il secondo cade ad alta voce (exit dichiarato, mai corruzione silenziosa) | gate [I11] |

E il rovescio della medaglia, ugualmente misurato (è parte del prodotto):

- `chiedi` con query brevi torna vuoto (8 su 8 nella corsa di misura):
  l'unione dei bigrammi uccide le domande corte — la scoperta senza
  proposta passa da `intervallo` per namespace;
- il passamano via parcheggio muore col TTL corto se il figlio è lento
  (4 morti, 0 hit, nel conto del server);
- i nomi delle chiavi sono parte del prodotto: nomi-template e frags
  ripetute catturano la similarità e il prefetch diventa rumore.

---

## 3. Quanto è originale

Il confronto per-feature contro il panorama di novembre 2026. "Unico"
vuole dire: non trovato in nessuno dei server dei cataloghi Glama e
Mnemoverse né nelle pagine ufficiali di Mem0/Zep/Letta/Dakera.

| la feature | chi l'ha | il nostro numero |
|---|---|---|
| **Prefetch speculativo dalla traccia di ragionamento** | nessuno (5.520 server a catalogo; il topic "che cosa dimenticare" ha riempito il 2026 di blog, il prefetch dei pensieri non esiste da nessuna parte) | proposta al turno 0, parcheggio fuori contesto, oblio incorporato |
| **Confidenza dentro ogni singola risposta** | nessuno la dichiara risposta per risposta (i benchmark LoCoMo/BEAM del piano A sono aggregati) | 1.0 l'esatto, Jaccard il vicino, silenzio il nulla; "forse" del bloom ≠ "sì" |
| **Audit come tool chiamabile** | nessuno espone `coerente=false` all'agente | gate [M7] + cose di banco in CI |
| **Oblio = morte contrattuale, non ranking** | il mercato converge lì (Hindsight: "nessun sistema copre bene tutte e 4 le leve"; tiered-TTL su ogni blog del 2026) ma per tutti decada = peso nell'indice; da noi è morte nel timing wheel | 20k scadenze/tick in 0,1 ms |
| **Un padrone per file, caduta ad alta voce** | nessun memory server dichiara il rischio dei due scrittori (è il buco che il post sul "consolidation problem" chiama *conflicting information without resolution*) | [I11], provato in CI con due processi veri |
| **Zero dipendenze, un binario** | il reference server ufficiale è un `npx`; i memory server vogliono Docker/OAuth | C11 + stdlib, un file |
| **La misura che smentisce pubblicata** | nessun competitor pubblica la corsa che dà torto alla propria feature | la gobba del prefetch, nel README |

Pareggi onesti: persistenza (SQLite l'ha da sempre), MCP standard
(tutti), local-first (la nuova ondata). Sopravvento altrui, dichiarato
nell'obitorio del repo: similarità **sintattica** non semantica (chi
cerca sinonimi deve stare sul piano A e pagare l'LLM a ogni `add`);
niente grafo temporale (Zep gioca una partita che non giochiamo);
niente multi-macchina oltre la lavagna; il filo pagina a 64 e il
cliente cieco oltre la prima pagina; percorsi per stringa, soglia 0.12
non adattiva.

---

## 4. Il confronto

|  | costo di un `add` | runtime richiesto | oblio | confidenza per risposta | audit visibile | prefetch | semantica | crash-safety |
|---|---|---|---|---|---|---|---|---|
| **Mem0** | LLM a ogni scrittura | hosted / Docker | ranking | no | no | no | sì | loro |
| **Zep/Graphiti** | LLM + grafo | servicio + OAuth | ranking temporale | no | no | no | sì | loro |
| **Letta** (ex MemGPT) | LLM a ogni scrittura | server Py | ranking/blocchi | no | no | no | sì | loro |
| **Dakera** | LLM a ogni scrittura | self-host o gestito | sei strategie di ranking | no | no | no | sì | loro |
| **mcp-memory-service** | consolidazione autonoma | Node + SQLite | ranking | no | no | no | ibrida | ereditata da SQLite |
| **reference ufficiale `memory`** | — (grafo in memoria) | `npx` | nessuno | no | no | no | sì | nessuna (ram) |
| **SQLite + MCP community** | zero | Node + file | solo manuale | no | no | no | FTS | SQLite |
| **Redis + MCP** | zero | server + porta | TTL nativo | no | no | no | moduli | AOF/RDB |
| **nostro** | O(1), µs, zero LLM | un binario C11 + un file | morte contrattale O(1) | sì, ogni risposta | sì, tool del filo | sì, dai pensieri | sintattica dichiarata | WAL, replay misurato |

Due righe di lettura: contro il **piano A** vinciamo dove il costo sta
nella scrittura (un `add` senza LLM) e dove il chiamante è una macchina
che sa retritare su una confidenza bassa; perdiamo dove serve semantica
vera — e lo dichiariamo in testa al tool ("la similarità è SINTATTICA,
NON semantica: capisce refusi, non sinonimi"). Contro **Redis**
facciamo ciò che Redis fa (TTL, µs, WAL) in un binario portabile e con
tre cose che Redis non ha: confidenza per risposta, audit come tool,
prefetch — ma Redis ha la replica, la scala, la multi-macchina: se il
tuo database deve avere SLA di servizio, non siamo il tuo piano.

---

## 5. Quando NON serve

- **Compiti piccoli**: sotto la gobba l'informazione passa intera nel
  prompt e la memoria non serve; sopra, il contesto satura e il
  prefetch muore. Il guadagno sta in mezzo: sessioni corte, dense di
  look-up, che muoiono prima che il dedup spenga il segnale (il profilo
  del subagente, misurato).
- **Se qualcuno ha già il dato in contesto**: l'hand-off diretto nel
  prompt batte memoria e prefetch entrambi. La memoria vince quando
  **nessuno** ce l'ha: risultati di un agente fratello in parallelo,
  resti di sessioni morte, ciò che è troppo grosso da girarsi in giro.
- **Semantica vera**: "il cliente ha parlato del rinnovo" trovato da
  "contratto in scadenza" — piano A, e si paga la scritture LLM.
- **Multi-macchina, SLA, replica**: non è il nostro piano, dichiarato
  nell'header del motore.

---

## 6. Il verdetto

Tre domande per decidere:

1. **Il tuo agente scrive molto e interroga selettivamente?** → il costo
   in domanda contro il costo in scrittura non è una sfida, è un ordine
   di grandezza (e in embedded un add-LLM è inaccettabile, punto).
2. **Ti serve che l'informazione muoia da sola, in modo verificabile?**
   → la morte contrattuale con audit visibile non esiste altrove; il
   resto del mercato offre invecchiamento del ranking.
3. **Vuoi che l'agente sappia quando non fidarsi?** → confidenza per
   risposta, silenzio invece di invenzione, il "forse" distinto dal
   "no": onestà come API, risposta per risposta.

Il prodotto non compete con i memory server: è il piano sotto. La
nicchia dichiarata — memoria embedded/edge, working memory
dell'agente di coding, lavagna piccola a un padrone — è stretta ed
è tutta nostra: nessuno dei 5.520 server a catalogo fa la nostra
parte, perché fanno un'altra: la logica. Noi il posto dove la logica
mette i fatti, con scadenza, confidenza, contabilità — e il dato
onesto anche quando smentisce l'idea di partenza.
