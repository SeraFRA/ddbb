/*
 * database_persist_semantic.c — decima iterazione: la fusione.
 *
 * =====================================================================
 * PERCHE' QUESTO FILE: CHIUDE IL CERCHIO
 * =====================================================================
 * Il file 5 ha dato al database la memoria di lungo termine (arena su
 * disco con CRC, snapshot compattato, load validato). Il file 7 gli ha
 * dato l'intuizione (MinHash + banding: la query col refuso cade nei
 * bucket dei suoi vicini). Ma erano due zavorre separate: uno ricordava
 * TUTTO senza capire niente, l'altro capiva senza ricordare niente tra
 * un avvio e l'altro.
 *
 * Il decimo file li fonde: la risposta semantica alita dal DISCO — il
 * valore torna insieme alla chiave, e il layer LSH e' RICOSTRUITO al
 * load dal nulla, perche' il file resta quello del file 5: solo arena,
 * solo CRC, zero firme. Se la semantica fosse stata persisted, il
 * formato sarebbe raddoppiato e ogni bug del formato avrebbe avuto due
 * modi di mentire. Il dischetto non sa che sopra ci vive una memoria
 * associativa: e' l'unica scelta onesta.
 *
 * =====================================================================
 * L'ESPERIMENTO: GLI INVARIANTI PRIMA DEL CODICE
 * =====================================================================
 * I file precedenti sono nati al contrario: codice, bug, post mortem.
 * Tre volte di fila lo stesso ciclo. Questa volta il contratto e'
 * stato scritto prima, e ha gia' catturato otto trappole DI DESIGN
 * (le rubber-duck sparate a tavolino, non a executable esploso):
 *
 *  [T1] la put di una chiave ESISTENTE creava una seconda voce:
 *       risposte doppie a pari confidenza -> la voce si RUSA (I2).
 *  [T2] una voce morta raggiunta dal probe avrebbe letto data[-1]:
 *       il confronto deve ignorare le voci morte.
 *  [T3] doppio conteggio della spazzatura (scadenza pigra e poi
 *       overwrite della stessa chiave): segno e conteggio di ogni
 *       morte vivono DENTRO derivo/muore, in una mano sola.
 *  [T4] put fallita dopo l'append lascerebbe un'entry orfana che la
 *       compattazione adotterebbe: usato non avanza finche' la
 *       derivazione non e' riuscita.
 *  [T5] db_del di una chiave solo scaduta (voce gia' zero): MANCA,
 *       senza ricontare la spazzatura.
 *  [T6] derivo() non puo' tornare "off vecchio E esito" insieme: i
 *       conti della morte sono di derivo, l'esito e' del chiamante.
 *  [T7] un file contraffatto con chiavi duplicate: first-wins al get,
 *       voci univoche, copia-ombra irraggiungibile in arena.
 *  [T8] la scadenza scoperta DURANTE il ranking semantico deve morire
 *       anche lei la voce (pulizia pigra anche in lettura).
 *  [T9] — catturata dal PRIMO RUN, NON dal pass degli invarianti: la
 *       put di una chiave esistente non segnava la entry SORPASSATA.
 *       Il file 5 se lo poteva permettere (compattava camminando gli
 *       SLOT: l'entry orfana era irraggiungibile); la fusione cammina
 *       l'ARENA, dove il bit MORTA e' l'unico che distingue la
 *       verita' dalla spazzatura. Eredita' difettosa da un'assunzione
 *       cambiata: l'invariante c'era (I1: "l'arena e' la verita'"),
 *       ma non e' stato letto fino in fondo. La lezione dell'esperimento.
 *
 * =====================================================================
 * IL CONTRATTO (ogni funzione del file lo paga o lo incassa)
 * =====================================================================
 *  [I1] FONTE UNICA. L'arena e' l'unica verita'; slot, voci, grammi,
 *       firme e bucket sono tutti DERIVATI, ricostruibili con UNA
 *       funzione sola (derivo) chiamata da put, carica e compatta.
 *       Nessun derivato contiene puntatori: solo offset, e la realloc
 *       dell'arena non invalida niente.
 *  [I2] IDENTITA'. Una voce = una chiave distinta viva. Sovrascrivere
 *       riusa la voce (stesso testo = stessa firma, non si risigla
 *       mai); cancellare la azzera; il load crea voci solo per le
 *       chiavi vive del file.
 *  [I3] MORTE ATOMICA. Ogni strada che accerta una morte o scadenza
 *       (delete, scadenza pigra al get [T8], ranking) azzera anche la
 *       voce nello stesso istante, dalla stessa funzione muore().
 *       E il sorpasso della put uccide la entry VECCHIA dentro derivo,
 *       bit MORTA e spazzatura nella stessa mano [T9]. Ogni morte
 *       lascia il segno E il conto: e' per questo che il dirty-check
 *       della salva non puo' mentire. E chi risponde paga UN solo
 *       controllo: voce a zero == chiave irraggiungibile.
 *  [I4] IL DISCO VEDE SOLO ENTRY VIVE. Si compatta PRIMA di scrivere:
 *       nel file finiscono solo byte di entry vive e intatte; il bit
 *       MORTA non esiste su disco e il CRC calcolato alla put resta
 *       valido per ogni byte del file.
 *  [I5] PREFISSO SICURO. Il load valida una entry per volta e al primo
 *       guasto (CRC o troncamento) SI FERMA: si carica il prefisso
 *       sano, si ignora la coda. (Regola del file 5, intatta.)
 *  [I6] RISPOSTE VIVE. Ogni risposta di db_chiedi — esatta o
 *       semantica — punta a una entry VIVA e riporta il SUO valore:
 *       un morto non risponde mai, nemmeno dal bucket dov'e' rimasto
 *       appollaiato. Tanto meno RISorge dal disco.
 *
 * =====================================================================
 * LA GEOMETRIA DELLA MEMORIA
 * =====================================================================
 *   arena (byte, la verita', l'unica cosa che va sul disco)
 *     [ fp | key_len | val_len | ttl | creazione(MORTA) | crc32 ] key val
 *   slot  { fp, id1 }         indice esatto: hash -> voce
 *   voce  { off1, gram_off, gram_cnt }   identita' semantica:
 *         l'entry corrente + i suoi bigrammi
 *   firma  32 bande x 4 minh per voce; bucket lists per banda
 *
 * 32 bande x 4 righe (il file 7 ne aveva 24): il confronto dei numeri
 * tra i due file e' un A/B vero — piu' bande = cattura piu' larga,
 * candidati in piu' da rankare. La matematica della banda e' sempre
 * quella di Broder: P(banda cattura) = s^R, P(totale) = 1-(1-s^R)^B.
 *
 * Limiti dichiarati: single-thread; db_carica vuole una struct virgin
 * (fa memset, come nel file 5); le tabelle di bucket si prenotano
 * intere a ogni init (2^17 uint32 per banda); i puntatori delle
 * risposte vivono fino alla prossima put/compattazione.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wpedantic"
# include <windows.h>
# pragma GCC diagnostic pop
#endif

/* ---------------------------- costanti di servizio (file 5) */
#define DIM_HEADER     24u
#define SLOT_INIZIALI  ((size_t)1 << 10)
#define LOAD_FACTOR_10 7
#define CAPACITA_MIN   ((size_t)1 << 16)
#define CAPACITA_MAX   ((size_t)1 << 28)

#define OFF1_VUOTO     0u
/* nel fusione lo slot non porta l'offset: porta la VOCE. Le sentinelle
 * si spostano su id1: 0 = slot vuoto, 0xFFFFFFFF = tombstone. */
#define SLOT_TOMBSTONE 0xFFFFFFFFu
#define VOCE_MORTA    0u
#define MORTA_BIT      0x80000000u

/* ---------------------------- costanti semantiche (file 7) */
#define N_BANKS       32        /* bande LSH (file 7: 24)             */
#define N_RIGHE       4         /* minh per banda                     */
#define N_BUCKET_BN   17        /* 2^17 bucket per banda              */
#define SOGLIA        0.40      /* confidenza minima dichiarata       */
#define MAX_CANDIDATI 8192      /* tetto difensivo per query          */
#define TOPLIM        8         /* k massimo di db_chiedi             */
#define MAX_GRAMMI    66        /* ^chiave$ -> al piu' 65 bigrammi     */
#define VOCI_INIZIALI 256

/* ---------------------------- formato del file (file 5)   */
#define FORMATO_VERSIONE 1u
static const uint8_t MAGIA[4] = { 'P', 'D', 'B', '1' };

enum {
    DB_OK          =  0,
    DB_ERR_MEMORY  = -1,
    DB_ERR_PIENO   = -2,
    DB_ERR_MANCA   = -3,
    DB_ERR_IO      = -4,
    DB_ERR_FORMATO = -5
};

typedef struct {
    uint32_t fp;      /* fingerprint FNV della chiave (come in arena) */
    uint32_t id1;     /* SLOT_TOMBSTONE / 0 vuoto / voce id + 1        */
} slot_t;

typedef struct {
    uint32_t off1;    /* entry corrente nell'arena +1; 0 = morta [I3]  */
    uint32_t gram_off;
    uint32_t gram_cnt;
} voce_t;

/* la risposta porta la chiave E il valore: pointers nell'arena,
 * validi fino alla prossima put/compattazione */
typedef struct {
    const uint8_t *chiave;
    uint32_t       chiave_len;
    const uint8_t *valore;
    uint32_t       valore_len;
    double         confidenza;
} risposta_t;

typedef struct {
    /* la verita' [I1] */
    uint8_t *data;
    size_t   capacita, usato, spazzatura;
    slot_t  *slot;
    size_t   n_slot, n_entry, n_tomb;
    /* il derivato semantico */
    voce_t   *voci;
    size_t    n_voci, capacita_voci;
    uint32_t *firma;                 /* [id * N_BANKS + banda]        */
    uint32_t *gram_arena;
    size_t    gram_usati;
    uint32_t *testa[N_BANKS];
    uint32_t *prossimo[N_BANKS];
    uint32_t *visto;
    uint32_t  marca;
} db_t;

/* ------------------------------------------------------------ utilita' */

static uint32_t leggi_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static void    scrivi_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint32_t calcolo_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)(uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static size_t prossima_potenza(size_t v)
{
    size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

static double ora_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = { { 0, 0 } };
    LARGE_INTEGER c;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* PRNG deterministico per i test (stato globale di processo) */
static uint32_t prng(void)
{
    static uint32_t x = 0x12345678u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

/* ------------------------------------------------------------- CRC-32 */

static uint32_t crc_tabella[256];
static int crc_pronta = 0;

static void crc_init(void)
{
    if (crc_pronta)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tabella[i] = c;
    }
    crc_pronta = 1;
}

static uint32_t crc32_inizio(void)                 { return 0xFFFFFFFFu; }
static uint32_t crc32_fine(uint32_t s)             { return s ^ 0xFFFFFFFFu; }

static uint32_t crc32_aggiorna(uint32_t s, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        s = crc_tabella[(s ^ p[i]) & 0xFFu] ^ (s >> 8);
    return s;
}

/* ------------------------------------------------------ entry in arena */

static size_t entry_dim(const uint8_t *e)
{
    return DIM_HEADER + leggi_u32(e + 4) + leggi_u32(e + 8);
}

static int entry_morta(const uint8_t *e)
{
    return (leggi_u32(e + 16) & MORTA_BIT) != 0;
}

static void segna_morta(uint8_t *data, size_t off)
{
    scrivi_u32(data + off + 16, leggi_u32(data + off + 16) | MORTA_BIT);
}

/* ------------------------------------- bigrammi posizionali (file 7) */

static uint32_t gram_hash(char a, char b, uint32_t pos)
{
    uint32_t base = (((uint32_t)(uint8_t)a << 8) | (uint8_t)b)
                  ^ (pos * 0x9e3779b9u);
    return mix(base + 0xabcde987u);
}

static uint32_t costruisci_grammi(uint32_t *out, const char *s, size_t len)
{
    char buf[MAX_GRAMMI];
    if (len > MAX_GRAMMI - 2)
        len = MAX_GRAMMI - 2;
    buf[0] = '^';
    memcpy(buf + 1, s, len);
    buf[len + 1] = '$';

    uint32_t n = (uint32_t)len + 1;
    for (uint32_t i = 0; i < n; i++)
        out[i] = gram_hash(buf[i], buf[i + 1], i);

    /* insertion sort + dedupe: e' un INSIEME, servi alla Jaccard */
    for (uint32_t i = 1; i < n; i++) {
        uint32_t v = out[i];
        uint32_t j = i;
        while (j > 0 && out[j - 1] > v) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = v;
    }
    uint32_t u = 0;
    for (uint32_t i = 0; i < n; i++)
        if (u == 0 || out[i] != out[u - 1])
            out[u++] = out[i];
    return u;
}

static double jaccard(const uint32_t *a, uint32_t na,
                       const uint32_t *b, uint32_t nb)
{
    uint32_t i = 0, j = 0, inter = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) i++;
        else if (a[i] > b[j]) j++;
        else { inter++; i++; j++; }
    }
    uint32_t uni = na + nb - inter;
    return uni ? (double)inter / (double)uni : 0.0;
}

static inline uint32_t hash_riga(uint32_t base, uint32_t k)
{
    return mix(base ^ (k * 0x9e3779b9u + 0x85ebca6bu));
}

static void firma_di_grammi(uint32_t *firma, const uint32_t *grammi, uint32_t ng)
{
    for (uint32_t b = 0; b < N_BANKS; b++) {
        uint32_t mini[N_RIGHE];
        for (uint32_t r = 0; r < N_RIGHE; r++)
            mini[r] = 0xffffffffu;
        for (uint32_t g = 0; g < ng; g++) {
            uint32_t base = grammi[g];
            for (uint32_t r = 0; r < N_RIGHE; r++) {
                uint32_t h = hash_riga(base, b * N_RIGHE + r);
                if (h < mini[r])
                    mini[r] = h;
            }
        }
        uint32_t bh = 0x9e3779b9u;
        for (uint32_t r = 0; r < N_RIGHE; r++)
            bh = mix(bh ^ mini[r]);
        firma[b] = bh;
    }
}

/* -------------------------------------------------------- indice esatto */

/*
 * Probe LINEARE sullo slot. Il match non guarda SOLO fp e testo: guarda
 * anche che la voce sia VIVA. [T2] Una voce a zero in uno slot occupato
 * e' una scadenza notata in passato: quello slot e' trasparente per il
 * probe — matcharlo significherebbe dereferenziare data[-1].
 */
static size_t trova_slot(const db_t *db, uint32_t hash,
                         const char *key, size_t key_len, int *trovata)
{
    size_t idx = hash & (db->n_slot - 1);
    size_t tomb = (size_t)-1;
    *trovata = 0;

    for (;;) {
        uint32_t id1 = db->slot[idx].id1;

        if (id1 == OFF1_VUOTO)
            return tomb == (size_t)-1 ? idx : tomb;

        if (id1 != SLOT_TOMBSTONE &&
            db->slot[idx].fp == hash &&
            db->voci[id1 - 1].off1 != VOCE_MORTA) {
            const uint8_t *e = db->data + db->voci[id1 - 1].off1 - 1;
            if (leggi_u32(e + 4) == key_len &&
                memcmp(e + DIM_HEADER, key, key_len) == 0) {
                *trovata = 1;
                return idx;
            }
        }
        if (id1 == SLOT_TOMBSTONE && tomb == (size_t)-1)
            tomb = idx;
        idx = (idx + 1) & (db->n_slot - 1);
    }
}

static int cresci_slot(db_t *db)
{
    size_t nuovo_n = db->n_slot * 2;
    slot_t *nuovo = calloc(nuovo_n, sizeof *nuovo);
    if (!nuovo)
        return DB_ERR_MEMORY;
    for (size_t v = 0; v < db->n_slot; v++)
        if (db->slot[v].id1 != OFF1_VUOTO &&
            db->slot[v].id1 != SLOT_TOMBSTONE) {
            size_t idx = db->slot[v].fp & (nuovo_n - 1);
            while (nuovo[idx].id1 != OFF1_VUOTO)
                idx = (idx + 1) & (nuovo_n - 1);
            nuovo[idx] = db->slot[v];
        }
    free(db->slot);
    db->slot = nuovo;
    db->n_slot = nuovo_n;
    db->n_tomb = 0;
    printf("[indice] raddoppiata a %zu slot\n", nuovo_n);
    return DB_OK;
}

/* ------------------------------------------------ arena e voce: crescita */

static int assicura_spazio(db_t *db, size_t dim)
{
    if (dim <= db->capacita - db->usato)
        return DB_OK;
    size_t nuova = db->capacita;
    while (dim > nuova - db->usato && nuova < CAPACITA_MAX)
        nuova *= 2;
    if (dim > nuova - db->usato)
        return DB_ERR_PIENO;
    uint8_t *p = realloc(db->data, nuova);
    if (!p)
        return DB_ERR_MEMORY;
    if (nuova != db->capacita)
        printf("[buffer] riallocato a %zu KB\n", nuova / 1024);
    db->data = p;
    db->capacita = nuova;
    return DB_OK;
}

/* crescere le voci: nessun puntatore da riparare, solo offset [I1] */
static int assicura_voci(db_t *db)
{
    if (db->n_voci < db->capacita_voci)
        return DB_OK;
    size_t nuova = db->capacita_voci * 2;

    voce_t *nv = realloc(db->voci, nuova * sizeof *nv);
    if (!nv) return DB_ERR_MEMORY;
    db->voci = nv;

    uint32_t *nf = realloc(db->firma, nuova * N_BANKS * sizeof *nf);
    if (!nf) return DB_ERR_MEMORY;
    db->firma = nf;

    uint32_t *ngram = realloc(db->gram_arena, nuova * MAX_GRAMMI * sizeof *ngram);
    if (!ngram) return DB_ERR_MEMORY;
    db->gram_arena = ngram;

    uint32_t *nvisto = realloc(db->visto, nuova * sizeof *nvisto);
    if (!nvisto) return DB_ERR_MEMORY;
    memset(nvisto + db->capacita_voci, 0,
           (nuova - db->capacita_voci) * sizeof *nvisto);
    db->visto = nvisto;

    for (uint32_t b = 0; b < N_BANKS; b++) {
        uint32_t *np = realloc(db->prossimo[b], nuova * sizeof *np);
        if (!np) return DB_ERR_MEMORY;
        memset(np + db->capacita_voci, 0,
               (nuova - db->capacita_voci) * sizeof *np);
        db->prossimo[b] = np;
    }
    db->capacita_voci = nuova;
    return DB_OK;
}

/* ------------------------------------------------------------ la morte */

/*
 * L'UNICA strada della morte [I3]: bit MORTA sull'entry, spazzatura
 * contata, voce azzerata, contatore delle vive calato. Cancellare,
 * scadere al get, scadere nel ranking: tutti passano di qui.
 */
static void muore(db_t *db, size_t id)
{
    size_t off = db->voci[id].off1 - 1;
    segna_morta(db->data, off);
    db->spazzatura += entry_dim(db->data + off);
    db->voci[id].off1 = VOCE_MORTA;
    db->n_entry--;
}

/* --------------------------------------------------------------- derivo */

/*
 * derivo — il cuore della fusione. L'UNICA strada per cui una entry
 * dell'arena entra negli indici, TUTTI E TRE (esatto, grammi, LSH).
 * Tre chiamanti, un solo codice [I1]:
 *
 *   derivo(db, off, 1) dalla put: la voce esistente va RIPUNTATA alla
 *                    entry nuova (ultima scrittura vince) [T1]
 *   derivo(db, off, 0) dal load: replay del file, la voce resta alla
 *                    PRIMA copia (first-wins) [T7]
 *   derivo(db, off, 0) dalla compattazione: ricostruzione dal nulla
 *
 * Se fallisce (memoria), nessun derivato e' stato toccato prima del
 * fallimento e la put puo' srotolare la coda dell'arena [T4].
 */
static int derivo(db_t *db, size_t off, int aggiorna)
{
    const uint8_t *e = db->data + off;
    uint32_t fp  = leggi_u32(e);
    uint32_t klen = leggi_u32(e + 4);
    const char *key = (const char *)(e + DIM_HEADER);

    if ((db->n_entry + db->n_tomb + 1) * 10 > db->n_slot * LOAD_FACTOR_10)
        if (cresci_slot(db) != DB_OK)
            return DB_ERR_MEMORY;

    int trovata;
    size_t idx = trova_slot(db, fp, key, klen, &trovata);

    if (trovata) {
        size_t id = (size_t)db->slot[idx].id1 - 1;
        uint32_t vecchio = db->voci[id].off1;    /* >= 1: il probe non
                                                  * matcha voci morte */
        if (aggiorna) {
            /* il sorpasso della put: la entry VECCHIA muore con il bit
             * MORTA nella stessa mano che punta la voce alla nuova.
             * Senza il bit, la compattazione per-arena non vedrebbe
             * mai questa morte: e' la lezione [T9] */
            segna_morta(db->data, vecchio - 1);
            db->spazzatura += entry_dim(db->data + vecchio - 1);
            db->voci[id].off1 = (uint32_t)(off + 1);
        } else {
            /* replay di un file con duplicati: la voce resta alla
             * prima copia; quella in eccesso e' un'OMBRA e muore ORA,
             * sepolta nel conto spazzatura [T7] */
            segna_morta(db->data, off);
            db->spazzatura += entry_dim(db->data + off);
        }
        return DB_OK;
    }

    if (assicura_voci(db) != DB_OK)
        return DB_ERR_MEMORY;                     /* slot intatti: rollback pulito */

    if (db->slot[idx].id1 == SLOT_TOMBSTONE)
        db->n_tomb--;

    size_t id = db->n_voci;
    uint32_t grammi[MAX_GRAMMI];
    uint32_t ng = costruisci_grammi(grammi, key, klen);

    db->voci[id].off1     = (uint32_t)(off + 1);
    db->voci[id].gram_off = (uint32_t)db->gram_usati;
    db->voci[id].gram_cnt = ng;
    memcpy(db->gram_arena + db->gram_usati, grammi, ng * sizeof *grammi);
    db->gram_usati += ng;

    firma_di_grammi(db->firma + id * N_BANKS, grammi, ng);
    for (uint32_t b = 0; b < N_BANKS; b++) {
        size_t bh = db->firma[id * N_BANKS + b] & (((size_t)1 << N_BUCKET_BN) - 1);
        db->prossimo[b][id] = db->testa[b][bh];
        db->testa[b][bh] = (uint32_t)(id + 1);
    }

    db->n_voci++;
    db->slot[idx].fp  = fp;
    db->slot[idx].id1 = (uint32_t)(id + 1);
    db->n_entry++;                                /* derivo possiede il contatore */
    return DB_OK;
}

/*
 * deriva_db: azzera OGNI derivato e ricostruisce derivo per derivo,
 * camminando l'arena entry per entry (nessuna fiducia negli slot:
 * l'arena e' la verita' [I1]). La compattazione la chiama sul nuovo
 * buffer appena scritto.
 */
static int deriva_db(db_t *db)
{
    memset(db->slot, 0, db->n_slot * sizeof *db->slot);
    for (uint32_t b = 0; b < N_BANKS; b++)
        memset(db->testa[b], 0, ((size_t)1 << N_BUCKET_BN) * sizeof **db->testa);
    db->n_entry   = 0;
    db->n_tomb    = 0;
    db->n_voci    = 0;
    db->gram_usati = 0;

    size_t off = 0;
    while (off < db->usato) {
        if (derivo(db, off, 0) != DB_OK)
            return DB_ERR_MEMORY;
        off += entry_dim(db->data + off);
    }
    return DB_OK;
}

/* ------------------------------------------------------------ database */

int db_init(db_t *db)
{
    memset(db, 0, sizeof *db);
    crc_init();

    db->capacita = CAPACITA_MIN;
    db->data = malloc(db->capacita);
    db->n_slot = SLOT_INIZIALI;
    db->slot = calloc(db->n_slot, sizeof *db->slot);

    db->capacita_voci = VOCI_INIZIALI;
    db->voci        = malloc(db->capacita_voci * sizeof *db->voci);
    db->firma       = malloc(db->capacita_voci * N_BANKS * sizeof *db->firma);
    db->gram_arena  = malloc(db->capacita_voci * MAX_GRAMMI * sizeof *db->gram_arena);
    db->visto       = calloc(db->capacita_voci, sizeof *db->visto);

    if (!db->data || !db->slot || !db->voci || !db->firma ||
        !db->gram_arena || !db->visto)
        return DB_ERR_MEMORY;

    for (uint32_t b = 0; b < N_BANKS; b++) {
        db->testa[b]    = calloc((size_t)1 << N_BUCKET_BN, sizeof **db->testa);
        db->prossimo[b] = calloc(db->capacita_voci, sizeof **db->prossimo);
        if (!db->testa[b] || !db->prossimo[b])
            return DB_ERR_MEMORY;
    }
    db->marca = 1;
    return DB_OK;
}

void db_libera(db_t *db)
{
    free(db->data);
    free(db->slot);
    free(db->voci);
    free(db->firma);
    free(db->gram_arena);
    free(db->visto);
    for (uint32_t b = 0; b < N_BANKS; b++) {
        free(db->testa[b]);
        free(db->prossimo[b]);
    }
    memset(db, 0, sizeof *db);
}

static void statistiche(const db_t *db, const char *quando)
{
    printf("%-14s %8zu byte, %6zu spazzatura (%4.1f%%), %5zu vive, %5zu voci, "
           "%6zu slot (LF %2.0f%%), firme %zu KB + grammi %zu KB\n",
           quando, db->usato, db->spazzatura,
           db->usato ? 100.0 * (double)db->spazzatura / (double)db->usato : 0.0,
           db->n_entry, db->n_voci, db->n_slot,
           100.0 * (double)(db->n_entry + db->n_tomb) / (double)db->n_slot,
           db->n_voci * N_BANKS * sizeof(uint32_t) / 1024,
           db->gram_usati * sizeof(uint32_t) / 1024);
}

/*
 * db_put: appende nell'arena (CRC a pagamento O(k+v)), poi deriva.
 * La coda dell'arena avanza SOLO se la derivazione e' riuscita [T4]:
 * mai orfani adottabili dalla prossima compattazione.
 */
int db_put(db_t *db, const char *key, size_t key_len,
           const char *valore, size_t val_len, uint32_t ttl)
{
    size_t dim = DIM_HEADER + key_len + val_len;
    int r = assicura_spazio(db, dim);
    if (r != DB_OK)
        return r;

    uint32_t fp = calcolo_hash(key, key_len);
    size_t off = db->usato;
    uint8_t *dest = db->data + off;
    scrivi_u32(dest +  0, fp);
    scrivi_u32(dest +  4, (uint32_t)key_len);
    scrivi_u32(dest +  8, (uint32_t)val_len);
    scrivi_u32(dest + 12, ttl);
    scrivi_u32(dest + 16, (uint32_t)time(NULL));
    memcpy(dest + DIM_HEADER, key, key_len);
    memcpy(dest + DIM_HEADER + key_len, valore, val_len);

    uint32_t s = crc32_inizio();
    s = crc32_aggiorna(s, dest, 20);
    s = crc32_aggiorna(s, dest + DIM_HEADER, key_len + val_len);
    scrivi_u32(dest + 20, crc32_fine(s));

    if (derivo(db, off, 1) != DB_OK)
        return DB_ERR_MEMORY;                    /* arena intatta [T4] */

    db->usato += dim;                            /* ora la coda e' viva */
    return DB_OK;
}

int db_del(db_t *db, const char *key, size_t key_len)
{
    int trovata;
    size_t idx = trova_slot(db, calcolo_hash(key, key_len), key, key_len, &trovata);
    if (!trovata)
        return DB_ERR_MANCA;
    size_t id = (size_t)db->slot[idx].id1 - 1;
    if (db->voci[id].off1 == VOCE_MORTA)
        return DB_ERR_MANCA;                     /* scaduta prima [T5] */
    muore(db, id);                               /* unica morte [I3]  */
    db->slot[idx].id1 = SLOT_TOMBSTONE;
    db->n_tomb++;
    return DB_OK;
}

const uint8_t *db_get(db_t *db, const char *key, size_t key_len,
                      uint32_t *value_len)
{
    int trovata;
    size_t idx = trova_slot(db, calcolo_hash(key, key_len), key, key_len, &trovata);
    if (!trovata)
        return NULL;
    size_t id = (size_t)db->slot[idx].id1 - 1;

    const uint8_t *e = db->data + db->voci[id].off1 - 1;
    uint32_t ttl = leggi_u32(e + 12);
    if (ttl != 0) {
        uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
        if ((uint32_t)time(NULL) - nato > ttl) {
            muore(db, id);                       /* scade qui [I3]     */
            return NULL;
        }
    }
    if (value_len)
        *value_len = leggi_u32(e + 8);
    return e + DIM_HEADER + key_len;
}

/*
 * db_compatta (file 5 + file 7): i byte VIVI si copiano uguali — il CRC
 * viaggia con loro — poi OGNI byte vivo viene riderivato da zero
 * (voce, firma, bucket). La compattazione scandisce l'ARENA, non gli
 * slot: l'arena e' la verita' e i derivati non si meritano fiducia
 * proprio mentre si ricostruiscono [I1].
 */
int db_compatta(db_t *db)
{
    uint8_t *nuovo = malloc(db->capacita);
    if (!nuovo)
        return DB_ERR_MEMORY;

    size_t nu = 0;
    uint32_t adesso = (uint32_t)time(NULL);
    size_t off = 0;
    while (off < db->usato) {
        const uint8_t *e = db->data + off;
        size_t dim = entry_dim(e);
        if (!entry_morta(e)) {
            uint32_t ttl  = leggi_u32(e + 12);
            uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
            if (ttl == 0 || adesso - nato <= ttl) {
                memcpy(nuovo + nu, e, dim);
                nu += dim;
            }
        }
        off += dim;
    }

    free(db->data);
    db->data = nuovo;
    db->usato = nu;
    db->spazzatura = 0;
    db->n_tomb = 0;

    if (deriva_db(db) != DB_OK)
        return DB_ERR_MEMORY;
    return DB_OK;
}

/* ---------------------------------------------------------- persistenza */

/*
 * valida_entry: il validator unico del formato (file 5).
 *   E_VALIDA -> sana e viva; E_SCARTA -> sana ma morta/scaduta, si salta;
 *   E_STOP   -> troncata o CRC sbagliato: da qui non ci si fida piu' [I5]
 */
enum { E_VALIDA = 0, E_SCARTA = 1, E_STOP = -1 };

static int valida_entry(const uint8_t *e, size_t restanti, size_t *dim)
{
    if (restanti < DIM_HEADER)
        return E_STOP;
    uint32_t kl = leggi_u32(e + 4);
    uint32_t vl = leggi_u32(e + 8);
    size_t corpo = restanti - DIM_HEADER;
    if (kl > corpo || vl > corpo - (size_t)kl)
        return E_STOP;

    uint32_t s = crc32_inizio();
    s = crc32_aggiorna(s, e, 20);
    s = crc32_aggiorna(s, e + DIM_HEADER, (size_t)kl + vl);
    if (crc32_fine(s) != leggi_u32(e + 20))
        return E_STOP;

    *dim = DIM_HEADER + (size_t)kl + vl;
    uint32_t ttl  = leggi_u32(e + 12);
    uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
    if (entry_morta(e))
        return E_SCARTA;
    if (ttl != 0 && (uint32_t)time(NULL) - nato > ttl)
        return E_SCARTA;
    return E_VALIDA;
}

int db_salva(db_t *db, const char *nomefile)
{
    if (db->spazzatura > 0 || db->n_tomb > 0)    /* [I4] prima si compatta */
        if (db_compatta(db) != DB_OK)
            return DB_ERR_MEMORY;

    FILE *f = fopen(nomefile, "wb");
    if (!f)
        return DB_ERR_IO;

    uint8_t header[12];
    memcpy(header, MAGIA, 4);
    scrivi_u32(header + 4, FORMATO_VERSIONE);
    scrivi_u32(header + 8, (uint32_t)db->n_entry);

    int r = DB_OK;
    if (fwrite(header, 1, sizeof header, f) != sizeof header ||
        fwrite(db->data, 1, db->usato, f) != db->usato)
        r = DB_ERR_IO;
    if (fclose(f) != 0)
        r = DB_ERR_IO;
    return r;
}

/*
 * db_carica: file 5 per la parte onesta (VALIDA entry per entry, carica
 * il prefisso sano [I5]), file 7 per la parte viva (ogni entry copiata
 * viene DERIVATA: voce + firma + bucket). Le firme NON sono sul disco:
 * il costo della semantica al load e' il tempo delle 32 firme a chiave.
 * ATTENZIONE: vuole una struct VERGINE (come nel file 5: fa memset).
 */
int db_carica(db_t *db, const char *nomefile, size_t *scartate_out)
{
    memset(db, 0, sizeof *db);
    crc_init();

    FILE *f = fopen(nomefile, "rb");
    if (!f)
        return DB_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0)
        { fclose(f); return DB_ERR_IO; }
    long dim_file = ftell(f);
    if (dim_file < (long)(sizeof MAGIA + 8))
        { fclose(f); return DB_ERR_FORMATO; }
    rewind(f);

    uint8_t *raw = malloc((size_t)dim_file);
    if (!raw)
        { fclose(f); return DB_ERR_MEMORY; }
    if (fread(raw, 1, (size_t)dim_file, f) != (size_t)dim_file)
        { free(raw); fclose(f); return DB_ERR_IO; }
    fclose(f);

    if (memcmp(raw, MAGIA, 4) != 0)
        { free(raw); return DB_ERR_FORMATO; }
    if (leggi_u32(raw + 4) != FORMATO_VERSIONE)
        { free(raw); return DB_ERR_FORMATO; }
    uint32_t dichiara = leggi_u32(raw + 8);

    const uint8_t *dati = raw + 12;
    size_t size = (size_t)dim_file - 12;

    /* PASSO 1: valida e conta (niente fiducia a priori) */
    size_t off = 0, vive = 0, scartate = 0, fermati_a = (size_t)-1;
    while (off < size) {
        size_t dim;
        int esito = valida_entry(dati + off, size - off, &dim);
        if (esito == E_STOP) {
            fermati_a = off;
            break;
        }
        if (esito == E_VALIDA)
            vive++;
        else
            scartate++;
        off += dim;
    }
    if (fermati_a != (size_t)-1)
        printf("[carica] validazione ferma a offset %zu (di %zu): "
               "troncata o CRC, ignoro %zu byte di coda [I5]\n",
               fermati_a, size, size - fermati_a);
    if (dichiara != vive)
        printf("[carica] header dichiarava %u, validate %zu: "
               "mi fido della validazione\n", dichiara, vive);

    /* PASSO 2: copia le vive e DERIVA */
    db->capacita = prossima_potenza(off + CAPACITA_MIN);
    db->data = malloc(db->capacita);
    db->n_slot = prossima_potenza(vive * 2);
    if (db->n_slot < SLOT_INIZIALI)
        db->n_slot = SLOT_INIZIALI;
    db->slot = calloc(db->n_slot, sizeof *db->slot);

    db->capacita_voci = prossima_potenza(vive + 1);
    if (db->capacita_voci < VOCI_INIZIALI)
        db->capacita_voci = VOCI_INIZIALI;
    db->voci        = malloc(db->capacita_voci * sizeof *db->voci);
    db->firma       = malloc(db->capacita_voci * N_BANKS * sizeof *db->firma);
    db->gram_arena  = malloc(db->capacita_voci * MAX_GRAMMI * sizeof *db->gram_arena);
    db->visto       = calloc(db->capacita_voci, sizeof *db->visto);

    int fallita_memoria = (!db->data || !db->slot || !db->voci ||
                           !db->firma || !db->gram_arena || !db->visto);
    for (uint32_t b = 0; !fallita_memoria && b < N_BANKS; b++) {
        db->testa[b]    = calloc((size_t)1 << N_BUCKET_BN, sizeof **db->testa);
        db->prossimo[b] = calloc(db->capacita_voci, sizeof **db->prossimo);
        if (!db->testa[b] || !db->prossimo[b])
            fallita_memoria = 1;
    }
    if (fallita_memoria)
        { free(raw); db_libera(db); return DB_ERR_MEMORY; }
    db->marca = 1;

    size_t nu = 0;
    off = 0;
    while (off < size) {
        size_t dim;
        int esito = valida_entry(dati + off, size - off, &dim);
        if (esito == E_STOP)
            break;
        if (esito == E_VALIDA) {
            memcpy(db->data + nu, dati + off, dim);
            if (derivo(db, nu, 0) != DB_OK)
                { free(raw); db_libera(db); return DB_ERR_MEMORY; }
            nu += dim;
        }
        off += dim;
    }

    free(raw);
    db->usato = nu;
    db->n_tomb = 0;
    /* spazzatura NON azzerata: se il file aveva duplicati, derivo li ha
     * sepolti col segno E col conto, e la prossima salva compattera'
     * via le ombre [I4] */
    if (scartate_out)
        *scartate_out = scartate;
    return DB_OK;
}

/* --------------------------------------------------------------- chiedi */

/*
 * db_chiedi — la domanda, con il VALORE stavolta.
 *  1. esatta: confidenza 1.0, valore e chiave dell'entry viva [I6].
 *     (Anche l'esatto controlla la scadenza: la morte e' pigra.)
 *  2. miss: firma della query, 32 bucket, dedupe, ranking per Jaccard
 *     VERA sui candidati VIVI, top-k per inserzione.
 */
size_t db_chiedi(db_t *db, const char *query, size_t q_len, size_t k,
                 risposta_t out[], size_t *n_candidati)
{
    if (k > TOPLIM)
        k = TOPLIM;
    if (k == 0)
        return 0;
    if (n_candidati)
        *n_candidati = 0;

    int trovata;
    size_t idx = trova_slot(db, calcolo_hash(query, q_len), query, q_len, &trovata);
    if (trovata) {
        size_t id = (size_t)db->slot[idx].id1 - 1;
        const uint8_t *e = db->data + db->voci[id].off1 - 1;
        uint32_t ttl = leggi_u32(e + 12);
        uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
        if (ttl != 0 && (uint32_t)time(NULL) - nato > ttl) {
            muore(db, id);                       /* anche qui [I3][T8] */
        } else {
            out[0].chiave     = e + DIM_HEADER;
            out[0].chiave_len = leggi_u32(e + 4);
            out[0].valore     = e + DIM_HEADER + out[0].chiave_len;
            out[0].valore_len = leggi_u32(e + 8);
            out[0].confidenza = 1.0;
            return 1;
        }
    }

    uint32_t grammi[MAX_GRAMMI];
    uint32_t ng = costruisci_grammi(grammi, query, q_len);
    if (ng == 0)
        return 0;

    uint32_t firma[N_BANKS];
    firma_di_grammi(firma, grammi, ng);

    db->marca++;
    static uint32_t cand[MAX_CANDIDATI];
    uint32_t n_cand = 0;
    for (uint32_t b = 0; b < N_BANKS; b++) {
        size_t bh = firma[b] & (((size_t)1 << N_BUCKET_BN) - 1);
        for (uint32_t e = db->testa[b][bh]; e != 0;
             e = db->prossimo[b][e - 1]) {
            uint32_t c = e - 1;
            if (db->visto[c] == db->marca)
                continue;
            db->visto[c] = db->marca;
            if (n_cand < MAX_CANDIDATI)
                cand[n_cand++] = c;
        }
    }
    if (n_candidati)
        *n_candidati = n_cand;
    if (n_cand == 0)
        return 0;

    size_t n_ris = 0;
    for (uint32_t c = 0; c < n_cand; c++) {
        uint32_t id = cand[c];
        if (db->voci[id].off1 == VOCE_MORTA)
            continue;                            /* un morto tace [I6] */
        const uint8_t *e = db->data + db->voci[id].off1 - 1;
        uint32_t ttl = leggi_u32(e + 12);
        uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
        if (ttl != 0 && (uint32_t)time(NULL) - nato > ttl) {
            muore(db, id);                       /* scade al ranking [T8]*/
            continue;
        }
        double j = jaccard(grammi, ng,
                           db->gram_arena + db->voci[id].gram_off,
                           db->voci[id].gram_cnt);
        if (n_ris >= k && j <= out[k - 1].confidenza)
            continue;
        size_t pos = n_ris < k ? n_ris : k - 1;
        if (n_ris < k)
            n_ris++;
        while (pos > 0 && out[pos - 1].confidenza < j) {
            out[pos] = out[pos - 1];
            pos--;
        }
        out[pos].chiave     = e + DIM_HEADER;
        out[pos].chiave_len = leggi_u32(e + 4);
        out[pos].valore     = e + DIM_HEADER + out[pos].chiave_len;
        out[pos].valore_len = leggi_u32(e + 8);
        out[pos].confidenza = j;
    }
    return n_ris;
}

/* ------------------------------------------------------------ benchmark */

enum { CAMPIONI = 100000 };

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void benchmark_get(db_t *db, uint32_t n_chiavi, const char *etichetta)
{
    static double t[CAMPIONI];
    static char kbuf[64];
    double media = 0;
    for (uint32_t i = 0; i < CAMPIONI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", (i * 7919u) % n_chiavi);
        double a = ora_ms();
        db_get(db, kbuf, (size_t)kl, NULL);
        double b = ora_ms();
        t[i] = b - a;
        media += t[i];
    }
    qsort(t, CAMPIONI, sizeof t[0], cmp_double);
    printf("%-24s media %6.3f us   p50 %6.3f   p90 %6.3f   "
           "p99 %6.3f   max %7.3f us\n",
           etichetta,
           media / CAMPIONI * 1000.0,
           t[CAMPIONI / 2] * 1000.0,
           t[CAMPIONI * 9 / 10] * 1000.0,
           t[CAMPIONI * 99 / 100] * 1000.0,
           t[CAMPIONI - 1] * 1000.0);
}

/* ------------------------------------------------------------ test util */

static long dimensione_file(const char *nome)
{
    FILE *f = fopen(nome, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int corrompi_byte(const char *nome, long off)
{
    FILE *f = fopen(nome, "r+b");
    if (!f) return -1;
    if (fseek(f, off, SEEK_SET) != 0) { fclose(f); return -1; }
    int c = fgetc(f);
    if (c == EOF) { fclose(f); return -1; }
    fseek(f, off, SEEK_SET);
    fputc(c ^ 0x5A, f);
    fclose(f);
    return 0;
}

static int tronca_file(const char *nome, long togli)
{
    long n = dimensione_file(nome);
    if (n < togli) return -1;
    FILE *f = fopen(nome, "rb");
    if (!f) return -1;
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        { free(buf); fclose(f); return -1; }
    fclose(f);
    f = fopen(nome, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, (size_t)(n - togli), f);
    fclose(f);
    free(buf);
    return 0;
}

/* refuso di SOSTITUZIONE: una cifra diventa una lettera. Solo questo
 * ha l'univocita' confrontabile: refusi diversi sono capitoli diversi */
static void genera_sostituzione(char *dest, size_t cap, const char *originale,
                                uint32_t errori)
{
    snprintf(dest, cap, "%s", originale);
    size_t l = strlen(dest);
    for (uint32_t e = 0; e < errori; e++) {
        size_t p = (size_t)(prng() % (uint32_t)l);
        if (dest[p] < '0' || dest[p] > '9')
            continue;
        dest[p] = (char)('a' + (prng() % 26));
    }
}

static void genera_lettere(char *dest, size_t cap)
{
    for (size_t i = 0; i < 8 && i < cap - 1; i++)
        dest[i] = (char)('a' + (prng() % 26));
    dest[8] = '\0';
}

static void chiedi_3(db_t *db, const char *query)
{
    risposta_t ris[TOPLIM];
    size_t n_cand;
    size_t n = db_chiedi(db, query, strlen(query), 3, ris, &n_cand);
    (void)n_cand;
    printf("   chiedo \"%s\"\n", query);
    for (size_t r = 0; r < n; r++)
        printf("      %u. \"%.*s\" -> \"%.*s\"  (confidenza %.2f)\n",
               (uint32_t)(r + 1),
               (int)ris[r].chiave_len, (const char *)ris[r].chiave,
               (int)ris[r].valore_len, (const char *)ris[r].valore,
               ris[r].confidenza);
    if (n == 0)
        printf("      (silenzio onesto: nessun vicino sopra il cofano)\n");
}

/* ---------------------------------------------------------------- test */

enum { N_CASI = 500 };

static int esatta_presente(db_t *db, const char *key, const char *valore_atteso)
{
    uint32_t vl;
    const uint8_t *v = db_get(db, key, strlen(key), &vl);
    return v != NULL && (size_t)vl == strlen(valore_atteso) &&
           memcmp(v, valore_atteso, vl) == 0;
}

/*
 * Autotest: il decimo file si guadagna il verde se OGNI invariante e'
 * guardato a vista almeno una volta.
 *  A esatto+valori           [I6]
 *  B refusi PRIMA del save   (il layer vivo funziona)
 *  C stessa query DOPO il reload: le confidenze del top-k SONO LE STESSE
 *    (il file non portava una firma: se la verita' e' ricostruita bene,
 *     le probabilita' tornaNO identiche) [I1]
 *  D alieni: silenzio sopra soglia      [I6 al contrario]
 *  E cancellata: MAI risponde, MAI risorge [I3][I6]
 *  F corruzione 1 byte: si perde SOLO quella [I4][I5]
 *  G troncamento: il prefisso resta vivo   [I5]
 */
static int autotest(void)
{
    int errori = 0;
    char kbuf[32], vbuf[32];
    db_t db;
    if (db_init(&db) != DB_OK)
        return 1;

    enum { N = 3000 };
    for (uint32_t i = 0; i < N; i++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", i);
        snprintf(vbuf, sizeof vbuf, "val%06u", i);
        db_put(&db, kbuf, strlen(kbuf), vbuf, strlen(vbuf), 0);
    }

    /* A: esatte con VALORI */
    for (uint32_t i = 0; i < N; i++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", i);
        snprintf(vbuf, sizeof vbuf, "val%06u", i);
        if (!esatta_presente(&db, kbuf, vbuf)) {
            printf("autotest FALLITO [A]: %s sbagliata o assente\n", kbuf);
            errori++;
            break;
        }
    }
    if (!errori)
        printf("autotest [A]: 3000 chiavi esatte, tutte col valore giusto: OK\n");

    /* B+C: le STESSE query prima e dopo il reload */
    static struct { uint32_t id; char q[16]; } casi[N_CASI];
    for (int t = 0; t < N_CASI; t++) {
        casi[t].id = prng() % N;
        snprintf(kbuf, sizeof kbuf, "k%06u", casi[t].id);
        genera_sostituzione(casi[t].q, sizeof casi[t].q, kbuf, 1);
    }

    uint32_t ripresi_prima = 0;
    for (int t = 0; t < N_CASI; t++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", casi[t].id);
        risposta_t ris[TOPLIM];
        size_t n = db_chiedi(&db, casi[t].q, strlen(casi[t].q), TOPLIM, ris, NULL);
        int ok = 0;
        for (size_t r = 0; r < n; r++)
            if (ris[r].chiave_len == 7 && memcmp(ris[r].chiave, kbuf, 7) == 0)
                ok = 1;
        ripresi_prima += (uint32_t)ok;
    }

    if (db_salva(&db, "prova10.db") != DB_OK) {
        printf("autotest FALLITO: salvataggio\n");
        db_libera(&db);
        return 1;
    }
    db_t ricaricato;
    size_t scartate;
    if (db_carica(&ricaricato, "prova10.db", &scartate) != DB_OK) {
        printf("autotest FALLITO: caricamento\n");
        db_libera(&db);
        return 1;
    }
    if (ricaricato.n_entry != N || scartate != 0) {
        printf("autotest FALLITO: attese %d vive, avute %zu\n", N, ricaricato.n_entry);
        errori++;
    }

    /* A post-reload: i valori sono tornati DAL DISCO */
    for (uint32_t i = 0; i < N; i++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", i);
        snprintf(vbuf, sizeof vbuf, "val%06u", i);
        if (!esatta_presente(&ricaricato, kbuf, vbuf)) {
            printf("autotest FALLITO [C]: %s non sopravvive al round trip\n", kbuf);
            errori++;
            break;
        }
    }

    /* C: stesse query sul RICARICATO stessa confidenza. Le firme NON
     * erano sul disco: se la ricostruzione e' fedele, lo stesso testo
     * firma lo stesso, i candidati sono gli stessi, e la lista delle
     * confidenze e' una multiset identica a quella del layer vivo. */
    uint32_t ripresi_dopo = 0, match_conf = 0, conf_confrontate = 0;
    for (int t = 0; t < N_CASI; t++) {
        risposta_t ra[TOPLIM], rb[TOPLIM];
        size_t na = db_chiedi(&db, casi[t].q, strlen(casi[t].q), TOPLIM, ra, NULL);
        size_t nb = db_chiedi(&ricaricato, casi[t].q, strlen(casi[t].q),
                              TOPLIM, rb, NULL);
        snprintf(kbuf, sizeof kbuf, "k%06u", casi[t].id);
        int ok = 0;
        for (size_t r = 0; r < nb; r++)
            if (rb[r].chiave_len == 7 && memcmp(rb[r].chiave, kbuf, 7) == 0)
                ok = 1;
        ripresi_dopo += (uint32_t)ok;

        if (na != nb) {
            printf("autotest FALLITO [C]: n risposte %zu vs %zu su \"%s\"\n",
                   na, nb, casi[t].q);
            errori++;
            break;
        }
        int pari = 1;
        for (size_t r = 0; r < na; r++)
            if (fabs(ra[r].confidenza - rb[r].confidenza) > 1e-9)
                pari = 0;
        conf_confrontate++;
        match_conf += (uint32_t)pari;
    }
    if (ripresi_prima < 300 || ripresi_dopo < 300 || ripresi_prima != ripresi_dopo) {
        printf("autotest FALLITO [B/C]: refusi ripresi %u/500 prima, %u/500 dopo\n",
               ripresi_prima, ripresi_dopo);
        errori++;
    }
    if (match_conf != conf_confrontate) {
        printf("autotest FALLITO [C]: confidenze diverse su %u/%u query "
               "(la ricostruzione dal disco NON e' la stessa memoria)\n",
               conf_confrontate - match_conf, conf_confrontate);
        errori++;
    }
    if (!errori)
        printf("autotest [B/C]: %u/%d refusi ripresi PRIMA e DOPO il reload, "
               "confidenze identiche byte per byte: il disco ridiva' la stessa "
               "memoria: OK\n", ripresi_prima, N_CASI);
    db_libera(&ricaricato);
    db_libera(&db);
    if (errori)
        return errori;

    /* D+E: alieni e cancellati, sul stesso db piccolo ricaricato */
    {
        db_t live;
        db_init(&live);
        for (uint32_t i = 0; i < N; i++) {
            snprintf(kbuf, sizeof kbuf, "k%06u", i);
            snprintf(vbuf, sizeof vbuf, "val%06u", i);
            db_put(&live, kbuf, strlen(kbuf), vbuf, strlen(vbuf), 0);
        }
        db_del(&live, "k000010", 7);

        uint32_t spurie = 0;
        for (int t = 0; t < N_CASI; t++) {
            char aliena[16];
            genera_lettere(aliena, sizeof aliena);
            risposta_t ris[TOPLIM];
            size_t n = db_chiedi(&live, aliena, strlen(aliena), TOPLIM, ris, NULL);
            if (n > 0 && ris[0].confidenza >= SOGLIA)
                spurie++;
        }
        if (spurie > 10) {
            printf("autotest FALLITO [D]: %u alieni sopra soglia\n", spurie);
            errori++;
        } else {
            printf("autotest [D]: alieni sopra soglia: %u/500: OK\n", spurie);
        }

        /* E: la cancellata non risponde MAI, nemmeno per la via semantica */
        risposta_t ris[TOPLIM];
        size_t n = db_chiedi(&live, "h000010", 7, TOPLIM, ris, NULL);
        int resuscitata = 0;
        for (size_t r = 0; r < n; r++)
            if (ris[r].chiave_len == 7 && memcmp(ris[r].chiave, "k000010", 7) == 0)
                resuscitata = 1;
        if (esatta_presente(&live, "k000010", "val000010") || resuscitata) {
            printf("autotest FALLITO [E]: la cancellata e' risorta\n");
            errori++;
        } else {
            printf("autotest [E]: k000010 cancellata: il get tace e NESSUNA "
                   "risposta semantica la propone: OK\n");
        }
        db_libera(&live);
    }
    if (errori)
        return errori;

    /* F+G: corruzione e troncamento (le regole del file 5 valgono ancora) */
    {
        db_t piccolo;
        db_init(&piccolo);
        enum { M = 2000 };
        for (int i = 0; i < M; i++) {
            char pk[16], pv[16];
            snprintf(pk, sizeof pk, "p%04d", i);
            snprintf(pv, sizeof pv, "vp%04d", i);
            db_put(&piccolo, pk, strlen(pk), pv, strlen(pv), 0);
        }
        for (int i = 0; i < M; i += 4) {
            char pk[16];
            snprintf(pk, sizeof pk, "p%04d", i);
            db_del(&piccolo, pk, strlen(pk));
        }
        if (piccolo.n_entry != M - M / 4) {
            printf("autotest FALLITO [F]: attese %d vive, avute %zu\n",
                   M - M / 4, piccolo.n_entry);
            errori++;
        }

        db_salva(&piccolo, "prova10c.db");
        corrompi_byte("prova10c.db", dimensione_file("prova10c.db") - 10);

        db_t corrotto;
        size_t sc;
        if (db_carica(&corrotto, "prova10c.db", &sc) != DB_OK) {
            printf("autotest FALLITO [F]: caricamento del corrotto\n");
            errori++;
        } else {
            /* l'ultima entry dell'arena compattata e' p1999: era viva,
             * l'abbiamo fatta franca con un byte solo suo, via CRC */
            int perse_solo_quella =
                corrotto.n_entry == (size_t)(M - M / 4 - 1) &&
                db_get(&corrotto, "p1999", 5, NULL) == NULL;
            for (int i = 1; i < M - 1 && perse_solo_quella; i += 2) {
                char pk[16], pv[16];
                snprintf(pk, sizeof pk, "p%04d", i);
                snprintf(pv, sizeof pv, "vp%04d", i);
                if (!esatta_presente(&corrotto, pk, pv))
                    perse_solo_quella = 0;
            }
            if (!perse_solo_quella) {
                printf("autotest FALLITO [F]: corruzione: perse %zu live, "
                       "attese solo l'ultima (p1999)\n", corrotto.n_entry);
                errori++;
            } else {
                printf("autotest [F]: 1 byte corrotto nell'ultima entry: "
                       "caricate %zu su %d, persa SOLO p1999: OK\n",
                       corrotto.n_entry, M - M / 4);
            }
            db_libera(&corrotto);
        }

        /* G: troncamento: il prefisso resta sano */
        db_salva(&piccolo, "prova10t.db");
        tronca_file("prova10t.db", 2000);
        db_t troncato;
        if (db_carica(&troncato, "prova10t.db", &sc) != DB_OK) {
            printf("autotest FALLITO [G]: troncamento non gestibile\n");
            errori++;
        } else {
            if (troncato.n_entry < (size_t)(M - M / 4 - 60) ||
                troncato.n_entry >= (size_t)(M - M / 4)) {
                printf("autotest FALLITO [G]: troncamento: %zu vive\n",
                       troncato.n_entry);
                errori++;
            } else {
                printf("autotest [G]: coda di 2000 byte persa: caricate %zu "
                       "vive, il prefisso intatto: OK\n", troncato.n_entry);
            }
            db_libera(&troncato);
        }
        db_libera(&piccolo);
    }

    remove("prova10.db");
    remove("prova10c.db");
    remove("prova10t.db");
    return errori;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    db_t db;
    if (db_init(&db) != DB_OK) {
        fprintf(stderr, "errore di allocazione\n");
        return 1;
    }

    static char kbuf[40], vbuf[64];
    enum { N_CHIAVI = 60000 };

    /* ---------- riempimento con VALORI ---------- */
    printf("riempio l'arena: %d chiavi \"uNNNNNNN\" coi loro valori...\n",
           N_CHIAVI);
    double t0 = ora_ms();
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-di-%u", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    printf("pieno: %d chiavi in %.0f ms (firma e bucket pagati a ogni put)\n",
           N_CHIAVI, ora_ms() - t0);
    statistiche(&db, "dopo carico");

    /* il passato sporco: sovrascrivo le pari, cancello le seste */
    for (uint32_t i = 0; i < N_CHIAVI; i += 2) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-nuovo-di-%u", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    for (uint32_t i = 0; i < N_CHIAVI; i += 6) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", i);
        db_del(&db, kbuf, (size_t)kl);
    }
    statistiche(&db, "dopo upd+del");

    /* ---------- salva: compatta, poi scrive [I4] ---------- */
    t0 = ora_ms();
    if (db_salva(&db, "demo10.db") != DB_OK) {
        fprintf(stderr, "salvataggio fallito\n");
        return 1;
    }
    printf("\ndb_salva: %ld byte in %.0f ms (compattazione inclusa: sul "
           "file SOLO entry vive, il bit MORTA non esiste su disco)\n",
           dimensione_file("demo10.db"), ora_ms() - t0);
    statistiche(&db, "dopo salva");

    /* ---------- carica: prefisso + ricostruzione LSH ---------- */
    db_t nuovo;
    size_t scartate;
    t0 = ora_ms();
    if (db_carica(&nuovo, "demo10.db", &scartate) != DB_OK) {
        fprintf(stderr, "caricamento fallito\n");
        return 1;
    }
    double t_carica = ora_ms() - t0;
    statistiche(&nuovo, "ricaricato");
    printf("db_carica: %.0f ms — di cui il prezzo della semantica: ogni "
           "chiave viva rifirma 32 bande da zero (le firme NON erano sul "
           "disco)\n\n", t_carica);

    /* onesta' dei valori: il pari aggiornato, il sesto cancellato */
    uint32_t vl;
    const uint8_t *v;
    v = db_get(&nuovo, "u0000002", 8, &vl);
    printf("verifica dal disco: u0000002 -> \"%.*s\" (atteso "
           "\"valore-nuovo-di-2\")\n",
           v ? (int)vl : 6, v ? (const char *)v : "(null)");
    v = db_get(&nuovo, "u0000006", 8, &vl);
    printf("verifica dal disco: u0000006 -> %s (cancellata prima del save: "
           "il disco NON la risente)\n",
           v ? "TROVATA?!" : "NULL, corretto");

    uint32_t sbagliate = 0;
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        uint32_t id = (i * 7919u) % N_CHIAVI;
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", id);
        if (id % 6 == 0) {
            if (db_get(&nuovo, kbuf, (size_t)kl, NULL) != NULL)
                sbagliate++;                 /* la cancellata risorta */
        } else {
            int vl2 = snprintf(vbuf, sizeof vbuf,
                               id % 2 == 0 ? "valore-nuovo-di-%u" : "valore-di-%u",
                               id);
            uint32_t dl;
            const uint8_t *g = db_get(&nuovo, kbuf, (size_t)kl, &dl);
            if (!g || dl != (uint32_t)vl2 || memcmp(g, vbuf, dl) != 0)
                sbagliate++;
        }
    }
    printf("verifica completa dal disco: %u sbagliate su %d campioni "
           "(valori, versioni e cancellazioni compresi)\n\n",
           sbagliate, N_CHIAVI);
    if (sbagliate != 0) {
        fprintf(stderr, "FALLITA la verifica dal disco: %u sbagliate su %d\n",
                sbagliate, N_CHIAVI);
        return 1;
    }

    benchmark_get(&nuovo, N_CHIAVI, "esatte sul ricaricato:");

    /* ---------- LA DOMANDA AL RICARICATO ---------- */
    enum { N_QUERY = 10000 };
    static double lat[N_QUERY];
    risposta_t ris[TOPLIM];
    static char qs[N_QUERY][16];
    static char veri[N_QUERY][16];
    uint32_t top1 = 0, topk = 0, dichiarate = 0;
    double somma_s = 0, somma_conf = 0, somma_pari = 0;
    size_t somma_cand = 0;

    for (int t = 0; t < N_QUERY; t++) {
        uint32_t id = prng() % N_CHIAVI;
        if (id % 6 == 0)
            id = (id + 1) % N_CHIAVI;            /* le seste sono morte */
        snprintf(veri[t], sizeof veri[t], "u%07u", id);
        genera_sostituzione(qs[t], sizeof qs[t], veri[t], 1);
    }

    printf("== %d query con UN refuso di sostituzione, sul DB RICARICATO "
           "DAL DISCO ==\n", N_QUERY);
    for (int t = 0; t < N_QUERY; t++) {
        uint32_t gq[MAX_GRAMMI], gv[MAX_GRAMMI];
        uint32_t nq = costruisci_grammi(gq, qs[t], strlen(qs[t]));
        uint32_t ng = costruisci_grammi(gv, veri[t], strlen(veri[t]));
        somma_s += jaccard(gq, nq, gv, ng);

        size_t n_cand;
        double a = ora_ms();
        size_t n = db_chiedi(&nuovo, qs[t], strlen(qs[t]), TOPLIM, ris, &n_cand);
        lat[t] = ora_ms() - a;
        somma_cand += n_cand;

        int vv1 = n > 0 && ris[0].chiave_len == strlen(veri[t]) &&
                  memcmp(ris[0].chiave, veri[t], ris[0].chiave_len) == 0;
        int vvk = vv1;
        for (size_t r = 1; r < n; r++)
            if (ris[r].chiave_len == strlen(veri[t]) &&
                memcmp(ris[r].chiave, veri[t], ris[r].chiave_len) == 0)
                vvk = 1;
        topk += (uint32_t)vvk;
        top1 += (uint32_t)vv1;

        /* il gruppo dei pari-merito: il refuso NASCONDE la cifra, il
         * db che dichiara il pareggio non sta barando */
        size_t pari = 0;
        for (size_t r = 0; r < n; r++)
            if (ris[r].confidenza >= ris[0].confidenza - 1e-9)
                pari++;
        somma_pari += (double)pari;

        if (n > 0 && ris[0].confidenza >= SOGLIA) {
            dichiarate++;
            somma_conf += ris[0].confidenza;
        }
    }
    qsort(lat, N_QUERY, sizeof lat[0], cmp_double);

    double s_medio = somma_s / N_QUERY;
    double p_teo = 1.0 - pow(1.0 - pow(s_medio, N_RIGHE), N_BANKS);
    printf("   verita' al 1o posto: %4.1f%%   verita' nel top-%d: %4.1f%%\n",
           100.0 * top1 / N_QUERY, TOPLIM, 100.0 * topk / N_QUERY);
    printf("   candidati medi per query: %zu\n", somma_cand / N_QUERY);
    printf("   gruppo dei pari-merito del top: %.1f chiavi in media "
           "(il db dichiara il pareggio: l'ambiguita' e' del refuso)\n",
           somma_pari / N_QUERY);
    printf("   risposte DICHIARATE (confidenza >= %.2f): %4.1f%%, "
           "confidenza media %.2f\n", SOGLIA,
           100.0 * dichiarate / N_QUERY,
           dichiarate ? somma_conf / dichiarate : 0.0);
    printf("   latenza: p50 %.3f ms   p99 %.3f ms\n",
           lat[N_QUERY / 2], lat[N_QUERY * 99 / 100]);
    printf("   teoria del banding: s medio %.2f -> P(cattura) = "
           "1-(1-s^%d)^%d = %.1f%%\n",
           s_medio, N_RIGHE, N_BANKS, 100.0 * p_teo);
    printf("   (confronto col file 7: 24 bande; qui 32 -> cattura piu' "
           "larga, piu' candidati da rankare)\n\n");

    /* ---------- alieni: il silenzio e' una risposta ---------- */
    {
        enum { N_ALIENI = 10000 };
        uint32_t spurie = 0;
        double conf_max = 0;
        for (int t = 0; t < N_ALIENI; t++) {
            char aliena[16];
            genera_lettere(aliena, sizeof aliena);
            size_t n = db_chiedi(&nuovo, aliena, 8, TOPLIM, ris, NULL);
            if (n > 0) {
                if (ris[0].confidenza > conf_max)
                    conf_max = ris[0].confidenza;
                if (ris[0].confidenza >= SOGLIA)
                    spurie++;
            }
        }
        printf("== %d query aliene (8 lettere a caso) sul ricaricato ==\n",
               N_ALIENI);
        printf("   spurie sopra soglia: %u (%.2f%%), confidenza massima "
               "vista: %.2f\n\n", spurie, 100.0 * spurie / N_ALIENI, conf_max);
    }

    /* ---------- esempi a occhio: chiave E valore dal disco ---------- */
    printf("== esempi a occhio (risposte col valore) ==\n");
    chiedi_3(&nuovo, "u000000r");        /* refuso di cifra          */
    chiedi_3(&nuovo, "w0059999");        /* refuso di prefisso: rispondere e' dovere */
    chiedi_3(&nuovo, "abcdefgh");        /* aliena: silenzio?       */
    printf("\n");

    printf("== il caso AMBIGUO per costruzione ==\n");
    {
        risposta_t r8[TOPLIM];
        size_t n = db_chiedi(&nuovo, "u000500r0", 9, TOPLIM, r8, NULL);
        printf("   chiedo \"u000500r0\"\n");
        for (size_t r = 0; r < n; r++)
            printf("      %u. \"%.*s\" -> \"%.*s\"  (confidenza %.2f)\n",
                   (uint32_t)(r + 1),
                   (int)r8[r].chiave_len, (const char *)r8[r].chiave,
                   (int)r8[r].valore_len, (const char *)r8[r].valore,
                   r8[r].confidenza);
        printf("   tutti a pari merito: il db dichiara il pareggio, "
               "non finge di sapere\n\n");
    }

    printf("== il momento della verita' sugli invarianti ==\n");
    if (autotest() != 0) {
        fprintf(stderr, "AUTOTEST FALLITO\n");
        return 1;
    }

    remove("demo10.db");
    db_libera(&nuovo);
    db_libera(&db);
    printf("\nverde. dieci file: il cerchio e' chiuso — la memoria "
           "probabilistica ora abita nel disco.\n");
    return 0;
}
