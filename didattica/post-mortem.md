# Il registro dei post mortem

*La storia che fino a fine novembre 2026 viveva nei messaggi di commit.*
*I commit sono ora inglesi e compatti — messa in servizio, non quaderno.*
*Il quaderno resta qui: ogni bug, dove viveva, la lezione che ci ha lasciato.*
*I riferimenti `sha` puntano al commit che ha riparato o dichiarato il fatto.*

---

## L'era dei capitoli (didattica/)

**Il sentinel disuguagliato** — `379eafb` (capitolo 3). Il confronto del
sentinel tombstone (0xFFFFFFFF) con `<=` era sempre vero e azzerava
l'indice a ogni raddoppio della tabella. Lo ha pescato l'autotest messo
in corsa fiducioso: la ricerca oltre i tombstone restituiva catene vuote.
Lezione: l'autotest è un cacciatore solo se la condizione che verifica
puo' realmente fallire.

**Il CRC invalidato dal mark-morto** — `9a41074` (capitolo 4). Marcare
un'entry come morta mutava il campo creazione coperto dal CRC:
l'entry cancellata risultava corrotta sul disco, e il load si fermava
prima del tempo. Fix alla radice: `db_salva` compatta prima di scrivere
— il file contiene solo vive e univoche, il load resta un replay senza
ambiguita'. Lezione: quando il derivato e' la verita' su disco, il
marking in-place avvelena la fotografia.

**Invariants-first, il giro invertito** — capitolo 9. Prima del
contratto scritto prima del codice, il ciclo era *code → bug → post
mortem* per tre capitoli di fila. Con il contratto a tavolino: 8 trappole
prese in disegno, 1 al primo run, e lo sfuggito ha detto ESATTAMENTE
quale asserzione mancava. Nello stesso capitolo, due lezioni gemelle
incise nei gate: **stampare non e' bloccare** (un test che stampa senza
far fallire il processo non e' un test) e **il confronto senza soglia
non e' un gate** ("0 == 0, verde!" contro 470/500).

---

## La CI come cacciatrice

**Il ramo mai compilato** — `67acc9b`. Quattro capitoli usavano
`struct timespec` e `clock_gettime` senza includere `<time.h>`: su
MinGW compilavano perche' il ramo `_WIN32` tira dentro `windows.h`,
su Linux no. E con `-std=c11` stretto la glibc non dichiara nemmeno
`clock_gettime` senza `_POSIX_C_SOURCE`. Lezione: il codice che una
piattaforma non compila mai e' il codice che l'altra non perdona.

**pow e la libm** — `90f2a69`. Il primo run della CI si e' fermato al
link: `pow` undefined. Su MinGW la libm e' piegata nel runtime e `-lm`
non serve; su Linux vive in libm. Locale tutto verde, CI rossa al link —
per questo il banco gira in CI, non in locale.

**La workflow muta** — `f324125`. Un due-punti non quotato dentro uno
`name:` e per YAML il valore diventava una mappatura: sintassi rotta,
NESSUN run partito, NESSUN log esistente. La sanzione di una workflow
invalida e' il silenzio, non il rosso. Regola incisa in AGENTS.md: ogni
workflow si valida col parser prima del push.

**L'ld muto di MinGW** — `8726da8`, `cf3d7a7`. Il collaudo ASan del
layer linkava senza `prefetch.c`; quello del prefetcher senza `wal.c` —
terza volta lo stesso tipo di buco. Su MinGW il `collect2` ingoia gli
undefined reference (stampa solo `ld returned 1`): la CI li ha gridati,
il locale li aveva sussurrati. Stessa famiglia: `off_t` vuole
`sys/types.h` (`a6bd3ef`), `rmdir` vuole `unistd.h` (`3608563`).

**Il cliente che si sparava da solo** — `8bdcbf0`. Il passo CI
"lavagna vera" falliva, ma non per la lavagna: il cliente di bash
catturava la risposta con `R=$(...; timeout 3 cat <&3)`, il timeout
ammazzava il cat a lavoro finito, la subshell usciva 124 — e GitHub
Actions gira gli step con `bash -e`: lo step moriva con la risposta
gia' in mano. Sotto `set -e` una command substitution in assegnamento
E' una sentinella: la morte di vecchiaia del cliente va assolta
esplicitamente (`|| true`), la sentinella vera resta il gate.

**Il calendario sotto sanitizer** — `3a316bd`, `cd9f522`. Le soglie del
[MISURA] giravano rosse sotto ASan senza che il codice fosse cambiato:
sigma misurata da 2246 a 15220 µs sullo STESSO binario. Sentenza
(gia' scritta per [G8]): il calendario non si misura sotto sanitizer —
le soglie vivono nel passo "banco nudo" che gira a ogni push; il
[silenzio] e' dichiarato, non nascosto.

---

## Il motore e il filo

**Il post mortem di 40 anni: Nagle + delayed-ACK** — `74dfeab`. Il send
da 1 byte del `\n` restava nello stomaco del kernel fino a 40 ms: il
banco si sarebbe appeso in CI, e su Windows un timing benigno lo
nascondeva. Cura: `TCP_NODELAY` su socket accettati e cliente (la
stessa pillola di Redis) e attese con scadenza nel banco.

**fsync senza fflush: i 70 µs disonesti** — `e07e338` / `misura_fsync`.
La prima misura della durabilita' diceva 70 µs per put; quella onesta
216: fsync senza fflush sincronizza un kernel che non ha ancora i byte
(stanno nel buffer del processo). La politica di durabilita' e' chiusa
dal numero onesto, non dal gusto — e il numero dichiara anche il costo:
236x contro la put nuda, visibile, non nascosto.

**Il fopen("wb") che bruciava mezzo file** — `def0a2c`, [I12]. Un crash
a meta' `salva` lasciava un file troncato che il CRC dichiarava morto
alla prima entry strappata: la finestra era teorica finche' il padrone
era uno solo, ed e' diventata urgente col gateway. Il salva ora scrive
su `<nome>.tmp` e rinomina: il rename e' atomico, la finestra e' chiusa.

**Il due padroni** — `a75257b`, [I11]. Due processi sullo stesso db non
erano "non supportati": erano corruzione silenziosa, due WAL che si
scrivono addosso senza che nessuno se ne accorga. Il flock (e
LockFile su Windows) trasforma il bug muto in errore ad alta voce col
cartellino del pid di chi tiene la porta.

**Il bilancio che girava su 2^64** — `ce75666`. `risparmio - spesa`
tra due uint64_t: senza colpi (risparmio 0, spesa 108) la sottrazione
faceva il giro e il tool dichiarava 1.8e19 invece di -108. L'ha
prodotto per primo la SESSIONE VERA, non il banco: il gate esercitava
solo il caso del colpo, mai il caso "predizioni parcheggiate e mai
chieste" — che e' poi il primo turno di ogni sessione. Fix col cast
signed e soglia ESATTA: bilancio == -90, non "negativo e basta".

**NaN alle porte** — `53efa3a`, [M10]. Il NaN passava i range check
(`NaN < 0` e' falso) e moriva nel cast, in silenzio; le frazioni si
truncavano senza dichiararlo. Disciplina replicata su tutti i tool che
leggonumeri dal filo: i numeri sporchi si rifiutano col codice
-32602, non si troncano.

**1e999: il non-finito sul filo** — `d00149f`, [M11]. `1e999` e'
grammatica RFC 8259 valida, strtod lo fa inf, e `jbuf_num` lo metteva
in filo come token `inf` — non-JSON: il round-trip parse→serialize
corrompeva il contratto dell'unica fonte di verita'. Il rosso e' arrivato
dalla corsa di misura dei subagent (il revisore SELVAGGIO, quello senza
contesto servito: il traffico extra era anatomia della scoperta, non
sporcizia). Fix: `isfinite` in jbuf_num — il filo resta JSON o niente,
e la LIBRERIA non dipende dalla cortesia dei chiamanti.

**Un fallimento ne avvelenava un altro** — `cd9f522`. Il setup del gate
[P6] non ripuliva i residui della corsa prima: una demo caduta lasciava
`banco12.db.wal` a terra e la corsa dopo lo riapplicava all'apertura.
Pulizia pre-mount (db, wal, lock, staging) e `.wal` rimosso a fine
corsa: il banco parte sempre da terreno vergine.

---

## I giochi dell'agente (il meta-livello)

**I caratteri fantasma** — `89d67e9`. Sei righe scritte in fretta
portavano cirillico e CJK dentro commenti italiani. Ne e' nato il gate
caratteri di AGENTS.md — e siccome una blacklist e' sempre in ritardo
sull'alfabeto successivo (per tre volte, fino a un ideogramma dentro un
MESSAGGIO di commit), il gate e' una WHITELIST. Il gate passa oggi
anche ai messaggi: il quaderno prima di firmarlo.

**La tasca dell'add -A** — `0d54b67` → riscrittura `d00149f`. Un
`git add -A` di troppo ha trascinato su GitHub il file che l'umano tiene
in locale per scelta. Rimediato con la riscrittura della punta ad arte
dell'umano (force-push) e il .gitignore che blinda la tasca per sempre.
Lezione doppia: (1) mai `git add -A` in una cartella con file volutamente
non tracciati; (2) la lista esplicita costa dieci secondi e salva la
storia.

**L'ombra ELF e il nome unico di MSYS** — `e5bd0c2`. Le build di
verifica da WSL lasciavano nella cartella binari ELF senza estensione:
bash uccideva l'hook col `Exec format error` — fail-open onesto, ma il
prefetch muto. E per Git Bash/MSYS `nome` e `nome.exe` sono LO STESSO
FILE in redirezione: un `> nome` di test ha sovrascritto l'exe vero.
Su MSYS non si crea MAI un file senza estensione accanto al suo .exe.

**La chirurgia sui fine-riga** — `d025eac`. La sostituzione
dell'header del racconto e' fallita al primo giro perche' i `.md` del
repo viaggiano a CRLF e il pattern viaggiava a LF. Rifatta
`\r?\n`-agnostica. Lezione: i pattern di testo su file altrui non
assumono mai il fine-riga.

---

*Fine del registro fino al 23-11-2026. Le voci nuove entrano in italiano
qua; il commit che ripara il bug lo dichiara in inglese in una riga.*
