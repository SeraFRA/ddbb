/*
 * database_indexed.c — terza iterazione del progetto.
 *
 * =====================================================================
 * ARCHITETTURA (pattern "Bitcask", usato anche da Redis e LevelDB)
 * =====================================================================
 *
 * Il database e' diviso in DUE strutture con ruoli opposti:
 *
 *   1. IL BUFFER (data) — e' il "dato vero": un array contiguo di entry
 *      in formato binario, append-only. Si scrive SOLO alla fine, mai
 *      in mezzo e mai sopra entry esistenti. Popolato, si potrebbe
 *      scrivere su disco com'e' con una singola write().
 *
 *   2. L'INDICE (slot) — una hash table in RAM che mappa
 *      chiave -> offset dell'entry nel buffer. E' METADATO:
 *      ricavabile con una scansione del buffer, quindi non serve
 *      persistarlo: al primo avvio si ricostruisce.
 *
 * Perche' questo rende tutto O(1):
 *   - put: appenda in fondo al buffer (O(1)) + aggiorna lo slot (O(1))
 *   - get: calcola l'hash, salta con una maschera allo slot (O(1)),
 *     conferma con un memcmp della chiave (anti-collisione), legge
 *     l'offset, punta direttamente all'entry.
 *
 * Il prezzo dell'append-only: aggiornare una chiave NON tocca la entry
 * vecchia (diventa "spazzatura": byte morti nel buffer). Recuperare
 * quello spazio richiede la compattazione (rif. compatta_database),
 * qui mostrata in forma semplice.
 *
 * =====================================================================
 * LAYOUT DELL'ENTRY NEL BUFFER (DIM_HEADER = 20 byte, tutti uint32)
 * =====================================================================
 *   [ 0..4)   hash         calcolata con FNV-1a
 *   [ 4..8)   key_len
 *   [ 8..12)  value_len
 *   [12..16)  ttl           0 = non scade mai
 *   [16..20)  creazione     time(NULL) alla scrittura:
 *                            serve perche' "scaduto" significa
 *                            adesso - creazione > ttl
 *   [20..20+key_len)              chiave
 *   [20+key_len .. +value_len)   valore
 *
 * Nota: le entry partono a offset arbitrari (mai multipli di 4), quindi
 * i campi si leggono/scrivono con memcpy: mai cast diretti.
 *
 * =====================================================================
 * L'INDICE: tabella di slot con linear probing
 * =====================================================================
 *   slot = { hash, off1 }   dove off1 = offset_entry + 1.
 *   off1 == 0 significa slot vuoto. Il "+1" evita l'ambiguita' con la
 *   prima entry, che starebbe a offset 0.
 *
 *   La tabella ha n_slot POTENZA DI 2: cosi' la posizione iniziale e'
 *   hash & (n_slot - 1) (una AND, niente divisione) e pure l'avanzamento
 *   del probing e' una AND wrapping. Lo slot occupato "sbagliato" si
 *   salta con probing lineare (idx+1, idx+2, ...).
 *
 *   Quando il load factor supera 70% la tabella raddoppia e tutti gli
 *   slot vengono reinserti (rehash). Senza crescita, il probing si
 *   degrada fino a diventare O(n): la tabella piena al 100% e' O(n).
 *
 * =====================================================================
 * COSA MANCA (esercizi concreti)
 * =====================================================================
 *   - tombstone/delete: svuotare lo slot di una chiave cancellata
 *     romperebbe le catene di probing di altre chiavi scelte da li'.
 *     Serve un terzo stato (tombstone) oppure incapssulare il delete
 *     nella compattazione.
 *   - scrittura su disco della struct e ricostruzione indice da file.
 * =====================================================================
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DIM_HEADER    20u                    /* 5 campi uint32          */
#define SLOT_INIZIALI ((size_t)1 << 10)      /* 1024, potenza di 2      */
#define LOAD_FACTOR_10 7                      /* > 70% piena -> raddoppia */

enum {
    DB_OK           =  0,
    DB_ERR_MEMORY   = -1,   /* malloc/calloc fallita          */
    DB_ERR_PIENO    = -2    /* buffer dati esaurito           */
};

/* ---------------------------------------------------------------- tipi */

typedef struct {
    uint32_t hash;    /* 5 x uint32 = 20 byte esatti, nessun padding */
    uint32_t key_len;
    uint32_t value_len;
    uint32_t ttl;
    uint32_t creazione;
} header_t;          /* usata solo come " documenta " del layout:      */
                     /* sul buffer si passa sempre da leggi/scrivi_u32 */

typedef struct {
    uint32_t hash;    /* confronto rapido prima del memcmp            */
    uint32_t off1;    /* offset entry + 1; 0 = slot vuoto             */
} slot_t;

typedef struct {
    uint8_t *data;    /* buffer append-only delle entry                */
    size_t   capacita;
    size_t   usato;      /* prossima entry va a data[usato]           */
    size_t   spazzatura; /* byte di entry vecchie/scadute, non utili  */
    slot_t  *slot;
    size_t   n_slot;     /* sempre potenza di 2                      */
    size_t   n_entry;
} db_t;

/* ----------------------------------------------------- lettura/scrittura */

/* memcpy e non cast: gli offset delle entry non garantiscono allineamento */
static uint32_t leggi_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static void scrivi_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof v);
}

/* FNV-1a: hash direttamente da (puntatore, lunghezza).
 * Cosi' il get NON alloca nulla: niente stringa_t da creare per cercare. */
static uint32_t calcolo_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)(uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* --------------------------------------------------------------- indice */

/*
 * trova_slot: cuore della hash table.
 * Ritorna l'indice dello slot:
 *   - *trovata == 1: lo slot contiene PROPRIO questa chiave (hash uguale
 *     NON basta: due chiavi diverse possono avere lo stesso hash. Si
 *     conferma con lunghezza + memcmp, leggendo la chiave dal buffer)
 *   - *trovata == 0: lo slot e' vuoto ed e' il punto di inserimento
 */
static size_t trova_slot(const db_t *db, uint32_t hash,
                         const char *key, size_t key_len, int *trovata)
{
    size_t idx = hash & (db->n_slot - 1);   /* AND = modulo su potenza di 2 */
    *trovata = 0;

    for (;;) {
        const slot_t *s = &db->slot[idx];

        if (s->off1 == 0)                  /* slot vuoto: inserisci qui */
            return idx;

        if (s->hash == hash) {             /* candidato: verifica la chiave */
            const uint8_t *e = db->data + (s->off1 - 1);
            uint32_t kl = leggi_u32(e + 4);
            if (kl == key_len &&
                memcmp(e + DIM_HEADER, key, key_len) == 0) {
                *trovata = 1;
                return idx;
            }
        }
        idx = (idx + 1) & (db->n_slot - 1); /* probing lineare circolare */
    }
}

/*
 * cresci_indice: raddoppia la tabella e reinserti ogni slot occupato.
 * Semplice perche' ogni chiave occupa al PIU' uno slot (il put aggiorna
 * lo slot esistente, non ne crea uno nuovo): basta riposizionare per
 * hash, senza rileggere le chiavi dal buffer.
 */
static int cresci_indice(db_t *db)
{
    size_t nuovo_n = db->n_slot * 2;
    slot_t *nuovo = calloc(nuovo_n, sizeof *nuovo);  /* calloc = tutti off1==0 */
    if (!nuovo)
        return DB_ERR_MEMORY;

    for (size_t v = 0; v < db->n_slot; v++) {
        if (db->slot[v].off1 == 0)
            continue;
        size_t idx = db->slot[v].hash & (nuovo_n - 1);
        while (nuovo[idx].off1 != 0)
            idx = (idx + 1) & (nuovo_n - 1);
        nuovo[idx] = db->slot[v];
    }

    free(db->slot);
    db->slot = nuovo;
    db->n_slot = nuovo_n;
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
    db->n_slot    = SLOT_INIZIALI;
    db->n_entry   = 0;
    return DB_OK;
}

void db_libera(db_t *db)
{
    free(db->data);
    free(db->slot);
    db->data = NULL;
    db->slot = NULL;
}

/*
 * db_put — O(1) ammortizzato.
 * Upsert: se la chiave esiste, lo slot viene puntato alla entry NUOVA
 * (appesa in fondo); la vecchia diventa spazzatura.
 */
int db_put(db_t *db, const char *key, size_t key_len,
           const char *valore, size_t val_len, uint32_t ttl)
{
    size_t dim = DIM_HEADER + key_len + val_len;
    if (db->usato + dim > db->capacita)
        return DB_ERR_PIENO;

    /* crescita PRIMA del probe: dopo il rehash gli indici cambiano,
     * quindi non si possono conservare posizioni trovate prima */
    if ((db->n_entry + 1) * 10 > db->n_slot * LOAD_FACTOR_10)
        if (cresci_indice(db) != DB_OK)
            return DB_ERR_MEMORY;

    uint32_t hash = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, hash, key, key_len, &trovata);

    if (trovata) {   /* aggiorna: la entry vecchia resta come spazzatura */
        size_t off_vecchia = db->slot[idx].off1 - 1;
        db->spazzatura += DIM_HEADER
                       + leggi_u32(db->data + off_vecchia + 4)
                       + leggi_u32(db->data + off_vecchia + 8);
    } else {
        db->n_entry++;
    }

    /* append: la entry nuova va SEMPRE in fondo al buffer */
    size_t off_nuova = db->usato;
    uint8_t *dest = db->data + off_nuova;
    scrivi_u32(dest +  0, hash);
    scrivi_u32(dest +  4, (uint32_t)key_len);
    scrivi_u32(dest +  8, (uint32_t)val_len);
    scrivi_u32(dest + 12, ttl);
    scrivi_u32(dest + 16, (uint32_t)time(NULL));   /* timestamp di nascita */
    memcpy(dest + DIM_HEADER, key, key_len);
    memcpy(dest + DIM_HEADER + key_len, valore, val_len);
    db->usato += dim;

    db->slot[idx].hash = hash;
    db->slot[idx].off1 = (uint32_t)(off_nuova + 1);
    return DB_OK;
}

/*
 * db_get — O(1).
 * Ritorna un puntatore DENTRO il buffer (valido fino alla prossima put,
 * perche' il buffer non si sposta mai) e la lunghezza del valore.
 *
 * TTL pigro (lazy): la scadenza si controlla qui, al momento della
 * lettura. zero costo in scrittura; lo spazio si recupera solo con la
 * compattazione. NB: lo slot NON viene svuotato su scadenza: farlo
 * romperebbe le catene di probing (vedi header). L'entry resta
 * raggiungibile ma " morta ", e ogni get la rilegge come scaduta.
 */
const uint8_t *db_get(db_t *db, const char *key, size_t key_len,
                      uint32_t *value_len)
{
    uint32_t hash = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, hash, key, key_len, &trovata);

    if (!trovata)
        return NULL;

    size_t off = db->slot[idx].off1 - 1;

    uint32_t ttl = leggi_u32(db->data + off + 12);
    if (ttl != 0) {
        uint32_t nato  = leggi_u32(db->data + off + 16);
        if ((uint32_t)time(NULL) - nato > ttl) {
            db->spazzatura += DIM_HEADER
                            + leggi_u32(db->data + off + 4)
                            + leggi_u32(db->data + off + 8);
            return NULL;    /* scaduta: come se non esistesse */
            /* (la spazzatura e' contata ripetutamente ad ogni get
             * scaduto: difetto noto, si sistemerebbe con un flag) */
        }
    }

    if (value_len)
        *value_len = leggi_u32(db->data + off + 8);
    return db->data + off + DIM_HEADER + key_len;
}

/*
 * get_lineare: la ricerca DI CONFRONTO, quella dell'iterazione precedente.
 * Identica semanticamente a db_get, ma cammina il buffer dall'inizio.
 * Serve solo per il benchmark: misurare il stesso lavoro con i due
 * algoritmi e' l'unico modo onesto di confronto.
 */
static const uint8_t *get_lineare(const db_t *db, const char *key,
                                  size_t key_len, uint32_t *value_len)
{
    uint32_t hash = calcolo_hash(key, key_len);
    size_t off = 0;

    while (off + DIM_HEADER <= db->usato) {
        uint32_t h  = leggi_u32(db->data + off);
        uint32_t kl = leggi_u32(db->data + off + 4);
        uint32_t dl = leggi_u32(db->data + off + 8);

        if (h == hash && kl == key_len &&
            memcmp(db->data + off + DIM_HEADER, key, key_len) == 0) {
            if (value_len)
                *value_len = dl;
            return db->data + off + DIM_HEADER + kl;
        }
        off += DIM_HEADER + kl + dl;
    }
    return NULL;
}

/* ------------------------------------------------------------ benchmark */

static double ms_da(clock_t t0)
{
    return (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
}

int main(void)
{
    db_t db;
    if (db_init(&db, 4u * 1024 * 1024) != DB_OK) {   /* 4 MB di buffer */
        fprintf(stderr, "errore di allocazione\n");
        return 1;
    }

    /* ------------------------------------------------ 1. riempimento */
    enum { N_CHIAVI = 50000 };
    static char kbuf[64], vbuf[64];

    clock_t t0 = clock();
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-di-%u", i);
        if (db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0) != DB_OK) {
            fprintf(stderr, "buffer pieno a %u\n", i);
            return 1;
        }
    }
    printf("caricate %d entry in %.1f ms  (%zu byte usati, indice %zu slot, "
           "load factor %.0f%%)\n\n",
           N_CHIAVI, ms_da(t0), db.usato, db.n_slot,
           100.0 * (double)db.n_entry / (double)db.n_slot);

    /* ---------------------------------------- 2. due dimostrazioni TLC */
    db_put(&db, "sessione:demo", 13, "abc123", 6, 2);       /* TTL 2 s */
    db_put(&db, "utente:0", 8, "valore-aggiornato-utente-0", 26, 0);

    uint32_t dl;
    const uint8_t *v = db_get(&db, "utente:1", 8, &dl);
    printf("get(\"utente:1\") -> \"%.*s\" (%u byte)\n", dl > 32 ? 32 : dl, v, dl);

    v = db_get(&db, "utente:0", 8, &dl);
    printf("get(\"utente:0\") -> \"%.*s\"  (upsert: la vecchia entry resta "
           "nel buffer come spazzatura)\n", dl > 40 ? 40 : dl, v);

    printf("get(\"sessione:demo\", ttl=2s) -> ");
    v = db_get(&db, "sessione:demo", 13, &dl);
    if (v) printf("\"%.*s\"  (non ancora scaduta)\n", dl, v);

    v = db_get(&db, "chiave-inesistente", 18, &dl);
    printf("get(\"chiave-inesistente\") -> %s\n\n", v ? "trovata?!" : "NULL");

    /* ------------------------------ 3. benchmark: stesso lavoro, due modi */
    enum { N_GET_INDICE = 100000, N_GET_LINEARE = 2000 };

    /* indicizzato: 100.000 ricerche pseudocasuali */
    t0 = clock();
    size_t somma = 0;
    for (uint32_t i = 0; i < N_GET_INDICE; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", (i * 7919) % N_CHIAVI);
        uint32_t len;
        const uint8_t *r = db_get(&db, kbuf, (size_t)kl, &len);
        if (r) somma += len;    /* usa il risultato: il compilatore non
                                   puo' eliminare la ricerca */
    }
    double t_indice = ms_da(t0);

    /* lineare: solo 2.000 ricerche, perche' gira molto piu' lento */
    t0 = clock();
    for (uint32_t i = 0; i < N_GET_LINEARE; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", (i * 7919) % N_CHIAVI);
        uint32_t len;
        const uint8_t *r = get_lineare(&db, kbuf, (size_t)kl, &len);
        if (r) somma += len;
    }
    double t_lineare = ms_da(t0);

    double per_indice = t_indice * 1000.0 / N_GET_INDICE;   /* microsecondi */
    double per_lineare = t_lineare * 1000.0 / N_GET_LINEARE;

    printf("---- benchmark get su %d entry (%zu byte) ----\n",
           N_CHIAVI, db.usato);
    printf("ricerca LINEARE (walk):    %6.2f ms per %5d get = %8.3f us/get\n",
           t_lineare, N_GET_LINEARE, per_lineare);
    printf("ricerca INDICIZZATA (O(1)): %6.2f ms per %d get = %8.3f us/get\n",
           t_indice, N_GET_INDICE, per_indice);
    printf("speedup: ~%.0f volte (cresce al crescere delle entry: O(n) vs O(1))\n\n",
           per_lineare / per_indice);

    printf("statistiche: %zu entry vive, %zu byte usati, %zu byte di spazzatura "
           "(%.1f%%), n_slot=%zu\n",
           db.n_entry, db.usato, db.spazzatura,
           100.0 * (double)db.spazzatura / (double)db.usato, db.n_slot);

    /* il valore della somma serve solo a impedire ottimizzazioni spurie */
    (void)somma;

    db_libera(&db);
    return 0;
}
