/*
 * prefetch.h — il prefetcher speculativo: la memoria che guarda i
 * pensieri. CAPITOLO 12.
 *
 * LA METAFORE: la CPU prefetcha le istruzioni guardando lo stream; il
 * database prefetcha guardando lo stream di ragionamento dell'agente.
 * Estrarre costa ~zero, ragionare costa caro: allora estraiamo piu'
 * di quanto serve, adesso — ma in parcheggio, NON in contesto.
 *
 * LA GEOMETRIA (la decisione aperta del capitolo, chiusa cosi'):
 * due processi, un file. L'hook pre_agent di Vibe e' il PRODUTTORE:
 * legge la traccia del ragionamento, predice col motore, scrive il
 * file di staging e inietta gli handle nel contesto dell'agente.
 * Il server MCP e' il CONSUMATORE: consuma il file, parcheggia le
 * predizioni, ne conta la morte, registra i colpi, espone la MISURA.
 * Il percorso dello staging e' DERIVATO dal percorso del db (regola
 * del repo: un derivato, una funzione di derivazione sola):
 *   <db>.prefetch
 *
 * =====================================================================
 * IL CONTRATTO [P1]..[P9] (invariants-first: scritto prima del codice)
 * =====================================================================
 *  [P1] IL SEGNALE E' LA TRACCIA, NON UN MODELLO. La predizione di
 *       cio' che servira' e' la similarita' tra le finestre della
 *       traccia recente (l'ultima finestra e' la piu' viva) e le
 *       chiavi del motore. Nessun modello nuovo: bigrammi NON
 *       posizionali, Jaccard, una soglia numerata.
 *       (POST MORTEM, scritto col sangue del primo tentativo: il
 *       prefetcher era nato su chiedi, una query per finestra — ma
 *       i grammi del motore sono POSIZIONALI, l'hash mescola la
 *       posizione del bigramma: chiedi e' un fuzzy-PREFIX, non una
 *       ricerca per tema. Prediceva solo chiavi gia' scritte
 *       verbatim nella traccia — cioe' quelle che il dedup [P4]
 *       scarta per costruzione. Sarebbe nato muto. La similarita'
 *       per tema vive nel layer: derivata dalle chiavi a ogni giro
 *       dell'hook, effimera, mai un secondo indice persistente.)
 *  [P2] UNA PREDIZIONE E' UNA CHIAVE COPIATA, MAI UN PUNTATORE
 *       ALL'ARENA. L'arena si muove alla prossima put; lo staging no.
 *       Ogni predizione porta: la chiave, la confidenza dichiarata,
 *       e il costo in byte della scoperta che eviterebbe (stimato
 *       alla nascita sui byte della risposta che l'agente avrebbe
 *       dovuto leggere per trovarsi qui: 43 + chiave + valore per
 *       item — il bilancio si chiude solo se il risparmio e'
 *       misurato quando nasce la spesa).
 *  [P3] IL FILE DI STAGING E' UN CONTRATTO TRA DUE PROCESSI. Si scrive
 *       atomico (tmp + rename), si consuma solo se intero (parse
 *       fallito = silenzio, fail-open: un prefetcher rotto non deve
 *       MAI fermare il turno). Una generazione (il turn_id) si
 *       consuma una volta sola.
 *  [P4] NON SI PREFETCHA CIO' CHE L'AGENTE HA GIA' IN VISTA. Una
 *       chiave gia' presente nella traccia e' dedup: il contesto e'
 *       la risorsa cara dell'LLM, riproporlo e' il bug economico.
 *  [P5] LE PREFETCHATE SONO "FORSE", NON "SI'". La confidenza viaggia
 *       nell'handle one-liner; il valore NON entra mai nel contesto
 *       (entra solo se l'agente tira, con get). Zero predizioni =
 *       zero handle, zero byte: il silenzio e' onesto anche qui.
 *  [P6] LA SPECULAZIONE SCADE DA SE'. La morte e' contata in
 *       RICHIESTE dal consumatore (l'orologio e' del chiamante
 *       [I8]: il layer non vede l'anello del motore, conta i giri;
 *       mai un orologio da muro). Lo sweep e' in batch alla
 *       consumazione: mai una spazzata per richiesta.
 *  [P7] IL COLPO E' UN GET. Hit = get con valore di una chiave
 *       parcheggiata e viva; conta UNA volta; la morte per TTL non
 *       cancella il colpo. Una predizione morta senza colpo e'
 *       inquinamento, e va contata come tale.
 *  [P8] IL BILANCIO E' IN BYTE. Spesa = handle iniettati in contesto;
 *       risparmio = costo di scoperta dei colpi (misurato alla
 *       nascita, [P2]). Il gate del capitolo e' numerato: hit-rate
 *       sotto soglia = la speculazione inquina piu' di quanto
 *       risparmia, e il dato si dichiara anche quando smentisce.
 *  [P9] IL PRODUTTORE VEDE L'ULTIMO SALVA, NON L'ARENA VIVA. L'hook
 *       legge lo snapshot PDB1; il consumatore vive nel server che
 *       possiede l'arena. La stalezza e' dichiarata e limitata dalla
 *       convenzione della memoria ("salva dopo scritture pesanti").
 *
 * =====================================================================
 * I LIMITI (dichiarati in testa, non scoperti in produzione)
 * =====================================================================
 *  - similarita' SINTATTICA (bigrammi non posizionali, casefold
 *    ASCII), non semantica: la traccia che dice "il capitolo
 *    dodici" non trova la chiave "capitolo-12" se non condividono
 *    bigrammi di parole. I sinonimi restano fuori: e' un limite
 *    dichiarato del metodo, non un difetto dell'implementazione;
 *  - il scan delle chiavi e' completo fino a 65536 chiavi vive:
 *    oltre, le eccedenti non partecipano alla predizione (una
 *    memoria di lavoro sta dentro; un magazzino no);
 *  - chiavi oltre 255 byte: scartate in predizione (le chiavi di
 *    una memoria di lavoro sono corte; il limite e' del parcheggio);
 *  - il journal si legge dalla fine, ultimo file, fino a 16 MB:
 *    una sessione piu' lunga di cosi' predice sulla coda recente —
 *    che e' esattamente la parte che serve [P1];
 *  - dirent.h: l'unica concessione fuori dalla stdlib C11 (per
 *    elencare il journal); c'e' su glibc e su MinGW.
 *  - il costo di scoperta [P2] e' STIMATO con una formula, non
 *    serializzato dal server: la formula vive qui, una fonte sola.
 */
#ifndef PREFETCH_H
#define PREFETCH_H

#include <stddef.h>
#include <stdint.h>

#include "motore.h"

/* la geometria della speculazione: numeri, non gusti */
#define PREFETCH_FINESTRA_PAROLE  5    /* token per finestra [P1]      */
#define PREFETCH_FINESTRE_MAX    64    /* cap: le ULTIME 64 finestre  */
#define PREFETCH_SOGLIA       0.12     /* sotto: rumore, non "forse"  */
#define PREFETCH_PREDIZIONI_MAX   4    /* handle per giro [P5]         */
#define PREFETCH_TRACCIA_MAX   8192    /* byte di traccia considerati   */
#define PREFETCH_TTL_RICHIESTE  32     /* vita in richieste [P6]        */
#define PREFETCH_PARCHEGGIO_MAX 32     /* slot del parcheggio           */
#define PREFETCH_CHIAVE_MAX    256     /* chiave copiata, limite [P2]   */
#define PREFETCH_CHIAVI_MAX  65536     /* tetto del scan delle chiavi   */
#define PREFETCH_GEN_MAX       128     /* il turn_id come generazione    */

/* una predizione: chiave COPIATA + il suo prezzo e il suo risparmio
 * [P2]. costo_scoperta = byte stimati della risposta che l'agente
 * avrebbe dovuto leggere per trovare questa chiave da solo. */
typedef struct {
    char     chiave[PREFETCH_CHIAVE_MAX];
    size_t   chiave_len;
    double   confidenza;      /* il "forse" dichiarato [P5]           */
    uint64_t costo_scoperta; /* il risparmio potenziale, alla nascita*/
} prefetch_predizione_t;

/* ------------------------------------------------------------- produttore */

/* predice dal motore sulla traccia: riempie pred[] (max max_pred) e
 * torna quante. 0 = silenzio onesto [P5]. La traccia va dal vecchio
 * al nuovo: la coda e' il pensiero piu' vivo [P1]. La confidenza e'
 * la similarita' della migliore finestra, NON la certezza: e' il
 * "forse" del filtro, come il bloom. */
size_t prefetch_predici(motore_t *m, const char *traccia, size_t tlen,
                        prefetch_predizione_t pred[], size_t max_pred);

/* il contratto su filo [P3]: JSON atomico, una generazione = un
 * turn_id. handle_byte = la spesa di contesto che l'hook sta per
 * iniettare [P8]. 0 = scritto, -1 = errore (l'hook tace e vive). */
int prefetch_scrivi_staging(const char *percorso, const char *gen,
                            const prefetch_predizione_t pred[],
                            size_t n, uint64_t handle_byte);

/* la traccia dal journal della sessione: cammina l'ULTIMO file
 * .jsonl dalla fine, raccoglie il ragionamento e il testo recenti
 * dell'assistente (dedup per testo identico), fino a cap byte.
 * torna i byte scritti (0 = journal muto: restera' solo il prompt). */
size_t prefetch_traccia_da_journal(const char *dir_sessione,
                                   char *traccia, size_t cap);

/* il percorso dello staging, derivato dal db: UNA regola sola */
void prefetch_percorso_staging(const char *db, char *out, size_t cap);

/* ------------------------------------------------------------- consumatore */

typedef struct prefetch_parcheggio prefetch_parcheggio_t;

prefetch_parcheggio_t *prefetch_parcheggio_apri(size_t capacita,
                                                uint32_t ttl);
void   prefetch_parcheggio_chiudi(prefetch_parcheggio_t *p);

/* consuma il file se la generazione e' nuova [P3]: spazza i morti
 * in batch [P6], parcheggia, conta la spesa [P8]. giro e' l'orologio
 * del CHIAMANTE (il contatore richieste del server). torna le voci
 * parcheggiate in QUESTA consumazione (0 = niente di nuovo). */
size_t prefetch_consuma(prefetch_parcheggio_t *p, const char *percorso,
                        uint64_t giro);

/* il colpo [P7]: 1 se la chiave e' parcheggiata e viva (e il colpo
 * viene contato una volta sola), 0 altrimenti. Non spazza mai. */
int    prefetch_tocca(prefetch_parcheggio_t *p, const char *chiave,
                      size_t klen, uint64_t giro);

/* la MISURA [P8]: tutti i numeri del capitolo, niente prosa */
void prefetch_stat(const prefetch_parcheggio_t *p, uint64_t giro,
                   uint64_t *parcheggiate, uint64_t *vive,
                   uint64_t *hit, uint64_t *morte,
                   uint64_t *spesa_handle, uint64_t *risparmio);

#endif /* PREFETCH_H */
