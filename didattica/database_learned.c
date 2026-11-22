/*
 * database_learned.c — sesta iterazione: l'indice del "commesso".
 *
 * =====================================================================
 * L'ESPERIMENTO
 * =====================================================================
 * Fino a qui la catena di probing partiva sempre dallo stesso posto:
 *     slot = FNV(chiave) & (n_slot - 1)
 * un hash universale sparge le chiavi a caso: due chiavi vicine nello
 * spazio ("u0000004" e "u0000005") finiscono in slot lontanissimi e
 * ignoti, proprio come una mappa del supermercato senza relazione con
 * gli ordini logici dei reparti.
 *
 * L'indice APPRESO (ispirato a: Kraska, "The Case for Learned Index
 * Structures", 2017) sostituisce la partenza con una PREVISIONE
 * statistica: come il commesso che non ha la mappa ma dopo mille
 * servizi ha interiorizzato la geometria degli scaffali.
 *
 *   model = spline: funzione a tratti lineare, monotona, che approssima
 *          la CDF delle chiavi: chiave -> posizione cumulativa in [0,1)
 *   slot  = previsione(chiave) * n_slot     (con rincalzo: probing)
 *
 * Con il carico del progetto (60000 chiavi dense "u0000000".."u0059999")
 * la CDF e' quasi perfettamente lineare: la previsione piazza le chiavi
 * CONSECUTIVE in slot CONSECUTIVI. Dove l'hash universale sparge al
 * caso (e collide per paradosso del compleanno), il modello allinea
 * chiavi vicine: catene di probing quasi azzerate.
 *
 * ONESTA' DELL'ESPERIMENTO:
 *   - il fingerprint FNV resta: la previsione puo' sbagliare lo slot
 *     di partenza, la conferma d'identita' non si affida mai al modello;
 *   - il probing lineare resta: la previsione sbagliata costa catene
 *     piu' lunghe, MAI risposte sbagliate;
 *   - si misura anche il caso ostico: chiavi ASSENTI da una distribuzione
 *     spostata (che il commesso non ha mai visto): la previsione parte
 *     dallo slot sbagliato e il costo del miss e' tutto li'.
 *
 * =====================================================================
 * L' INVARIANTE (la vera lezione pratica dei learned indexes)
 * =====================================================================
 * La posizione di una chiave nella tabella dipende dal punto di partenza
 * usato quando e' stata inserita. Se la funzione di partenza cambia
 * (addestramento, cambio motore, raddoppio della tabella) SENZA
 * ricostruire, le chiavi diventano irraggiungibili: cercare partendo da
 * un punto diverso non trovera' mai cio' che sta oltre il primo slot
 * vuoto della catena. Quindi:
 *
 *     OGNI cambiamento della funzione di partenza RICOSTRUISCE la
 *     tabella con la stessa funzione.
 *
 * cambia_modalita(), cresci_indice() e db_compatta() rispettano questo
 * contratto: modelo e tabella vengono sempre rigenerati INSIEME.
 *
 * =====================================================================
 * POST MORTEM DELLA PRIMA VERSIONE (l'esperimento fallito, da conservare)
 * =====================================================================
 * La feature iniziale era "i primi 8 byte big-endian" della chiave.
 * Sembra innocua: e' monotona sull'ordine lessicografico. Ma le chiavi
 * del carico sono DECIMALI ("u0000000".."u0059999") e in binario il
 * riporto del 9 allo 0 salta di ~247 unita' di feature invece che di 1:
 * nello spazio dela feature la CDF e' a gradini, non lineare, e con
 * 1024 nodi la spline interpola a spanne di ~58 chiavi: dentro ogni
 * segmento le previsioni sbagliavano di decine di slot, le chiavi si
 * ammassavano a bande (le miss "sotto" toccavano SEMPRE 42 slot pieni,
 * media = max: firma inequivocabile) e il commesso perdeva 22 a 1.58
 * contro la mappa FNV. Correttezza mai compromessa (il probing rincalza
 * sempre), solo velocita' ---- la firma di un learned index.
 *
 * Doppia lezione: (1) la feature e' TUTTO: deve essere monotona E lineare
 * rispetto al ordine "logico" dei dati; (2) la densita' dei nodi e' la
 * memoria del commesso: pochi nodi = interpolation a spanne lunghe =
 * errore. La feature giusta sotto ("il numero in coda") rende la CDF
 * esattamente lineare e basterebbero 4 nodi.
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
#endif

#define DIM_HEADER     20u
#define SLOT_INIZIALI  ((size_t)1 << 10)
#define LOAD_FACTOR_10 7
#define CAPACITA_MIN   ((size_t)1 << 16)
#define CAPACITA_MAX   ((size_t)1 << 28)
#define OFF1_VUOTO     0u
#define OFF1_TOMBSTONE 0xFFFFFFFFu
#define MORTA_BIT      0x80000000u
#define NODI_SPLINE    1024

enum {
    DB_OK          =  0,
    DB_ERR_MEMORY  = -1,
    DB_ERR_PIENO   = -2,
    DB_ERR_MANCA   = -3
};

typedef struct {
    uint32_t fingerprint;   /* FNV(chiave): conferma d'identita'         */
    uint32_t off1;          /* VUOTO / TOMBSTONE / offset entry + 1      */
} slot_t;

typedef struct {
    uint64_t u;     /* ordinata: prefisso chiave a 8 byte big-endian   */
    double   q;     /* CDF approssimata in [0,1): "quanto sono avanti"  */
} nodo_t;

typedef struct {
    nodo_t *nodo;
    size_t  n_nodi;
} modello_t;

typedef struct {
    uint8_t *data;
    size_t   capacita;
    size_t   usato;
    size_t   spazzatura;
    slot_t  *slot;
    size_t   n_slot;
    size_t   n_entry;
    size_t   n_tomb;
    modello_t modello;
    int      usa_modello;
    /* strumento di misura: i probe Toccati per ogni operazione */
    uint64_t prove_tot;
    uint32_t prove_max;
} db_t;

/* ------------------------------------------------------------- utilita' */

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

/*
 * riserva: prefisso bytes big-endian (fino a 8 byte), monotono in se'.
 * Serve per le chiavi SENZA numero in coda.
 */
static uint64_t chiave_a_u64(const char *k, size_t len)
{
    uint64_t u = 0;
    size_t n = len < 8 ? len : 8;
    for (size_t i = 0; i < n; i++)
        u = (u << 8) | (uint8_t)k[i];
    return u << (8 * (8 - n));
}

/*
 * LA FEATURE: "che numero ha in coda?" — se la chiave finisce con cifre,
 * il numero in coda e' la posizione logica (il prezzo sullo scaffale).
 * Questa e' la parte INGEGNERIA di un learned index: scegliere la
 * trasformazione giusta dei dati non e' dettaglio, e' il progetto.
 * Chiavi senza cifre in coda cadono sul prefisso bytes: resta una
 * funzione ben definita ma mescola le due scale — la spline le ordinera'
 * in fila alle numeriche. Il probing rincalza: costo, mai errore.
 */
static uint64_t chiave_a_rango(const char *k, size_t len)
{
    size_t inizio = len;
    while (inizio > 0 && k[inizio - 1] >= '0' && k[inizio - 1] <= '9')
        inizio--;
    if (inizio == len)                        /* nessuna cifra in coda */
        return chiave_a_u64(k, len);
    uint64_t n = 0;
    for (size_t i = inizio; i < len; i++)
        n = n * 10 + (uint64_t)(k[i] - '0');
    return n;
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

/* ------------------------------------------------ il modello (la spline) */

/*
 * PREVISIONE: cerca il segmento della spline che contiene u e interpola.
 * Costo: log2(n_nodi) confronti su un array da 12KB = una decina di
 * cache line: il "calcolo mentale" del commesso. Se il modello non e'
 * addestrato (n_nodi < 2) cade sull'hash universale: la stessa tabella
 * resta utilisabile sia col commesso sia con la mappa.
 */
static uint32_t previsione_slot(const db_t *db, const char *key, size_t klen,
                                uint32_t fingerprint)
{
    if (!db->usa_modello || db->modello.n_nodi < 2)
        return fingerprint & (uint32_t)(db->n_slot - 1);

    uint64_t u = chiave_a_rango(key, klen);
    const nodo_t *nd = db->modello.nodo;
    size_t n = db->modello.n_nodi;

    size_t lo = 0, hi = n - 1;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (nd[mid].u <= u)
            lo = mid;
        else
            hi = mid;
    }

    double t = 0.0;
    if (nd[hi].u > nd[lo].u)
        t = (double)(u - nd[lo].u) / (double)(nd[hi].u - nd[lo].u);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    double q = nd[lo].q + t * (nd[hi].q - nd[lo].q);
    double s = q * (double)db->n_slot;
    if (s >= (double)db->n_slot)
        s = (double)(db->n_slot - 1);
    return (uint32_t)s;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/*
 * ADDESTRAMENTO: raccoglie gli u64 delle chiavi vive, li ordina (il
 * dei valori: e' tutta qui la saggezza del commesso) e campiona la
 * CDF al volo: NODI_SPLINE punti equidistanti in quantile.
 * Serve O(n log n) una volta ogni tanto (qui la si paga ai cambi di
 * modalita' e alle compattazioni): il costo della sua memoria.
 */
static int addestra(db_t *db)
{
    size_t puntate = 0;
    for (size_t v = 0; v < db->n_slot; v++)
        if (db->slot[v].off1 != OFF1_VUOTO &&
            db->slot[v].off1 != OFF1_TOMBSTONE)
            puntate++;

    free(db->modello.nodo);
    db->modello.nodo = NULL;
    db->modello.n_nodi = 0;
    if (puntate < 2)
        return DB_OK;

    uint64_t *vals = malloc(puntate * sizeof *vals);
    if (!vals)
        return DB_ERR_MEMORY;

    size_t k = 0;
    for (size_t v = 0; v < db->n_slot; v++) {
        uint32_t off1 = db->slot[v].off1;
        if (off1 == OFF1_VUOTO || off1 == OFF1_TOMBSTONE)
            continue;
        const uint8_t *e = db->data + (off1 - 1);
        vals[k++] = chiave_a_rango((const char *)(e + DIM_HEADER), leggi_u32(e + 4));
    }

    qsort(vals, puntate, sizeof *vals, cmp_u64);

    /* quantili equidistanti, saltando i duplicati (la spline esige
     * strettamente crescente per la divisione sicura nell'interpola) */
    nodo_t *nodici = malloc(NODI_SPLINE * sizeof *nodici);
    size_t passi = puntate < NODI_SPLINE ? puntate : NODI_SPLINE;
    size_t n_nodi = 0;
    for (size_t i = 0; i < passi; i++) {
        size_t idx = i * (puntate - 1) / (passi - 1);
        if (n_nodi > 0 && vals[idx] == nodici[n_nodi - 1].u)
            continue;
        nodici[n_nodi].u = vals[idx];
        nodici[n_nodi].q = (double)i / (double)(passi - 1);
        n_nodi++;
    }

    free(vals);
    if (n_nodi < 2)
        { free(nodici); return DB_OK; }
    db->modello.nodo = nodici;
    db->modello.n_nodi = n_nodi;
    return DB_OK;
}

/* --------------------------------------------------------------- indice */

/*
 * trova_slot: probing lineare IDENTICO a prima, cambia solo il punto
 * di partenza. Conta i probe toccati: e' la metrica piu' onesta per
 * capire chi lavora meglio fra la mappa e il commesso.
 */
static size_t trova_slot(db_t *db, const char *key, size_t key_len,
                         uint32_t fingerprint, int *trovata)
{
    size_t idx = previsione_slot(db, key, key_len, fingerprint);
    size_t tomb = (size_t)-1;
    uint32_t prove = 0;
    *trovata = 0;

    for (;;) {
        prove++;
        uint32_t off1 = db->slot[idx].off1;

        if (off1 == OFF1_VUOTO) {
            size_t r = tomb == (size_t)-1 ? idx : tomb;
            db->prove_tot += prove;
            if (prove > db->prove_max)
                db->prove_max = prove;
            return r;
        }

        if (off1 == OFF1_TOMBSTONE) {
            if (tomb == (size_t)-1)
                tomb = idx;
        } else if (db->slot[idx].fingerprint == fingerprint) {
            const uint8_t *e = db->data + (off1 - 1);
            if (leggi_u32(e + 4) == key_len &&
                memcmp(e + DIM_HEADER, key, key_len) == 0) {
                *trovata = 1;
                db->prove_tot += prove;
                if (prove > db->prove_max)
                    db->prove_max = prove;
                return idx;
            }
        }
        idx = (idx + 1) & (db->n_slot - 1);
    }
}

/*
 * inserisci_entry: posiziona una entry ESISTENTE nella tabella (usato
 * da cresci / ricostruisci / compatta). Non discrimina le chiavi:
 * ogni entry prende il primo slot libero della sua catena.
 */
static void inserisci_entry(db_t *db, const uint8_t *entry, size_t off)
{
    const char *key = (const char *)(entry + DIM_HEADER);
    uint32_t fingerprint = leggi_u32(entry);
    uint32_t key_len = leggi_u32(entry + 4);
    size_t idx = previsione_slot(db, key, key_len, fingerprint);
    while (db->slot[idx].off1 != OFF1_VUOTO)
        idx = (idx + 1) & (db->n_slot - 1);
    db->slot[idx].fingerprint = fingerprint;
    db->slot[idx].off1 = (uint32_t)(off + 1);
}

/*
 * cresci_indice — il raddoppio RISPETTA l'invariante: la funzione di
 * partenza cambia (maschera diversa, n_slot diverso) quindi TUTTE le
 * entrry vengono riposizionate con la funzione nuova. Con il modello
 * attivo, la previsione di ogni chiave si ricalcola dal suo rango.
 */
static int cresci_indice(db_t *db)
{
    size_t nuovo_n = db->n_slot * 2;
    slot_t *nuovo = calloc(nuovo_n, sizeof *nuovo);
    if (!nuovo)
        return DB_ERR_MEMORY;

    /* appoggio: lista degli offset da riposizionare */
    size_t puntate = db->n_entry + db->n_tomb;
    uint32_t *offsets = malloc((puntate ? puntate : 1) * sizeof *offsets);
    if (!offsets)
        { free(nuovo); return DB_ERR_MEMORY; }
    size_t k = 0;
    for (size_t v = 0; v < db->n_slot; v++) {
        uint32_t off1 = db->slot[v].off1;
        if (off1 == OFF1_VUOTO || off1 == OFF1_TOMBSTONE)
            continue;
        offsets[k++] = off1 - 1;   /* i tombstone muoiono col rebuild */
    }

    free(db->slot);
    db->slot = nuovo;
    db->n_slot = nuovo_n;
    db->n_tomb = 0;

    for (size_t i = 0; i < k; i++)
        inserisci_entry(db, db->data + offsets[i], offsets[i]);
    free(offsets);
    return DB_OK;
}

/*
 * ricostruisci: riposiziona tutto con la funzione di partenza corrente.
 * E' il gestore dell'INVARIANTE: la si chiama dopo ogni addestramento
 * o cambio di motore. O(n), i tombstone si dissolvono (catene fresche).
 */
static int ricostruisci(db_t *db)
{
    size_t puntate = db->n_entry + db->n_tomb;
    uint32_t *offsets = malloc((puntate ? puntate : 1) * sizeof *offsets);
    if (!offsets)
        return DB_ERR_MEMORY;
    size_t k = 0;
    for (size_t v = 0; v < db->n_slot; v++) {
        uint32_t off1 = db->slot[v].off1;
        if (off1 == OFF1_VUOTO || off1 == OFF1_TOMBSTONE)
            continue;
        offsets[k++] = off1 - 1;
    }

    memset(db->slot, 0, db->n_slot * sizeof *db->slot);
    db->n_tomb = 0;
    for (size_t i = 0; i < k; i++)
        inserisci_entry(db, db->data + offsets[i], offsets[i]);
    free(offsets);
    return DB_OK;
}

/*
 * cambia_modalita: il commesso subentra alla mappa (o viceversa).
 * Sempre nello stesso ordine: (1) RIADDESTRA sul dataset corrente
 * (il commesso all'inizio del turno rilegge il magazzino, sempre:
 * usare un modello stanco non risparmia niente e confonde l'invariante),
 * (2) ricostruisci con la funzione nuova. Il contrario cercherebbe con un
 * modello allenato su chiavi che gia' non ci sono piu'.
 */
static int cambia_modalita(db_t *db, int usa_modello)
{
    if (db->usa_modello == usa_modello)
        return DB_OK;                    /* niente cambia, niente si rifa' */
    if (usa_modello && db->n_entry > 0) {
        int r = addestra(db);
        if (r != DB_OK)
            return r;
    }
    db->usa_modello = usa_modello;
    return ricostruisci(db);
}

/* ------------------------------------------- crescita del buffer dati */

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
    db->data = p;
    db->capacita = nuova;
    return DB_OK;
}

/* ------------------------------------------------------------ database */

int db_init(db_t *db)
{
    db->data = malloc(CAPACITA_MIN);
    db->slot = calloc(SLOT_INIZIALI, sizeof *db->slot);
    if (!db->data || !db->slot) {
        free(db->data);
        free(db->slot);
        return DB_ERR_MEMORY;
    }
    memset(&db->modello, 0, sizeof db->modello);
    db->capacita = CAPACITA_MIN;
    db->usato = 0;
    db->spazzatura = 0;
    db->n_slot = SLOT_INIZIALI;
    db->n_entry = 0;
    db->n_tomb = 0;
    db->usa_modello = 0;
    db->prove_tot = 0;
    db->prove_max = 0;
    return DB_OK;
}

void db_libera(db_t *db)
{
    free(db->data);
    free(db->slot);
    free(db->modello.nodo);
    db->data = NULL;
    db->slot = NULL;
    db->modello.nodo = NULL;
}

static void statistiche(const db_t *db, const char *quando)
{
    printf("%-14s %7zu byte, %6zu spazzatura (%4.1f%%), %5zu vive, %4zu tomb, "
           "%6zu slot (LF %2.0f%%), modello %s\n",
           quando, db->usato, db->spazzatura,
           db->usato ? 100.0 * (double)db->spazzatura / (double)db->usato : 0.0,
           db->n_entry, db->n_tomb, db->n_slot,
           100.0 * (double)(db->n_entry + db->n_tomb) / (double)db->n_slot,
           db->usa_modello && db->modello.n_nodi >= 2 ? "ATTIVO" : "off");
}

int db_put(db_t *db, const char *key, size_t key_len,
           const char *valore, size_t val_len, uint32_t ttl)
{
    size_t dim = DIM_HEADER + key_len + val_len;
    int r = assicura_spazio(db, dim);
    if (r != DB_OK)
        return r;

    if ((db->n_entry + db->n_tomb + 1) * 10 > db->n_slot * LOAD_FACTOR_10)
        if (cresci_indice(db) != DB_OK)
            return DB_ERR_MEMORY;

    uint32_t fingerprint = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, key, key_len, fingerprint, &trovata);

    if (trovata) {
        size_t off_vecchia = db->slot[idx].off1 - 1;
        if (!entry_morta(db->data + off_vecchia))
            db->spazzatura += entry_dim(db->data + off_vecchia);
    } else {
        if (db->slot[idx].off1 == OFF1_TOMBSTONE)
            db->n_tomb--;
        db->n_entry++;
    }

    size_t off = db->usato;
    uint8_t *dest = db->data + off;
    scrivi_u32(dest +  0, fingerprint);
    scrivi_u32(dest +  4, (uint32_t)key_len);
    scrivi_u32(dest +  8, (uint32_t)val_len);
    scrivi_u32(dest + 12, ttl);
    scrivi_u32(dest + 16, (uint32_t)time(NULL));
    memcpy(dest + DIM_HEADER, key, key_len);
    memcpy(dest + DIM_HEADER + key_len, valore, val_len);
    db->usato += dim;

    db->slot[idx].fingerprint = fingerprint;
    db->slot[idx].off1 = (uint32_t)(off + 1);
    return DB_OK;
}

int db_del(db_t *db, const char *key, size_t key_len)
{
    uint32_t fingerprint = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, key, key_len, fingerprint, &trovata);
    if (!trovata)
        return DB_ERR_MANCA;
    size_t off = db->slot[idx].off1 - 1;
    segna_morta(db->data, off);
    db->spazzatura += entry_dim(db->data + off);
    db->slot[idx].off1 = OFF1_TOMBSTONE;
    db->n_tomb++;
    db->n_entry--;
    return DB_OK;
}

const uint8_t *db_get(db_t *db, const char *key, size_t key_len,
                      uint32_t *value_len)
{
    uint32_t fingerprint = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, key, key_len, fingerprint, &trovata);
    if (!trovata)
        return NULL;
    const uint8_t *e = db->data + (db->slot[idx].off1 - 1);
    if (entry_morta(e))
        return NULL;
    uint32_t ttl = leggi_u32(e + 12);
    if (ttl != 0) {
        uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
        if ((uint32_t)time(NULL) - nato > ttl) {
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
 * La compattazione NON riaddestra il commesso. Il modello descrive le
 * CHIAVI VIVE e la compattazione non tocca le chiavi vive: elimina solo
 * i byte morti e riposiziona TUTTO con la stessa funzione di partenza
 * (invariante salvaguardata). I riaddettramenti avvengono nei punti in
 * cui cambia la funzione o il compito: cambio modalita'.
 * (Nella prima versione addestra girava DOPO lo swap del buffer, con gli
 * slot ancora carichi di offset VECCHI: leggeva "chiavi" dal punto
 * sbagliato del buffer nuovo e segfaultava. L'ordine delle operazioni
 * quando si moscano buffer e indici resta da disegnare PRIMA.)
 */
int db_compatta(db_t *db)
{
    uint8_t *nuovo = malloc(db->capacita);
    if (!nuovo)
        return DB_ERR_MEMORY;

    size_t nu = 0;
    size_t vive = 0;
    uint32_t adesso = (uint32_t)time(NULL);

    uint32_t *offsets = malloc((db->n_entry + db->n_tomb + 1) * sizeof *offsets);
    if (!offsets)
        { free(nuovo); return DB_ERR_MEMORY; }

    for (size_t v = 0; v < db->n_slot; v++) {
        uint32_t off1 = db->slot[v].off1;
        if (off1 == OFF1_VUOTO || off1 == OFF1_TOMBSTONE)
            continue;
        const uint8_t *e = db->data + (off1 - 1);
        if (entry_morta(e))
            continue;
        uint32_t ttl  = leggi_u32(e + 12);
        uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
        if (ttl != 0 && adesso - nato > ttl)
            continue;
        memcpy(nuovo + nu, e, entry_dim(e));
        offsets[vive] = (uint32_t)nu;
        nu += entry_dim(e);
        vive++;
    }

    free(db->data);
    db->data = nuovo;
    db->usato = nu;
    db->spazzatura = 0;
    db->n_entry = vive;
    db->n_tomb = 0;

    /* riposiziona con la funzione di partenza INVARIATA: il modello
     * descrive le chiavi vive, non la geometria del buffer */
    memset(db->slot, 0, db->n_slot * sizeof *db->slot);
    for (size_t i = 0; i < vive; i++)
        inserisci_entry(db, db->data + offsets[i], offsets[i]);
    free(offsets);
    return DB_OK;
}

void db_reset_prove(db_t *db)
{
    db->prove_tot = 0;
    db->prove_max = 0;
}

/* ------------------------------------------------------------ benchmark */

enum { CAMPIONI = 100000 };

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void benchmark_get(db_t *db, uint32_t n_chiavi, const char *fmt,
                         const char *etichetta)
{
    static double t[CAMPIONI];
    static char kbuf[64];
    double media = 0;

    db_reset_prove(db);
    for (uint32_t i = 0; i < CAMPIONI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, fmt, (i * 7919u) % n_chiavi);
        double a = ora_ms();
        db_get(db, kbuf, (size_t)kl, NULL);
        double b = ora_ms();
        t[i] = b - a;
        media += t[i];
    }
    qsort(t, CAMPIONI, sizeof t[0], cmp_double);

    printf("%-26s media %6.3f us  p50 %6.3f  p90 %6.3f  p99 %6.3f  "
           "max %7.1f us   probe: medie %.2f, max %u\n",
           etichetta,
           media / CAMPIONI * 1000.0,
           t[CAMPIONI / 2]        * 1000.0,
           t[CAMPIONI * 9 / 10]   * 1000.0,
           t[CAMPIONI * 99 / 100] * 1000.0,
           t[CAMPIONI - 1]        * 1000.0,
           (double)db->prove_tot / (double)CAMPIONI,
           db->prove_max);
}

/* ------------------------------------------------------------------ test */

/*
 * Il banco di corretezza per il commesso: mescola insirimenti, update,
 * delete e reinsert; attraversa le ricostruzioni (cambio modalita',
 * compattazione, raddoppio INDICE durante il put con modello attivo);
 * verifica alla fine che OGNI chiave sia esattamente dove deve essere.
 * Le aspettative sono calcolate con la stessa aritmetica dei pattern:
 *   i%5==0  aggiornata (value2)
 *   i%7==0  cancellata ... MA
 *   i%11==0 risorta (re-insert dopo la cancellazione)
 */
static int autotest(void)
{
    db_t db;
    if (db_init(&db) != DB_OK)
        return 1;
    enum { N = 2000 };
    char kbuf[24], vbuf[40];
    int errori = 0;

    /* fase 1: inserimento sotto FNV */
    for (int i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "t%04d", i);
        int vl = snprintf(vbuf, sizeof vbuf, "v%d", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }

    /* il commesso subentra: addestramento + ricostruzione */
    if (cambia_modalita(&db, 1) != DB_OK) {
        printf("autotest FALLITO: addestramento\n");
        return 1;
    }

    /* fase 2: Mescolanza sotto il modello; chiavi NUOVE spingono la
     * tabella oltre una soglia di crescita QUI, con la previsione attiva:
     * e' il percorso piu' delicato dell'invariante */
    for (int i = N; i < N + 1200; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "t%04d", i);
        int vl = snprintf(vbuf, sizeof vbuf, "v%d", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    for (int i = 0; i < N + 1200; i += 5) {
        int kl = snprintf(kbuf, sizeof kbuf, "t%04d", i);
        int vl = snprintf(vbuf, sizeof vbuf, "aggiornato-%d", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    for (int i = 0; i < N + 1200; i += 7) {
        int kl = snprintf(kbuf, sizeof kbuf, "t%04d", i);
        db_del(&db, kbuf, (size_t)kl);
    }
    for (int i = 0; i < N + 1200; i += 11) {
        int kl = snprintf(kbuf, sizeof kbuf, "t%04d", i);
        int vl = snprintf(vbuf, sizeof vbuf, "risorto-%d", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }

    /* verifica in tre stadi: subito, dopo compattazione, dopo il
     * ritorno alla mappa FNV E dopo il ritorno del commesso */
    for (int fase = 0; fase < 4 && errori == 0; fase++) {
        if (fase == 1)
            db_compatta(&db);
        else if (fase == 2)
            cambia_modalita(&db, 0);
        else if (fase == 3)
            cambia_modalita(&db, 1);

        for (int i = 0; i < N + 1200 && errori == 0; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "t%04d", i);
            uint32_t dl;
            const uint8_t *v = db_get(&db, kbuf, (size_t)kl, &dl);

            int cancellata  = (i % 7 == 0) && (i % 11 != 0);
            if (cancellata) {
                if (v != NULL) {
                    printf("autotest FALLITO (fase %d): %s resuscitata\n",
                           fase, kbuf);
                    errori++;
                }
                continue;
            }
            /* esiste: il valore deve essere l'ultima versione */
            snprintf(vbuf, sizeof vbuf, "risorto-%d", i);
            int vl1 = (int)strlen(vbuf);
            if (i % 11 == 0) {
                if (v == NULL || dl != (uint32_t)vl1 ||
                    memcmp(v, vbuf, dl) != 0) {
                    printf("autotest FALLITO (fase %d): %s non risorta\n",
                           fase, kbuf);
                    errori++;
                }
                continue;
            }
            if (i % 5 == 0) {
                snprintf(vbuf, sizeof vbuf, "aggiornato-%d", i);
                int vl2 = (int)strlen(vbuf);
                if (v == NULL || dl != (uint32_t)vl2 ||
                    memcmp(v, vbuf, dl) != 0) {
                    printf("autotest FALLITO (fase %d): %s non aggiornata\n",
                           fase, kbuf);
                    errori++;
                }
                continue;
            }
            snprintf(vbuf, sizeof vbuf, "v%d", i);
            int vl3 = (int)strlen(vbuf);
            if (v == NULL || dl != (uint32_t)vl3 ||
                memcmp(v, vbuf, dl) != 0) {
                printf("autotest FALLITO (fase %d): %s persa o errata\n",
                       fase, kbuf);
                errori++;
            }
        }
    }

    if (errori == 0)
        printf("autotest: 3200 chiavi, update+delete+reinsert, growth col "
               "modello attivo, compattazione e cambio motore avanti e "
               "indietro: OK\n");
    db_libera(&db);
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

    static char kbuf[32], vbuf[64];
    enum { N_CHIAVI = 60000 };

    printf("==== fase 1: carico DENSO e SEQUENZIALE (il terreno del "
           "commesso) ====\n");
    double t0 = ora_ms();
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-di-%u", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    printf("pieno: %d chiavi in %.1f ms\n", N_CHIAVI, ora_ms() - t0);
    statistiche(&db, "dopo carico");

    benchmark_get(&db, N_CHIAVI, "u%07u", "FNV, chiavi presenti:");

    t0 = ora_ms();
    if (cambia_modalita(&db, 1) != DB_OK) {
        fprintf(stderr, "addestramento fallito\n");
        return 1;
    }
    printf("addestramento + ricostruzione: %.1f ms, %zu nodi di spline su "
           "%d chiavi\n", ora_ms() - t0, db.modello.n_nodi, N_CHIAVI);

    benchmark_get(&db, N_CHIAVI, "u%07u", "LEARNED, chiavi presenti:");

    printf("\n==== fase 2: sporcizia (upsert metà, cancella un sesto) ====\n");
    for (uint32_t i = 0; i < N_CHIAVI; i += 2) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-nuovo-lungo-di-%u", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    for (uint32_t i = 0; i < N_CHIAVI; i += 6)
        db_del(&db, kbuf, (size_t)snprintf(kbuf, sizeof kbuf, "u%07u", i));
    statistiche(&db, "dopo upd+del");

    benchmark_get(&db, N_CHIAVI, "u%07u", "LEARNED, con garbage:");
    cambia_modalita(&db, 0);
    benchmark_get(&db, N_CHIAVI, "u%07u", "FNV, stesso stato:");

    printf("\n==== fase 3: chiavi ASSENTI: la distribuzione che il "
           "commesso non ha mai visto ====\n");
    cambia_modalita(&db, 1);
    benchmark_get(&db, N_CHIAVI, "s%07u", "LEARNED, assenti sotto:");
    benchmark_get(&db, N_CHIAVI, "v%07u", "LEARNED, assenti sopra:");
    cambia_modalita(&db, 0);
    benchmark_get(&db, N_CHIAVI, "s%07u", "FNV, assenti sotto:");
    benchmark_get(&db, N_CHIAVI, "v%07u", "FNV, assenti sopra:");

    printf("\n");
    if (autotest() != 0)
        return 1;

    db_libera(&db);
    return 0;
}
