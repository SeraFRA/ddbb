/*
 * database_compact.c — quarta iterazione del progetto.
 *
 * =====================================================================
 * NOVITA' RISPETTO A database_indexed.c
 * =====================================================================
 *
 *  1. DELETE vero, con TOMBSTONE.
 *     Problem classico: in una hash table con probing lineare, svuotare
 *     lo slot di una chiave cancellata rompe le catene. Se A, B e C
 *     hanno colliso e sono finiti agli slot 10, 11, 12, cancellare B e
 *     rendere "vuoto" l'11 fa fermare la ricerca di C a meta'.
 *
 *     Soluzione: tre stati per slot
 *       off1 == 0             slot VUOTO (fine catena: la chiave non c'e')
 *       off1 == OFF1_TOMBSTONE slot cancellato (TOMBSTONE)
 *       altrimenti             slot occupato -> offset entry + 1
 *
 *     La ricerca NON si ferma su un tombstone: lo annota come possibile
 *     punto di inserimento e continua fino a uno slot VUOTO. Solo il
 *     vuoto certifica l'assenza della chiave. (vantaggio doppio: il
 *     put successivo riusa lo slot tombstone).
 *
 *  2. COMPATTAZIONE: recupera la spazzatura del buffer.
 *     Una passata sola, O(n):
 *       a. per ogni slot occupato, copia l'entry VIVA in un buffer nuovo
 *          (le morte, cancellate e scadute vengono eliminate)
 *       b. azzera e RICOSTRUISCE l'indice dalle entry copiate.
 *     Il punto sottile: NON si ricalcola nessun hash per ricostruire,
 *     perche' l'hash e' gia' scritto nell'header dell'entry. E non ci
 *     sono tombstone da rispettare: l'indice viene rigenerato da zero,
 *     quindi le catene tornano perfette. Per questo i tombstone si
 *     possono eliminare SOLO durante una ricostruzione completa.
 *
 *  3. CONTABILITA' DELLA SPAZZATURA, corretta.
 *     Invariante: ogni entry viene contata UNA volta, quando smette di
     * essere viva. Le tre morti possibili: overwrite (upsert), delete,
 *     scadenza scoperta da un get. Da quel momento l'entry porta il
 *     bit MORTA (bit 31 del campo creazione: un trucco valido finche'
 *     il tempo restera' sotto il 2038 -- esattamente il motivo per cui
 *     i sistemi reali usano timestamp a 64 bit) e il bit impedisce il
 *     riconteggio.
 *
 *  4. Timer ad alta risoluzione (QueryPerformanceCounter / clock_gettime):
 *     clock() ha ~15 ms di risoluzione su Windows e mediava i microsecondi
 *     in modo poco credibile.
 *
 *  5. AUTOTEST: il vero test della correttezza di tombstone e
 *     compattazione e' mescolare put, delete e get e verificare che
 *     TUTTE le chiavi sopravvissute restino trovabili -- prima e dopo
 *     la compattazione.
 *
 * =====================================================================
 * LIMITI NOTI (esercizi successivi)
 * =====================================================================
 *   - persistenza su disco con CRC32 per entry e numero di versione
 *   - il growth check della hash table conta anche i tombstone (devono
 *     occupare slot fino alla prossima ricostruzione)
 *   - percentili p50/p99 del tempo di get
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wpedantic"
# include <windows.h>
# pragma GCC diagnostic pop
#else
# include <unistd.h>
#endif

#define DIM_HEADER   20u
#define SLOT_INIZIALI ((size_t)1 << 10)
#define LOAD_FACTOR_10 7

/* stati di slot.off1 */
#define OFF1_VUOTO     0u
#define OFF1_TOMBSTONE 0xFFFFFFFFu

/* bit "entry morta" rubato al campo creazione */
#define MORTA_BIT 0x80000000u

enum {
    DB_OK          =  0,
    DB_ERR_MEMORY  = -1,
    DB_ERR_PIENO   = -2,
    DB_ERR_MANCA   = -3
};

typedef struct {
    uint32_t hash;    /* hash chiave: confronto rapido anti-collisione      */
    uint32_t off1;    /* VUOTO / TOMBSTONE / entry offset + 1              */
} slot_t;

typedef struct {
    uint8_t *data;
    size_t   capacita;
    size_t   usato;
    size_t   spazzatura;   /* byte morti riconosciuti                       */
    size_t   spazzatura_stimata_extra;  /* stimata: scadute mai lette       */
    slot_t  *slot;
    size_t   n_slot;        /* potenza di 2                                 */
    size_t   n_entry;       /* chiavi vive                                   */
    size_t   n_tomb;        /* tombstone                                     */
} db_t;

/* ------------------------------------------------ struttura entry binaria
 * [ 0..4) hash  [4..8) key_len  [8..12) value_len  [12..16) ttl
 * [16..20) creazione (bit 31 = MORTA)  [20..] chiave, valore
 */

static uint32_t leggi_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static void    scrivi_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }

static uint32_t calcolo_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)(uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

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

/* --------------------------------------------------------------- timer */

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

static void pausa(long ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    sleep(ms / 1000);
#endif
}

/* --------------------------------------------------------------- indice */

/*
 * trova_slot — la ricerca canonica con tombstone.
 * Ritorna lo slot dove:
 *   *trovata == 1 : contiene la chiave
 *   *trovata == 0 : la chiave non c'e'; il valore di ritorno e' dove
 *                   inserirla (uno slot tombstone riusato, se visto,
 *                   altrimenti il primo vuoto)
 * REGOLA: il tombstone NON ferma la ricerca — la chiave potrebbe essere
 * oltre, nella stessa catena. Solo lo slot VUOTO chiude la catena.
 */
static size_t trova_slot(const db_t *db, uint32_t hash,
                         const char *key, size_t key_len, int *trovata)
{
    size_t idx = hash & (db->n_slot - 1);
    size_t tomb = (size_t)-1;
    *trovata = 0;

    for (;;) {
        uint32_t off1 = db->slot[idx].off1;

        if (off1 == OFF1_VUOTO)
            return tomb == (size_t)-1 ? idx : tomb;

        if (off1 == OFF1_TOMBSTONE) {
            if (tomb == (size_t)-1)
                tomb = idx;                     /* candidata al riuso */
        } else if (db->slot[idx].hash == hash) {
            const uint8_t *e = db->data + (off1 - 1);
            uint32_t kl = leggi_u32(e + 4);
            if (kl == key_len &&
                memcmp(e + DIM_HEADER, key, key_len) == 0) {
                *trovata = 1;
                return idx;
            }
        }
        idx = (idx + 1) & (db->n_slot - 1);
    }
}

/*
 * cresci_indice — raddoppia la tabella.
 * Durante il rehash i TOMBSTONE VENGONO SCARTATI: ogni catena viene
 * ricostruita da zero, quindi un tombstone non protegge piu' niente.
     (il growth, cosi', e' anche una mezza pulizia della tabella —
 * ma non del buffer, che resta compito della compattazione)
 */
static int cresci_indice(db_t *db)
{
    size_t nuovo_n = db->n_slot * 2;
    slot_t *nuovo = calloc(nuovo_n, sizeof *nuovo);
    if (!nuovo)
        return DB_ERR_MEMORY;

    for (size_t v = 0; v < db->n_slot; v++) {
        /* BUG FIX (trovato dall'autotest): la condizione era
         * "off1 <= OFF1_TOMBSTONE", ma il tombstone e' 0xFFFFFFFF, il
         * MASSIMO unsigned: la disequazione era vera per OGNI slot e
         * ogni raddoppio copiava zero entry, cancellando l'indice.
         * I sentinel si confrontano per uguaglianza, mai con < o >. */
        if (db->slot[v].off1 == OFF1_VUOTO ||
            db->slot[v].off1 == OFF1_TOMBSTONE)
            continue;
        size_t idx = db->slot[v].hash & (nuovo_n - 1);
        while (nuovo[idx].off1 != OFF1_VUOTO)
            idx = (idx + 1) & (nuovo_n - 1);
        nuovo[idx] = db->slot[v];
    }

    free(db->slot);
    db->slot  = nuovo;
    db->n_slot = nuovo_n;
    db->n_tomb = 0;
    printf("[indice] tabella raddoppiata: %zu slot\n", nuovo_n);
    return DB_OK;
}

/* ------------------------------------------------------------ database */

int db_init(db_t *db, size_t capacita)
{
    db->data = malloc(capacita);
    db->slot = calloc(SLOT_INIZIALI, sizeof *db->slot);
    if (!db->data || !db->slot) {
        free(db->data);
        free(db->slot);
        return DB_ERR_MEMORY;
    }
    db->capacita  = capacita;
    db->usato     = 0;
    db->spazzatura = 0;
    db->spazzatura_stimata_extra = 0;
    db->n_slot    = SLOT_INIZIALI;
    db->n_entry   = 0;
    db->n_tomb    = 0;
    return DB_OK;
}

void db_libera(db_t *db)
{
    free(db->data);
    free(db->slot);
    db->data = NULL;
    db->slot = NULL;
}

static void statistiche(const db_t *db, const char *quando)
{
    printf("%-14s %6zu byte usati, %6zu di spazzatura (%4.1f%%), "
           "%5zu vive, %4zu tombstone, %6zu slot (LF %2.0f%%)\n",
           quando, db->usato, db->spazzatura,
           db->usato ? 100.0 * (double)db->spazzatura / (double)db->usato : 0.0,
           db->n_entry, db->n_tomb, db->n_slot,
           100.0 * (double)(db->n_entry + db->n_tomb) / (double)db->n_slot);
}

int db_put(db_t *db, const char *key, size_t key_len,
           const char *valore, size_t val_len, uint32_t ttl)
{
    size_t dim = DIM_HEADER + key_len + val_len;
    if (dim > db->capacita - db->usato)
        return DB_ERR_PIENO;

    /* crescita: contano anche i tombstone, occupano slot reali */
    if ((db->n_entry + db->n_tomb + 1) * 10 > db->n_slot * LOAD_FACTOR_10)
        if (cresci_indice(db) != DB_OK)
            return DB_ERR_MEMORY;

    uint32_t hash = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, hash, key, key_len, &trovata);

    if (trovata) {
        /* overwrite: l'entry vecchia muore adesso. Se era GIA' morta, la
         * sua spazzatura e' stata contata quando e' morta: l'invariante
         * e' "conteggio una volta sola", garantito dal bit MORTA */
        size_t off_vecchia = db->slot[idx].off1 - 1;
        if (!entry_morta(db->data + off_vecchia))
            db->spazzatura += entry_dim(db->data + off_vecchia);
    } else {
        if (db->slot[idx].off1 == OFF1_TOMBSTONE)
            db->n_tomb--;    /* tombstone riusato */
        db->n_entry++;
    }

    size_t off = db->usato;
    uint8_t *dest = db->data + off;
    scrivi_u32(dest +  0, hash);
    scrivi_u32(dest +  4, (uint32_t)key_len);
    scrivi_u32(dest +  8, (uint32_t)val_len);
    scrivi_u32(dest + 12, ttl);
    scrivi_u32(dest + 16, (uint32_t)time(NULL));
    memcpy(dest + DIM_HEADER, key, key_len);
    memcpy(dest + DIM_HEADER + key_len, valore, val_len);
    db->usato += dim;

    db->slot[idx].hash = hash;
    db->slot[idx].off1 = (uint32_t)(off + 1);
    return DB_OK;
}

int db_del(db_t *db, const char *key, size_t key_len)
{
    uint32_t hash = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, hash, key, key_len, &trovata);

    if (!trovata)
        return DB_ERR_MANCA;

    size_t off = db->slot[idx].off1 - 1;
    segna_morta(db->data, off);
    db->spazzatura += entry_dim(db->data + off);

    /* tombstone: lo slot resta PERCORSO dalla ricerca, soltanto non
     * contenitore della chiave. svuotarlo (OFF1_VUOTO) romperebbe
     * le catene delle chiavi che da qui son passate */
    db->slot[idx].off1 = OFF1_TOMBSTONE;
    db->n_tomb++;
    db->n_entry--;
    return DB_OK;
}

const uint8_t *db_get(db_t *db, const char *key, size_t key_len,
                      uint32_t *value_len)
{
    uint32_t hash = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, hash, key, key_len, &trovata);

    if (!trovata)
        return NULL;

    const uint8_t *e = db->data + (db->slot[idx].off1 - 1);

    if (entry_morta(e))
        return NULL;   /* cancellata: gia' contata */

    uint32_t ttl = leggi_u32(e + 12);
    if (ttl != 0) {
        uint32_t nato   = leggi_u32(e + 16) & ~MORTA_BIT;
        if ((uint32_t)time(NULL) - nato > ttl) {
            /* scaduta: muore ADESSO -> conteggio UNA volta, bit MORTA
             * e le prossime get la vedranno morta senza ricontarla */
            segna_morta(db->data, db->slot[idx].off1 - 1);
            db->spazzatura += entry_dim(e);
            return NULL;
        }
    }

    if (value_len)
        *value_len = leggi_u32(e + 8);
    return e + DIM_HEADER + key_len;
}

/*
 * db_compatta — O(n), recupera TUTTA la spazzatura:
 *   1. copia in un buffer nuovo solo le entry vive e non scadute
 *   2. azzera l'indice e lo RICOSTRUISCE dalle entry copiate:
 *      l'hash sta nell'header -> zero ricalcoli; le chiavi sono
 *      uniche per costruzione -> zero confronti di chiave
 * Effetto collaterale positivo: i tombstone spariscono (n_tomb = 0)
 * e le catene di probing tornano corte come a freddo.
 */
int db_compatta(db_t *db)
{
    uint8_t *nuovo = malloc(db->capacita);
    if (!nuovo)
        return DB_ERR_MEMORY;

    size_t nu = 0;
    size_t vive = 0;
    uint32_t adesso = (uint32_t)time(NULL);
    size_t scadute_recuperate = 0;

    for (size_t v = 0; v < db->n_slot; v++) {
        uint32_t off1 = db->slot[v].off1;
        if (off1 == OFF1_VUOTO || off1 == OFF1_TOMBSTONE)
            continue;

        const uint8_t *e = db->data + (off1 - 1);
        if (entry_morta(e))
            continue;                       /* cancellata: eliminata */

        uint32_t ttl  = leggi_u32(e + 12);
        uint32_t nato = leggi_u32(e + 16);
        if (ttl != 0 && adesso - nato > ttl) {
            scadute_recuperate++;
            continue;                       /* scaduta: eliminata */
        }

        memcpy(nuovo + nu, e, entry_dim(e));
        nu += entry_dim(e);
        vive++;
    }

    free(db->data);
    db->data = nuovo;
    db->usato = nu;
    db->spazzatura = 0;
    db->spazzatura_stimata_extra = 0;
    db->n_entry = vive;
    db->n_tomb = 0;

    /* ricostruzione indice: solo probing su slot vuoti */
    memset(db->slot, 0, db->n_slot * sizeof *db->slot);
    size_t off = 0;
    while (off < nu) {
        const uint8_t *e = nuovo + off;
        uint32_t h = leggi_u32(e);
        size_t idx = h & (db->n_slot - 1);
        while (db->slot[idx].off1 != OFF1_VUOTO)
            idx = (idx + 1) & (db->n_slot - 1);
        db->slot[idx].hash = h;
        db->slot[idx].off1 = (uint32_t)(off + 1);
        off += entry_dim(e);
    }

    if (scadute_recuperate > 0)
        printf("[compatta] %zu entry scadute eliminate col l'occasione\n",
               scadute_recuperate);
    return DB_OK;
}

/* -------------------------------------------------------------- autotest */

/*
 * La prova di correttezza vera: mescola put/del/get e verifica che
 * ogni chiave NON cancellata resti trovabile con il valore giusto, e
 * ogni cancellata resti NULL — prima e dopo la compattazione. Se un
 * tombstone rompesse una catena di probing, questo test lo scopre.
 */
static int autotest(void)
{
    db_t db;
    if (db_init(&db, 1024 * 1024) != DB_OK)
        return 1;

    char kbuf[32], vbuf[32];
    enum { N = 2000 };

    for (int i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "test:%04d", i);
        int vl = snprintf(vbuf, sizeof vbuf, "val:%d", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    for (int i = 0; i < N; i += 3) {              /* cancella un terzo */
        int kl = snprintf(kbuf, sizeof kbuf, "test:%04d", i);
        if (db_del(&db, kbuf, (size_t)kl) != DB_OK) {
            printf("autotest FALLITO: del #%d non cancella\n", i);
            return 1;
        }
    }

    int errori = 0;
    for (int fase = 0; fase < 2; fase++) {          /* prima e dopo compact */
        if (fase == 1)
            db_compatta(&db);
        for (int i = 0; i < N; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "test:%04d", i);
            int vl = snprintf(vbuf, sizeof vbuf, "val:%d", i);
            uint32_t dl;
            const uint8_t *r = db_get(&db, kbuf, (size_t)kl, &dl);
            if (i % 3 == 0) {
                if (r != NULL) {
                    printf("autotest FALLITO: %s resuscitata (fase %d)\n", kbuf, fase);
                    errori++;
                }
            } else if (r == NULL || dl != (uint32_t)vl ||
                       memcmp(r, vbuf, dl) != 0) {
                printf("autotest FALLITO: %s persa (fase %d)\n", kbuf, fase);
                errori++;
            }
        }
    }
    if (errori == 0)
        printf("autotest: 2000 chiavi, 667 cancellate, verificate PRIMA e "
               "DOPO compattazione: OK\n");
    db_libera(&db);
    return errori;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    db_t db;
    if (db_init(&db, 4u * 1024 * 1024) != DB_OK) {
        fprintf(stderr, "errore di allocazione\n");
        return 1;
    }

    static char kbuf[64], vbuf[64];
    enum { N_CHIAVI = 30000, N_GET = 100000 };

    /* --- riempimento --- */
    double t0 = ora_ms();
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-di-%u", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    printf("pieno:   %d chiavi in %.1f ms\n", N_CHIAVI, ora_ms() - t0);
    statistiche(&db, "dopo carico");

    /* --- overwrite della meta': spazzatura strutturale --- */
    for (uint32_t i = 0; i < N_CHIAVI; i += 2) {
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-nuovo-lungo-di-%u", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    /* --- delete di un sesto: tombstone --- */
    for (uint32_t i = 0; i < N_CHIAVI; i += 6) {
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", i);
        db_del(&db, kbuf, (size_t)kl);
    }
    statistiche(&db, "dopo upd+del");

    /* --- TTL demo: scade prima della compattazione --- */
    db_put(&db, "sessione:demo", 13, "abc123", 6, 1);      /* ttl 1 secondo */

    /* --- get prima della compattazione --- */
    double somma_ms = 0;
    for (int round = 0; round < 3; round++) {
        t0 = ora_ms();
        size_t zie = 0;
        for (uint32_t i = 0; i < N_GET; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", (i * 7919) % N_CHIAVI);
            uint32_t dl;
            if (db_get(&db, kbuf, (size_t)kl, &dl))
                zie += dl;         /* risultato usato: niente dead-code elim */
        }
        double t = ora_ms() - t0;
        somma_ms += t;
        (void)zie;
    }
    printf("get con spazzatura:  %d in %.2f ms (%.3f us/get)\n",
           N_GET, somma_ms / 3.0, somma_ms / 3.0 * 1000.0 / N_GET);

    /* --- scade la sessione, compatta --- */
    pausa(1500);
    t0 = ora_ms();
    db_compatta(&db);
    printf("compattazione: %.1f ms\n", ora_ms() - t0);
    statistiche(&db, "dopo compatta");

    /* --- get dopo la compattazione: stesso ordine, indici ripuntati --- */
    somma_ms = 0;
    for (int round = 0; round < 3; round++) {
        t0 = ora_ms();
        size_t zie = 0;
        for (uint32_t i = 0; i < N_GET; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", (i * 7919) % N_CHIAVI);
            uint32_t dl;
            if (db_get(&db, kbuf, (size_t)kl, &dl))
                zie += dl;
        }
        double t = ora_ms() - t0;
        somma_ms += t;
        (void)zie;
    }
    printf("get dopo compatta:   %d in %.2f ms (%.3f us/get)\n",
           N_GET, somma_ms / 3.0, somma_ms / 3.0 * 1000.0 / N_GET);

    /* --- verifiche di senso compiuto --- */
    uint32_t dl;
    const uint8_t *v = db_get(&db, "utente:10", 9, &dl);
    printf("get(\"utente:10\") -> \"%.*s\"\n", v ? (int)dl : 6,
           v ? (const char *)v : "(null)");
    v = db_get(&db, "utente:6", 8, &dl);   /* era tra le cancellate */
    printf("get(\"utente:6\")  -> %s (cancellata, e resta tale dopo compact)\n",
           v ? "TROVATA?!" : "NULL");
    v = db_get(&db, "sessione:demo", 13, &dl);
    printf("get(\"sessione:demo\") -> %s (scaduta: eliminata dalla compattazione)\n",
           v ? "TROVATA?!" : "NULL");

    printf("\n");
    if (autotest() != 0)
        return 1;

    db_libera(&db);
    return 0;
}
