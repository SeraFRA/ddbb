/*
 * mcp_server.c — il layer MCP: il motore come strumento. CAPITOLO 11.
 *
 * =====================================================================
 * LA METAFORE E LA MECCANICA
 * =====================================================================
 * "L'agente non deve conoscere il motore, deve poterlo chiamare — e
 * ogni risposta deve dichiarare quanto vale." Un server MCP (Model
 * Context Protocol) sopra il motore: l'agente genera le chiamate
 * (costo: token e latenza LLM), il motore esegue in microsecondi
 * (costo: ~zero). L'economia del layer detta TUTTO il design:
 *
 * =====================================================================
 * IL CONTRATTO [M1]..[M9] (invariants-first: scritto prima del codice)
 * =====================================================================
 *  [M1] IL FILO E' UN CONTRATTO. Spec MCP 2025-06-18 pinnata nella
 *       risposta initialize; JSON-RPC 2.0, un oggetto per riga su
 *       stdio. Cambiare spec = un server che parla entrambe, come il
 *       disco [I10]. Metodi accettati: initialize, ping, tools/list,
 *       tools/call; le notifiche sono accettate e tacciono.
 *  [M2] MENO ROUND-TRIP POSSIBILE. Ogni tool restituisce tutto cio'
 *       che la chiamata giustifica: chiedi torna chiave E valore E
 *       confidenza (mai solo le chiavi: la seconda chiamata per avere
 *       i valori e' il bug economico); intervallo torna il batch PIU'
 *       il totale oltre il cap; presente e' BATCH (una chiamata, N
 *       verdetti). Ogni risposta e' compatta per costruzione: il
 *       costo di una risposta si paga in contesto LLM.
 *  [M3] LA CONFIDENZA VIAGGIA SEMPRE. Ogni risposta di get/chiedi/
 *       intervallo porta la confidenza dichiarata (1.0 esatta,
 *       Jaccard altrimenti, 3 decimali). Un tool che risponde senza
 *       confidenza mente per omissione: l'agente non saprebbe quando
 *       ritentare con un'altra formulazione.
 *  [M4] IL SILENZIO E' ONESTO. get senza chiave -> null (NON e' un
 *       errore); chiedi senza candidati -> array vuoto. Mai inventare
 *       una risposta per fare contento il chiamante.
 *  [M5] PRESENTE E' IL PREFILTRO ECONOMICO. Batch: max 256 chiavi per
 *       chiamata. 0 = certamente no [I9] (mai falsi negativi sulle
 *       vive), 1 = probabilmente (le cancellate restano "1" fino
 *       alla compattazione: e' un filtro, non un indice — dichiarato
 *       nella description, perche' e' la parte che l'LLM legge).
 *  [M6] L'OROLOGIO E' DEL CHIAMANTE [I8]. Il server NON legge mai
 *       l'orologio da muro: avanza e' un tool, il tempo si muove solo
 *       se l'agente lo muove. Senza questo il banco di prova non
 *       sarebbe deterministico e le scadenze non sarebbero testabili.
 *  [M7] AUDIT E' UN TOOL. Il contratto rotto e' visibile anche
 *       all'agente: audit torna vive, dichiarate e "coerente". Un
 *       agente puo' non fidarsi dei dati e verificare.
 *  [M8] I LIMITI SONO LA DESCRIZIONE. La similarita' e' SINTATTICA
 *       (bigrammi posizionali + MinHash), non semantica: capisce i
 *       refusi, NON i sinonimi. Ogni tool dichiara i suoi limiti
 *       nella description — e' la regola "i limiti si dichiarano in
 *       testa al file", tradotta nel canale che l'LLM legge davvero.
 *  [M9] NUCLEO PURO, GUSCIO SOTTILE. mcp_gestisci e' l'unica fonte
 *       di verita' del filo; il main stdio e' un adattatore. Il
 *       banco di prova chiama il nucleo in process: i gate non
 *       parlano con pipe.
 *  [I11] IL DB HA UN SOLO PADRONE, E LO DICE. (Numerato nella serie
 *       del motore perche' e' geometria dei processi sul db, ma il
 *       codice vive qui: il motore non conosce i file altrui, il
 *       padrone li conosce tutti e tre — db, WAL, staging.) Aprire
 *       un db e' prendersi il file: due padroni vivi sono corruzione
 *       SILENTE, due WAL che si scrivono addosso senza che nessun
 *       CRC se ne accorga (capitolo 13). mcp_apri chiude quindi il
 *       lucchetto su <db>.lock PRIMA di toccare il db, e lo tiene
 *       fino a mcp_chiudi: chi arriva secondo riceve NULL e una
 *       riga che dice chi tiene la porta (il pid sul cartellino).
 *       il lucchetto NON sta sul db stesso: su Windows un LockFile
 *       li' negherebbe la lettura all'hook pre_agent, che e' un
 *       lettore silenzioso per contratto [P3] — un file a parte
 *       esclude i padroni, MAI i lettori. E dentro il file stesso
 *       vale la stessa lezione: la zona chiusa e' solo la testa (16
 *       byte), il cartellino col pid sta dietro (offset 64), cosi'
 *       il secondo padrone legge chi tiene la porta MENTRE la tiene.
 *       L'OS rilascia il lucchetto alla morte del processo: il crash
 *       non lascia serrature eterne. LIMITI DICHIARATI: il
 *       riconoscimento dentro un PROCESSO e' per STRINGA di percorso
 *       (la stessa verrucca di tool_salva: la normalizzazione dei
 *       path sara' una funzione sola, un giorno); il pid del
 *       cartellino e' diagnostica onesta, non verita' — i pid si
 *       riciclano, e c'e' una finestra di microsecondi tra
 *       lucchetto e penna in cui il cartellino nomina il padrone
 *       che sta per arrivare, non quello che tiene. E il cartellino
 *       si scrive SEMPRE col medesimo fd del lucchetto: la prima
 *       versione lo scriveva con una fopen sepolta, e la fclose
 *       rilasciava il lock appena preso — perche' POSIX ammazza
 *       TUTTI i lock fcntl del processo su un file quando chiude UN
 *       suo fd qualunque di quel file. Trappola invisibile su
 *       Windows (LockFileEx e' per-handle) e mascherata in-process
 *       dal registro: l'ha catturata solo il banco della CI con due
 *       processi VERI (il /proc/locks senza il nostro lock).
 *
 * =====================================================================
 * OBITUARIO: COSA NON E' UN TOOL, E PERCHE'
 * =====================================================================
 *  - COMPAATTA: e' un'ottimizzazione interna del motore, non un
 *    verbo dell'agente. Nessun compito dell'agente si esprime come
 *    "compatta": e' rumore di superficie per l'LLM.
 *  - STATISTICHE: printf diagnostico del motore, non dati. Se un
 *    domani servisse, entra come tool SOLO con numeri, mai come
 *    testo da interpretare.
 *  - STREAM/HTTP: il canale e' stdio. Un secondo trasporto e' un
 *    secondo contratto [M1]: entra solo se un chiamante lo paga.
 *  - PUT/GET DI MASSA: il carico iniziale costa N round-trip — e'
 *    il costo onesto dichiarato del modello (la scrittura passa
 *    dall'LLM); un tool "carica array" nasconderebbe il costo, non
 *    lo abolirebbe.
 * =====================================================================
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wpedantic"
# include <windows.h>
# pragma GCC diagnostic pop
#else
# include <fcntl.h>
# include <unistd.h>
# include <sys/types.h>
#endif

#include "motore.h"
#include "mcp_server.h"
#include "json.h"
#include "prefetch.h"
#include "wal.h"

#define MCP_SPEC        "2025-06-18"
#define SERVER_NOME     "motore-pdb"
#define SERVER_VERSIONE "1.0"

#define CHIEDI_K_MAX        8   /* il TOPLIM del motore: dichiarato   */
#define INTERVALLO_K_MAX   64  /* il cap del batch: token, non RAM   */
#define PRESENTE_BATCH_MAX  256
#define TTL_FINESTRA       4096 /* N_TICK del motore: oltre, rifiuto */

struct mcp {
    motore_t *mo;
    uint64_t  richieste;
    uint64_t  byte_payload;
    /* il parcheggio del prefetcher (capitolo 12): vive dove vive
     * il motore, consuma lo staging derivato dal db [P3][P6] */
    prefetch_parcheggio_t *pf;
    char      staging[512];
    /* il write-ahead log (capitolo 13): la durabilita' fra due salva.
     * Vive accanto al db (<db>.wal), e si taglia quando lo snapshot
     * torna SULLO STESSO file [W5] */
    wal_t    *wal;
    char      percorso_db[512];
    /* il lucchetto del padrone [I11]: il manico dell'OS dietro
     * <db>.lock. NULL / -1 = db in memoria (o gia' restituito) */
#ifdef _WIN32
    void     *lucchetto;
#else
    int       lucchetto;
#endif
};

/* ----------------------------------------------- il padrone del db [I11] */

/* il registro dei db che QUESTO processo possiede: fcntl esclude i
 * processi, non i doppi open interni (POSIX: un processo non si
 * blocca mai da solo); il registro chiude anche il caso in-process,
 * che e' poi quello del banco di prova. Il confronto e' per STRINGA
 * di percorso — la verrucca dichiarata, un giorno una funzione di
 * normalizzazione sola la curera' (la stessa di tool_salva). */
#define PADRONI_MAX 16
static char padroni[PADRONI_MAX][512];
static int  padroni_vivi[PADRONI_MAX];

static unsigned long pid_nostro(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

/* il secondo arriva e la porta e' chiusa: gli si dice chi la tiene.
 * Il pid sta nel cartellino ALL'OFFSET 64: la zona chiusa dal
 * lucchetto e' solo la testa del file (i primi 16 byte), cosi' il
 * secondo padrone PUO' leggere chi tiene la porta anche mentre la
 * tiene — la lezione dell'hook [P3] vale anche qui. E' diagnostica
 * onesta, non verita' [I11]: finestra di microsecondi tra lucchetto
 * e penna, e i pid si riciclano */
#define CARTELLINO_OFFSET 64

static void custode_annuncia_occupato(const char *percorso_lock,
                                      const char *percorso_db)
{
    char pid[32] = "?";
    FILE *c = fopen(percorso_lock, "rb");
    if (c) {
        if (fseek(c, CARTELLINO_OFFSET, SEEK_SET) == 0 && fgets(pid, sizeof pid, c)) {
            size_t l = strlen(pid);
            while (l && (pid[l - 1] == '\n' || pid[l - 1] == '\r'))
                pid[--l] = '\0';
            if (l == 0)
                snprintf(pid, sizeof pid, "?");
        } else {
            snprintf(pid, sizeof pid, "?");
        }
        fclose(c);
    }
    fprintf(stderr, "[I11] \"%s\" e' gia' posseduto (cartellino: pid %s):"
                    " un db, un solo padrone — chiudi l'altro server o"
                    " apri un altro file\n", percorso_db, pid);
}

/* prende il lucchetto: 0 = padrone, -1 = la porta era chiusa (e la
 * riga l'ha gia' detto ad alta voce). Al SO tocca il resto: chi muore
 * rilascia da solo, il crash incluso [I11] */
static int custode_prendi(mcp_t *s, const char *percorso_db)
{
    if (!s || !percorso_db)
        return -1;
    if (strlen(percorso_db) >= sizeof s->percorso_db) {
        fprintf(stderr, "[I11] percorso del db oltre %zu byte\n",
                sizeof s->percorso_db);
        return -1;
    }
    int buco = -1;
    for (int i = 0; i < PADRONI_MAX; i++) {
        if (padroni_vivi[i]) {
            if (strcmp(padroni[i], percorso_db) == 0) {
                fprintf(stderr, "[I11] \"%s\" e' gia' posseduto da QUESTO"
                                " processo: un db, un solo padrone\n",
                        percorso_db);
                return -1;
            }
        } else if (buco < 0) {
            buco = i;
        }
    }
    if (buco < 0) {
        fprintf(stderr, "[I11] questo processo possiede gia' %d db:"
                        " il tetto del registro\n", PADRONI_MAX);
        return -1;
    }
    char percorso_lock[512 + 8];
    if ((size_t)snprintf(percorso_lock, sizeof percorso_lock,
                         "%s.lock", percorso_db) >= sizeof percorso_lock) {
        fprintf(stderr, "[I11] percorso del lucchetto troppo lungo\n");
        return -1;
    }
#ifdef _WIN32
    HANDLE h = CreateFileA(percorso_lock, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[I11] il lucchetto \"%s\" non si crea/apro:"
                        " niente proprieta', niente server\n",
                percorso_lock);
        return -1;
    }
    /* si chiude SOLO la testa del file (16 byte): il cartellino a
     * offset 64 resta leggibile al secondo che arrivera' */
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 16, 0, &ov)) {
        CloseHandle(h);
        custode_annuncia_occupato(percorso_lock, percorso_db);
        return -1;
    }
    /* il cartellino, col MEDESIMO manico del lucchetto: qui un
     * secondo handle aperto/chiuso non libererebbe nulla (LockFileEx
     * e' per-handle), ma il cammino unico evita di trascinare
     * l'analoga trappola POSIX di sotto */
    {
        char pid_buf[32];
        int n = snprintf(pid_buf, sizeof pid_buf, "%lu\n", pid_nostro());
        LARGE_INTEGER pos;
        DWORD scritti = 0;
        pos.QuadPart = CARTELLINO_OFFSET;
        if (SetFilePointerEx(h, pos, NULL, FILE_BEGIN))
            WriteFile(h, pid_buf, (DWORD)n, &scritti, NULL);
    }
    s->lucchetto = (void *)h;
#else
    int fd = open(percorso_lock, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fprintf(stderr, "[I11] il lucchetto \"%s\" non si crea/apro:"
                        " niente proprieta', niente server\n",
                percorso_lock);
        return -1;
    }
    /* fcntl, non flock: e' POSIX del 1990, dichiarato con
     * -std=c11 + _POSIX_C_SOURCE, e i conflitti sono per processo —
     * il registro qui sopra copre il caso dentro questo processo.
     * TRAPPOLA (catturata dal banco due-processi in CI): i lock
     * fcntl muoiono quando il processo chiude UN fd qualunque dello
     * stesso file — quindi dopo di qui il cartellino va scritto
     * col MEDESIMO fd, mai con una fopen/fclose che chiuderebbe
     * la porta appena chiusa */
    struct flock lk;
    memset(&lk, 0, sizeof lk);
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 16;                      /* la testa: il resto si legge */
    if (fcntl(fd, F_SETLK, &lk) == -1) {
        close(fd);
        custode_annuncia_occupato(percorso_lock, percorso_db);
        return -1;
    }
    {
        char pid_buf[32];
        int n = snprintf(pid_buf, sizeof pid_buf, "%lu\n", pid_nostro());
        if (lseek(fd, CARTELLINO_OFFSET, SEEK_SET) != (off_t)-1 &&
                write(fd, pid_buf, (size_t)n) < 0) {
            /* muto di proposito: il cartellino e' diagnostica, il
             * lucchetto e' il contratto */
        }
    }
    s->lucchetto = fd;
#endif
    snprintf(padroni[buco], sizeof padroni[buco], "%s", percorso_db);
    padroni_vivi[buco] = 1;
    return 0;
}

/* restituisce il lucchetto: OS e registro nello stesso gesto, perche'
 * la proprieta' muore atomica o non muore [I11] */
static void custode_lascia(mcp_t *s)
{
    if (!s)
        return;
#ifdef _WIN32
    if (s->lucchetto) {
        CloseHandle((HANDLE)s->lucchetto);
        s->lucchetto = NULL;
    }
#else
    if (s->lucchetto >= 0) {
        close(s->lucchetto);
        s->lucchetto = -1;
    }
#endif
    if (s->percorso_db[0]) {
        for (int i = 0; i < PADRONI_MAX; i++) {
            if (padroni_vivi[i] &&
                    strcmp(padroni[i], s->percorso_db) == 0) {
                padroni_vivi[i] = 0;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------- utilita' */

mcp_t *mcp_apri(const char *percorso_db, uint32_t adesso, size_t *scartate)
{
    if (scartate)
        *scartate = 0;
    mcp_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
#ifndef _WIN32
    s->lucchetto = -1;    /* calloc porta 0: un fd valido no [I11] */
#endif
    if (percorso_db) {
        /* il lucchetto viene PRIMA di toccare il file [I11]: e' la
         * prima cosa che chi vuole diventare padrone deve vincere */
        if (custode_prendi(s, percorso_db) != 0) {
            free(s);
            return NULL;
        }
        snprintf(s->percorso_db, sizeof s->percorso_db, "%s", percorso_db);
        s->mo = motore_apri_da(percorso_db, adesso, scartate);
        if (!s->mo) {
            custode_lascia(s);
            free(s);
            return NULL;
        }
        /* il WAL vive accanto al db (capitolo 13): prima si riavvolge
         * il log, poi si serve. Fail-closed: se il segmento non si puo'
         * aprire non si promette durabilita' che non si ha */
        char percorso_wal[512 + 8];
        snprintf(percorso_wal, sizeof percorso_wal, "%s.wal", percorso_db);
        s->wal = wal_apri(percorso_wal);
        if (!s->wal) {
            custode_lascia(s);
            motore_chiudi(s->mo);
            free(s);
            return NULL;
        }
        size_t scarti_wal = 0;
        size_t applicati = wal_riavvolgi(s->wal, s->mo, &scarti_wal);
        if (applicati == (size_t)-1) {
            fprintf(stderr, "[mcp] WAL illeggibile: non parto senza "
                            "durabilita'\n");
            mcp_chiudi(s);
            return NULL;
        }
        if (applicati > 0 || scarti_wal > 0)
            fprintf(stderr, "[mcp] WAL: %zu record riapplicati, %zu byte "
                            "di coda non valida troncati [W3]\n",
                    applicati, scarti_wal);
        /* lo staging e' un derivato del db: una regola sola [P3] */
        prefetch_percorso_staging(percorso_db, s->staging,
                                  sizeof s->staging);
        s->pf = prefetch_parcheggio_apri(PREFETCH_PARCHEGGIO_MAX,
                                         PREFETCH_TTL_RICHIESTE);
        if (!s->pf)
            /* fail-open e' il contratto, il silenzio no (gialli della
             * review): chi apre il db merita di sapere che le proposte
             * non ci sono, e perche' */
            fprintf(stderr, "[mcp] parcheggio del prefetch assente"
                            " (memoria?): le proposte tacciono, il"
                            " il resto serve — fall-open dichiarato,"
                            " non taciuto\n");
    } else {
        s->mo = motore_apri();
        if (!s->mo) {
            free(s);
            return NULL;
        }
        motore_avanza(s->mo, adesso);
    }
    return s;
}

void mcp_chiudi(mcp_t *s)
{
    if (!s)
        return;
    custode_lascia(s);          /* il padrone restituisce prima di andarsene [I11] */
    wal_chiudi(s->wal);
    prefetch_parcheggio_chiudi(s->pf);
    motore_chiudi(s->mo);
    free(s);
}

void mcp_contatori(const mcp_t *s, uint64_t *richieste, uint64_t *byte_payload)
{
    if (richieste)
        *richieste = s ? s->richieste : 0;
    if (byte_payload)
        *byte_payload = s ? s->byte_payload : 0;
}

/* ------------------------------------------------- i tool (il vocabolario) */

static const char *msg_motore(int codice)
{
    switch (codice) {
    case MOTORE_ERR_MEMORIA: return "memoria esaurita";
    case MOTORE_ERR_PIENO:   return "arena al tetto di sicurezza";
    case MOTORE_ERR_MANCA:   return "chiave assente";
    case MOTORE_ERR_IO:      return "errore di I/O";
    case MOTORE_ERR_FORMATO: return "formato del file non valido";
    case MOTORE_ERR_TTL:     return "ttl oltre la finestra dell'anello (4096)";
    default:                 return "errore del motore";
    }
}

/* il payload di errore: codice + parola, niente prosa [M2] */
static void payload_errore(jbuf_t *out, int codice, const char *msg)
{
    JBUF_LIT(out, "{\"errore\":\"");
    jbuf_testo(out, msg, strlen(msg));
    JBUF_LIT(out, "\",\"codice\":");
    jbuf_num(out, (double)codice);
    JBUF_LIT(out, "}");
}

static const char *arg_str(const jval_t *arg, const char *nome, size_t *len)
{
    return json_str(json_get(arg, nome), len);
}

/* un item {chiave, valore, confidenza} [M2][M3] */
static void scrivi_item(jbuf_t *out, const motore_risposta_t *r)
{
    char conf[32];
    snprintf(conf, sizeof conf, "%.3f", r->confidenza);
    JBUF_LIT(out, "{\"chiave\":\"");
    jbuf_testo(out, (const char *)r->chiave, r->chiave_len);
    JBUF_LIT(out, "\",\"valore\":\"");
    jbuf_testo(out, (const char *)r->valore, r->valore_len);
    JBUF_LIT(out, "\",\"confidenza\":");
    jbuf_add(out, conf, strlen(conf));
    JBUF_LIT(out, "}");
}

static int tool_put(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    size_t kl = 0, vl = 0;
    const char *chiave = arg_str(arg, "chiave", &kl);
    const char *valore = arg_str(arg, "valore", &vl);
    if (!chiave || !valore) {
        payload_errore(out, -32602, "servono chiave e valore (stringhe)");
        return 1;
    }
    int valido = 0;
    double ttl = json_num(json_get(arg, "ttl"), &valido);
    if (!valido)
        ttl = 0;
    /* la disciplina dell'avanza, replicata (gialli della review, 22-11):
     * NaN supera le due disuguaglianze di range — lo cattura il
     * !(...&&...) negato; e le frazioni si buttano prima del cast,
     * perche' (uint32_t)NaN e' UB puro e il trunc silenzioso e' una
     * bugia gentile. */
    if (!(ttl >= 0 && ttl <= 4294967295.0) || ttl != (uint32_t)ttl) {
        payload_errore(out, -32602,
                       "ttl fuori range (intero 0..4294967295)");
        return 1;
    }
    /* la finestra dell'anello si verifica PRIMA dell'inchiostro: il WAL
     * non deve mai custodire un'intenzione che il motore rifiuterebbe */
    if (ttl > TTL_FINESTRA) {
        payload_errore(out, MOTORE_ERR_TTL, msg_motore(MOTORE_ERR_TTL));
        return 1;
    }
    /* il log viene PRIMA del motore [W1]: un crash fra i due riapplica,
     * il contrario perderebbe la put accettata */
    if (s->wal && wal_put(s->wal, chiave, kl, valore, vl,
                          (uint32_t)ttl, motore_adesso(s->mo)) != WAL_OK) {
        payload_errore(out, MOTORE_ERR_IO,
                        "WAL non scrivibile: put rifiutata (durabilita' o niente)");
        return 1;
    }
    int r = motore_put(s->mo, chiave, kl, valore, vl, (uint32_t)ttl);
    if (r != MOTORE_OK) {
        /* POST MORTEM (review, giro del 22-11): il record restava nel
         * WAL nonostante il motore rifiutasse, e al replay la put
         * risorgeva: stato dichiarato != stato durabile. Compensazione:
         * l'intenzione si ritira dal log (una del dopo la cancella).
         * Limite dichiarato: se la chiave era VIVA col vecchio valore,
         * la compensazione la seppellisce — nel mondo dell'OOM in cui
         * tutto questo vive, il veleno nel log sarebbe peggio. Se la
         * compensazione stessa cade, il WAL e' gia' rotto per conto
         * suo (wal_rollback) e ogni prossima put si rifiuta ad alta
         * voce: mai piu' veleno silenzioso. */
        if (s->wal &&
                wal_del(s->wal, chiave, kl, motore_adesso(s->mo)) != WAL_OK)
            payload_errore(out, MOTORE_ERR_IO,
                           "put rifiutata E la compensazione caduta:"
                           " il WAL e' compromesso, da qui ogni"
                           " scrittura si rifiuta");
        else
            payload_errore(out, r, msg_motore(r));
        return 1;
    }
    JBUF_LIT(out, "{\"scritto\":true,\"ttl\":");
    jbuf_num(out, ttl);
    JBUF_LIT(out, "}");
    return 0;
}

static int tool_get(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    size_t kl = 0;
    const char *chiave = arg_str(arg, "chiave", &kl);
    if (!chiave) {
        payload_errore(out, -32602, "serve chiave (stringa)");
        return 1;
    }
    uint32_t vl = 0;
    const uint8_t *v = motore_get(s->mo, chiave, kl, &vl);
    if (!v) {
        JBUF_LIT(out, "null");        /* il silenzio e' onesto [M4] */
        return 0;
    }
    /* il colpo del prefetcher [P7]: get con valore di una chiave
     * parcheggiata e viva. Contato una volta sola, qui. */
    if (s->pf)
        prefetch_tocca(s->pf, chiave, kl, s->richieste);
    motore_risposta_t r;
    r.chiave     = (const uint8_t *)chiave;
    r.chiave_len = (uint32_t)kl;
    r.valore     = v;
    r.valore_len = vl;
    r.confidenza = 1.0;                  /* l'esatto non mente [M3]   */
    scrivi_item(out, &r);
    return 0;
}

static int tool_chiedi(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    size_t ql = 0;
    const char *query = arg_str(arg, "query", &ql);
    if (!query) {
        payload_errore(out, -32602, "serve query (stringa)");
        return 1;
    }
    int valido = 0;
    double kd = json_num(json_get(arg, "k"), &valido);
    /* gialli della review: NaN e compagnie passavano i confronti e
     * finivano nel cast (UB); le frazioni si troncavano in silenzio.
     * Assente = il default; presente e sporco = rifiutato ad alta
     * voce. Il tetto 1e15 protegge il cast a size_t oltre 2^53, dove
     * il double non distingue piu' gli interi. */
    if (valido && (!(kd >= 0 && kd <= 1e15) || kd != (uint64_t)kd)) {
        payload_errore(out, -32602,
                       "k non valido (intero 0..1000000000000000)");
        return 1;
    }
    size_t k = valido ? (size_t)kd : CHIEDI_K_MAX;
    if (k < 1)
        k = 1;
    if (k > CHIEDI_K_MAX)
        k = CHIEDI_K_MAX;
    motore_risposta_t risp[CHIEDI_K_MAX];
    size_t n = motore_chiedi(s->mo, query, ql, k, risp, NULL);
    JBUF_LIT(out, "[");               /* vuoto = silenzio [M4]     */
    for (size_t i = 0; i < n; i++) {
        if (i)
            JBUF_LIT(out, ",");
        scrivi_item(out, &risp[i]);
    }
    JBUF_LIT(out, "]");
    return 0;
}

static int tool_intervallo(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    size_t dal = 0, al = 0;
    const char *da = arg_str(arg, "da", &dal);
    const char *a  = arg_str(arg, "a", &al);
    if (!da || !a) {
        payload_errore(out, -32602, "servono da e a (stringhe)");
        return 1;
    }
    int valido = 0;
    double kd = json_num(json_get(arg, "k"), &valido);
    /* la stessa disciplina del k di chiedi (gialli della review)      */
    if (valido && (!(kd >= 0 && kd <= 1e15) || kd != (uint64_t)kd)) {
        payload_errore(out, -32602,
                       "k non valido (intero 0..1000000000000000)");
        return 1;
    }
    size_t k = valido ? (size_t)kd : INTERVALLO_K_MAX / 2;
    if (k < 1)
        k = 1;
    if (k > INTERVALLO_K_MAX)
        k = INTERVALLO_K_MAX;
    motore_risposta_t risp[INTERVALLO_K_MAX];
    size_t totali = 0;
    size_t n = motore_intervallo(s->mo, da, dal, a, al, k, risp,
                                 &totali, NULL);
    JBUF_LIT(out, "{\"totali\":");   /* il batch PIU' il totale [M2] */
    jbuf_num(out, (double)totali);
    JBUF_LIT(out, ",\"trovate\":[");
    for (size_t i = 0; i < n; i++) {
        if (i)
            JBUF_LIT(out, ",");
        scrivi_item(out, &risp[i]);
    }
    JBUF_LIT(out, "]}");
    return 0;
}

static int tool_presente(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    const jval_t *arr = json_get(arg, "chiavi");
    if (!arr || arr->tipo != J_ARR || arr->n == 0) {
        payload_errore(out, -32602, "serve chiavi (array non vuoto)");
        return 1;
    }
    if (arr->n > PRESENTE_BATCH_MAX) {
        payload_errore(out, -32602, "max 256 chiavi per chiamata");
        return 1;
    }
    for (size_t i = 0; i < arr->n; i++) {
        if (!json_str(arr->figli[i], NULL)) {
            payload_errore(out, -32602, "chiavi: tutte stringhe");
            return 1;
        }
    }
    JBUF_LIT(out, "[");               /* N verdetti, una chiamata [M5] */
    for (size_t i = 0; i < arr->n; i++) {
        size_t kl = 0;
        const char *chiave = json_str(arr->figli[i], &kl);
        if (i)
            JBUF_LIT(out, ",");
        jbuf_num(out, (double)motore_presente(s->mo, chiave, kl));
    }
    JBUF_LIT(out, "]");
    return 0;
}

static int tool_del(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    size_t kl = 0;
    const char *chiave = arg_str(arg, "chiave", &kl);
    if (!chiave) {
        payload_errore(out, -32602, "serve chiave (stringa)");
        return 1;
    }
    /* la vittima si fotografa PRIMA (la get e' a costo ~zero): se il
     * motore poi rifiuta per memoria, la compensazione la rimette nel
     * log — il TTL si perde, limite dichiarato del mondo-OOM. Per la
     * MANCA non serve nulla: il DEL di una chiave assente al replay e'
     * legale per contratto [W5]. */
    uint32_t vl_vecchia = 0;
    const uint8_t *vecchio = motore_get(s->mo, chiave, kl, &vl_vecchia);
    /* il tombstone si inchiostra prima [W1]; al replay il DEL di una
     * chiave assente e' legale: puo' essere scaduta per conto suo [W5] */
    if (s->wal && wal_del(s->wal, chiave, kl,
                          motore_adesso(s->mo)) != WAL_OK) {
        payload_errore(out, MOTORE_ERR_IO,
                        "WAL non scrivibile: delete rifiutata (durabilita' o niente)");
        return 1;
    }
    int r = motore_del(s->mo, chiave, kl);
    if (r != MOTORE_OK) {
        if (r == MOTORE_ERR_MANCA || !vecchio) {
            payload_errore(out, r, msg_motore(r));
            return 1;                  /* assente: il replay fa lo
                                          * stesso, legale [W5]         */
        }
        /* POST MORTEM (review, rossi del 22-11, il gemello della put):
         * senza compensazione, il replay uccideva cio' che il vivo
         * aveva risparmiato — la fotografia va rinchiostrata. Se pure
         * la compensazione cade, il WAL e' rotto per conto suo e ogni
         * prossima scrittura si rifiuta ad alta voce. */
        if (s->wal &&
                wal_put(s->wal, chiave, kl, (const char *)vecchio,
                        vl_vecchia, 0,
                        motore_adesso(s->mo)) == WAL_OK)
            payload_errore(out, r, msg_motore(r));
        else
            payload_errore(out, MOTORE_ERR_IO,
                          "delete rifiutata E la compensazione caduta:"
                          " il WAL e' compromesso, da qui ogni"
                          " scrittura si rifiuta");
        return 1;
    }
    JBUF_LIT(out, "{\"cancellata\":true}");
    return 0;
}

static int tool_avanza(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    int valido = 0;
    double t = json_num(json_get(arg, "adesso"), &valido);
    if (!valido || t < 0 || t > 4294967295.0 || t != (uint32_t)t) {
        payload_errore(out, -32602, "serve adesso (intero 0..4294967295)");
        return 1;
    }
    motore_avanza(s->mo, (uint32_t)t);   /* l'orologio e' tuo [M6][I8] */
    JBUF_LIT(out, "{\"adesso\":");
    jbuf_num(out, (double)motore_adesso(s->mo));
    JBUF_LIT(out, "}");
    return 0;
}

static int tool_audit(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    (void)arg;                            /* audit non prende ordini   */
    size_t dichiarate = 0;
    size_t vive = motore_audit(s->mo, &dichiarate);
    JBUF_LIT(out, "{\"vive\":");
    jbuf_num(out, (double)vive);
    JBUF_LIT(out, ",\"dichiarate\":");
    jbuf_num(out, (double)dichiarate);
    JBUF_LIT(out, ",\"coerente\":");
    if (vive == dichiarate)
        JBUF_LIT(out, "true}");
    else
        JBUF_LIT(out, "false}");
    return 0;
}

static int tool_salva(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    size_t nl = 0;
    const char *nome = arg_str(arg, "nome", &nl);
    if (!nome || nl == 0) {
        payload_errore(out, -32602, "serve nome (stringa non vuota)");
        return 1;
    }
    int r = motore_salva(s->mo, nome);
    if (r != MOTORE_OK) {
        payload_errore(out, r, msg_motore(r));
        return 1;
    }
    /* il taglio del WAL viene DOPO lo snapshot, e SOLO se lo snapshot
     * e' tornato sullo stesso file che il WAL custodisce [W5]: salvare
     * altrove non taglia il log di un altro db */
    if (s->wal && strcmp(nome, s->percorso_db) == 0) {
        if (wal_taglia(s->wal) != WAL_OK) {
            payload_errore(out, MOTORE_ERR_IO,
                           "snapshot salvato ma WAL non tagliato: riprova il salva");
            return 1;
        }
    }
    JBUF_LIT(out, "{\"salvato\":true}");
    return 0;
}

/* la MISURA del capitolo 12 [P8]: numeri, niente prosa. Il gate e'
 * dell'agente che legge: hit_rate basso = la speculazione inquina
 * piu' di quanto risparmia, e il dato e' onesto anche quando
 * smentisce l'idea. */
static int tool_prefetch_stat(mcp_t *s, const jval_t *arg, jbuf_t *out)
{
    (void)arg;                            /* la misura non prende ordini */
    if (!s->pf) {
        payload_errore(out, -32602,
                       "il parcheggio vive solo su un db da file");
        return 1;
    }
    uint64_t parcheggiate = 0, vive = 0, hit = 0, morte = 0;
    uint64_t spesa = 0, risparmio = 0;
    prefetch_stat(s->pf, s->richieste, &parcheggiate, &vive, &hit,
                  &morte, &spesa, &risparmio);
    JBUF_LIT(out, "{\"parcheggiate\":");
    jbuf_num(out, (double)parcheggiate);
    JBUF_LIT(out, ",\"vive\":");
    jbuf_num(out, (double)vive);
    JBUF_LIT(out, ",\"hit\":");
    jbuf_num(out, (double)hit);
    JBUF_LIT(out, ",\"morte\":");
    jbuf_num(out, (double)morte);
    JBUF_LIT(out, ",\"hit_rate\":");
    if (parcheggiate > 0)
        jbuf_num(out, (double)hit / (double)parcheggiate);
    else
        JBUF_LIT(out, "null");        /* nessuna predizione: nessun
                                          tasso, non zero finto [M4] */
    JBUF_LIT(out, ",\"spesa_handle\":");
    jbuf_num(out, (double)spesa);
    JBUF_LIT(out, ",\"risparmio\":");
    jbuf_num(out, (double)risparmio);
    JBUF_LIT(out, ",\"bilancio\":");
    /* il bilancio e' SIGNED: senza colpi la spesa vince, e il
     * numero deve dirlo (risparmio - spesa tra uint64_t faceva
     * il giro di 2^64: 1.8e19 invece di -108 — post mortem) */
    jbuf_num(out, (double)((int64_t)risparmio - (int64_t)spesa));
    JBUF_LIT(out, "}");
    return 0;
}

/* --------------------------------------------- il catalogo dei tool [M8] */

typedef int (*tool_fn)(mcp_t *, const jval_t *, jbuf_t *);

typedef struct {
    const char *nome;
    const char *descrizione;
    const char *schema;
    tool_fn     gira;
} tool_t;

static const tool_t CATALOGO[] = {
{ "put",
  "Scrive una coppia chiave/valore, con TTL opzionale in tick (INTERO; "
  "le frazioni e i non-numeri si rifiutano). Il TTL e' "
  "inclusivo: la entry nata a t con TTL T e' morta quando adesso >= t + T; "
  "0 = per sempre. Sovrascrive la chiave esistente. Oltre 4096 tick il TTL "
  "viene rifiutato (e' la finestra dell'anello di scadenze).",
  "{\"type\":\"object\",\"properties\":{"
  "\"chiave\":{\"type\":\"string\"},"
  "\"valore\":{\"type\":\"string\"},"
  "\"ttl\":{\"type\":\"integer\",\"description\":\"vita in tick; 0 = per sempre\"}"
  "},\"required\":[\"chiave\",\"valore\"]}",
  tool_put },

{ "get",
  "Ritorna il valore della chiave ESATTA con confidenza 1.0, oppure null: "
  "il silenzio e' onesto, mai bugie. Se non ricordi la chiave esatta, "
  "usa chiedi.",
  "{\"type\":\"object\",\"properties\":{"
  "\"chiave\":{\"type\":\"string\"}"
  "},\"required\":[\"chiave\"]}",
  tool_get },

{ "chiedi",
  "La domanda probabile: fino a k (max 8) risposte vicine alla query, "
  "OGNUNA con chiave, valore e confidenza dichiarata (1.0 = esatta, "
  "altrimenti similarita' Jaccard 0..1). ATTENZIONE: la similarita' e' "
  "SINTATTICA (bigrammi posizionali + MinHash), NON semantica: capisce "
  "refusi e chiavi quasi identiche, NON sinonimi. Confidenza bassa = "
  "ritenta con un'altra formulazione: e' auto-correzione a un giro.",
  "{\"type\":\"object\",\"properties\":{"
  "\"query\":{\"type\":\"string\"},"
  "\"k\":{\"type\":\"integer\",\"description\":\"max risposte (1..8)\"}"
  "},\"required\":[\"query\"]}",
  tool_chiedi },

{ "intervallo",
  "Tutte le chiavi vive in [da, a] in ordine di chiave, col valore: il "
  "lavoro che un indice hash non sa fare. Ritorna fino a k coppie (max 64) "
  "PIU' il totale delle corrispondenze anche oltre il cap.",
  "{\"type\":\"object\",\"properties\":{"
  "\"da\":{\"type\":\"string\"},"
  "\"a\":{\"type\":\"string\"},"
  "\"k\":{\"type\":\"integer\",\"description\":\"max coppie (1..64)\"}"
  "},\"required\":[\"da\",\"a\"]}",
  tool_intervallo },

{ "presente",
  "Prefiltro economico e BATCH: una chiamata, N chiavi (max 256), N "
  "verdetti. 0 = certamente non c'e' (mai falsi negativi sulle vive), "
  "1 = probabilmente c'e' (Bloom: le cancellate restano \"1\" fino alla "
  "prossima compattazione — e' un filtro, non un indice). Usalo per "
  "decidere SE val la pena chiamare get o chiedi.",
  "{\"type\":\"object\",\"properties\":{"
  "\"chiavi\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"
  "},\"required\":[\"chiavi\"]}",
  tool_presente },

{ "del",
  "Cancella una chiave. Errore MANCA se la chiave non esiste.",
  "{\"type\":\"object\",\"properties\":{"
  "\"chiave\":{\"type\":\"string\"}"
  "},\"required\":[\"chiave\"]}",
  tool_del },

{ "avanza",
  "Muove il tempo del motore al tick ASSOLUTO (non un delta) e fa scadere "
  "in batch. L'orologio e' del chiamante: il server non ha un orologio "
  "da muro, il tempo si muove solo se lo muovi tu.",
  "{\"type\":\"object\",\"properties\":{"
  "\"adesso\":{\"type\":\"integer\",\"description\":\"tick assoluto\"}"
  "},\"required\":[\"adesso\"]}",
  tool_avanza },

{ "audit",
  "Autoverifica del contratto: conta le entry vive camminando l'arena e "
  "le confronta col contatore interno. coerente=false = contratto rotto: "
  "non fidarti dei dati.",
  "{\"type\":\"object\",\"properties\":{}}",
  tool_audit },

{ "salva",
  "Scrive lo snapshot compattato su disco (formato PDB1, CRC-32 per "
  "entry, prefisso sicuro in lettura).",
  "{\"type\":\"object\",\"properties\":{"
  "\"nome\":{\"type\":\"string\"}"
  "},\"required\":[\"nome\"]}",
  tool_salva },

{ "prefetch_stat",
  "La MISURA del prefetcher speculativo (capitolo 12): quante "
  "predizioni parcheggiate, quante vive, quanti colpi (get di una "
  "chiave parcheggiata), quante morte senza colpo (inquinamento), "
  "hit_rate, e il bilancio in byte (risparmio delle scoperte evitate "
  "meno spesa degli handle iniettati). LIMITI: le predizioni vengono "
  "dalla similarita' SINTATTICA della traccia di ragionamento, non "
  "semantica; hit_rate basso = la speculazione costa piu' di quanto "
  "risparmia: il dato e' onesto anche quando smentisce.",
  "{\"type\":\"object\",\"properties\":{}}",
  tool_prefetch_stat },
};

#define N_TOOL (sizeof CATALOGO / sizeof CATALOGO[0])

static const tool_t *trova_tool(const char *nome)
{
    if (!nome)
        return NULL;
    for (size_t i = 0; i < N_TOOL; i++)
        if (strcmp(CATALOGO[i].nome, nome) == 0)
            return &CATALOGO[i];
    return NULL;
}

/* ------------------------------------------------- il filo JSON-RPC [M1] */

static void risposta_errore(jbuf_t *b, const jval_t *id, int codice,
                            const char *msg)
{
    JBUF_LIT(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_scrivi(b, id);                  /* NULL -> "null"            */
    JBUF_LIT(b, ",\"error\":{\"code\":");
    jbuf_num(b, (double)codice);
    JBUF_LIT(b, ",\"message\":\"");
    jbuf_testo(b, msg, strlen(msg));
    JBUF_LIT(b, "\"}}");
}

static void scrivi_tools_list(jbuf_t *b)
{
    JBUF_LIT(b, "{\"tools\":[");
    for (size_t i = 0; i < N_TOOL; i++) {
        if (i)
            JBUF_LIT(b, ",");
        JBUF_LIT(b, "{\"name\":\"");
        jbuf_add(b, CATALOGO[i].nome, strlen(CATALOGO[i].nome));
        JBUF_LIT(b, "\",\"description\":\"");
        jbuf_testo(b, CATALOGO[i].descrizione,
                   strlen(CATALOGO[i].descrizione));
        JBUF_LIT(b, "\",\"inputSchema\":");
        jbuf_add(b, CATALOGO[i].schema, strlen(CATALOGO[i].schema));
        JBUF_LIT(b, "}");
    }
    JBUF_LIT(b, "]}");
}

/* l'involucro di tools/call: il payload viaggia come testo [M2] */
static void scrivi_result_tool(jbuf_t *b, const jval_t *id,
                               const char *payload, size_t plen,
                               int is_error)
{
    JBUF_LIT(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_scrivi(b, id);
    JBUF_LIT(b, ",\"result\":{");
    if (is_error)
        JBUF_LIT(b, "\"isError\":true,");
    JBUF_LIT(b, "\"content\":[{\"type\":\"text\",\"text\":\"");
    jbuf_testo(b, payload, plen);
    JBUF_LIT(b, "\"}]}}");
}

int mcp_gestisci(mcp_t *s, const char *richiesta, size_t len,
                 char **risposta, size_t *risposta_len)
{
    if (risposta)
        *risposta = NULL;
    if (risposta_len)
        *risposta_len = 0;
    if (!s || !richiesta || !risposta)
        return -1;

    jbuf_t b;
    if (jbuf_init(&b) != 0)
        return -1;

    jval_t *root = json_parse(richiesta, len);
    s->richieste++;

    /* la consumazione dello staging [P3][P6]: un file piccolo per
     * richiesta, l'orologio e' il contatore qui sopra. Il costo e'
     * dichiarato nel banco del capitolo 12. */
    if (s->pf)
        prefetch_consuma(s->pf, s->staging, s->richieste);

    if (!root) {
        risposta_errore(&b, NULL, -32700, "JSON non valido");
    } else {
        const jval_t *id = json_get(root, "id");
        const char *metodo = json_str(json_get(root, "method"), NULL);
        const jval_t *params = json_get(root, "params");

        if (!metodo) {
            if (id)
                risposta_errore(&b, id, -32600, "manca il method");
        } else if (!id) {
            /* notifica: accettata, nessuna risposta e' la risposta */
        } else if (strcmp(metodo, "initialize") == 0) {
            const char *init =
                ",\"result\":{\"protocolVersion\":\"" MCP_SPEC
                "\",\"capabilities\":{\"tools\":{}},"
                "\"serverInfo\":{\"name\":\"" SERVER_NOME
                "\",\"version\":\"" SERVER_VERSIONE "\"}}}";
            JBUF_LIT(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
            json_scrivi(&b, id);
            jbuf_add(&b, init, strlen(init));
        } else if (strcmp(metodo, "ping") == 0) {
            JBUF_LIT(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
            json_scrivi(&b, id);
            JBUF_LIT(&b, ",\"result\":{}}");
        } else if (strcmp(metodo, "tools/list") == 0) {
            JBUF_LIT(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
            json_scrivi(&b, id);
            JBUF_LIT(&b, ",\"result\":");
            scrivi_tools_list(&b);
            JBUF_LIT(&b, "}");           /* chiude il root: il post
                                             mortem del banco lo conta */
        } else if (strcmp(metodo, "tools/call") == 0) {
            const char *nome = json_str(json_get(params, "name"), NULL);
            const jval_t *args = json_get(params, "arguments");
            const tool_t *t = trova_tool(nome);
            if (!t) {
                risposta_errore(&b, id, -32602, "tool sconosciuto");
            } else {
                jbuf_t payload;
                /* gialli della review: l'init non si controllava e un
                 * malloc caduto toccava la struct morta a mani nude */
                if (jbuf_init(&payload) != 0) {
                    risposta_errore(&b, id, -32603,
                                   "memoria: il tool non ha potuto"
                                   " nemmeno cominciare");
                } else {
                    int is_error = t->gira(s, args, &payload);
                    scrivi_result_tool(&b, id, payload.p, payload.n,
                                       is_error);
                    s->byte_payload += payload.n;
                    jbuf_libera(&payload);
                }
            }
        } else {
            risposta_errore(&b, id, -32601, "method sconosciuto");
        }
    }
    json_libera(root);

    if (b.rotto) {                        /* morte atomica del buffer  */
        jbuf_libera(&b);
        return -1;
    }
    if (b.n == 0) {                       /* notifica: il silenzio e'  */
        jbuf_libera(&b);                  /* la risposta giusta [M1]   */
        return 0;
    }
    *risposta = b.p;
    if (risposta_len)
        *risposta_len = b.n;
    return 0;
}
