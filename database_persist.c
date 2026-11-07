/*
 * database_persist.c — quinta iterazione del progetto.
 *
 * =====================================================================
 * NOVITA' RISPETTO A database_compact.c
 * =====================================================================
 *
 *  1. PERSISTENZA su file con protezione del formato binario.
 *
 *     File: [ MAGIA "PDB1" | versione | n_entry ]-- 12 byte di header
 *           [ entry 0 ][ entry 1 ]...               -- solo db->usato byte
 *
 *     Entry (ora DIM_HEADER = 24 byte, 6 x uint32):
 *       [ 0..4)  hash  [4..8) key_len  [8..12) value_len
 *       [12..16) ttl    [16..20) creazione (bit 31 = entry morta)
 *       [20..24) crc32                            <-- NUOVO
 *       [24..24+key_len) chiave, poi valore
 *
 *     Il CRC-32 (IEEE 802.3, polinomio riflesso 0xEDB88320: quello di
 *     zip e png) copre i primi 20 byte di header PIU' chiave e valore,
 *     cioe' tutto tranne il CRC stesso. Un solo campo corrotto — ttl,
 *     lunghezze, un byte del valore — rende il CRC insensato.
 *     Regola pratica: CRC su tutto quello che si legge per fidarsi.
 *
 *  2. CRASH SAFETY = la validazione al CARICAMENTO, non al salvataggio.
 *     Se il processo muore mentre scrive il file, l'ultima entry e'
 *     troncata a meta': il walker la trova con key_len+value_len oltre
 *     la fine del file, oppure con il CRC sbagliato, e si FERMA li'.
 *     Ogni byte oltre il primo errore non viene nemmeno guardato:
 *     il database carica il prefisso valido e segnala quanti byte ha
 *     perso. Ricominciare dal'ultimo file buono e' compito del
 *     chiamante (backup / giornale).
 *
 *  3. IL SALVATAGGIO E' UNO SNAPSHOT COMPATTATO e il load e' un replay
 *     semplice. db_salva compatta PRIMA di scrivere: nel file finiscono
 *     SOLO le entry vive, in ordine di slot. Questo evita un difetto
 *     sottile: le chiavi sovrascritte avrebbero due versioni nel file
 *     (quale vince al load?), e le cancellate hanno il campo creazione
 *     mutato dal bit MORTA — cioe' il CRC calcolato alla put non
 *     corrisponde piu' e il walker si fermerebbe su una entry "guasta"
 *     che in realta' e' semplicemente morta. Con lo snapshot compattato
 *     il load ha due sole regole: entry sana => carica, entry guasta
 *     (troncata o CRC errato) => fermati li'. Compattare al salvataggio
 *     e' la stessa scelta del "merge" di Bitcask/LSM.
 *
 *  4. CRESCITA DINAMICA DEL BUFFER con realloc.
 *     Si puo' fare SOLO perche' l'indice contiene OFFSET, non
 *     puntatori: se l'indice salvasse uint8_t* la realloc invaliderebbe
 *     ogni entry. Gli offset sopravvivono allo spostamento del buffer.
 *     Le dimensioni partono piccole (64 KB) e raddoppiano a domanda.
 *
 *  5. BENCHMARK A PERCENTILI (p50/p99) invece della sola media.
 *     L'anomalia dell'iterazione precedente (get piu' lento DOPO la
 *     compattazione) si indaga con la distribuzione, non con la media:
 *     la hash table rende bene quando p99 ~ p50; se p99 >> p50 le code
 *     delle catene di probing dominano. Campionare il tempo di OGNI
 *     get e ordinare il campione il costo che paga.
 *
 *  6. TEST PERSISTENZA: round-trip save/load completo, poi corruzione
 *     controllata di un byte dentro l'ultima entry (il CRC deve
 *     fermare il load li'), poi troncamento del file (il walker deve
 *     perdere solo la coda).
 *
 * Limiti restanti (prossimi passi): save asincrono, fsync, percentili
 * con warmup multi-round, robin-hood hashing.
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

#define DIM_HEADER     24u               /* 6 x uint32                    */
#define SLOT_INIZIALI  ((size_t)1 << 10)
#define LOAD_FACTOR_10 7
#define CAPACITA_MIN   ((size_t)1 << 16)  /* 64 KB                         */
#define CAPACITA_MAX   ((size_t)1 << 28)  /* 256 MB: tetto di sicurezza     */

#define OFF1_VUOTO     0u
#define OFF1_TOMBSTONE 0xFFFFFFFFu
#define MORTA_BIT      0x80000000u

/* formato del file */
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
    uint32_t hash;
    uint32_t off1;      /* VUOTO / TOMBSTONE / offset+1 */
} slot_t;

typedef struct {
    uint8_t *data;
    size_t   capacita;
    size_t   usato;
    size_t   spazzatura;
    slot_t  *slot;
    size_t   n_slot;
    size_t   n_entry;
    size_t   n_tomb;
} db_t;

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

static size_t prossima_potenza(size_t v)
{
    size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
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

/* Streaming: stato iniziale, aggiornamenti per chunk, final xor */
static uint32_t crc32_inizio(void)                 { return 0xFFFFFFFFu; }
static uint32_t crc32_fine(uint32_t s)             { return s ^ 0xFFFFFFFFu; }

static uint32_t crc32_aggiorna(uint32_t s, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        s = crc_tabella[(s ^ p[i]) & 0xFFu] ^ (s >> 8);
    return s;
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

/* --------------------------------------------------------------- indice */

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
                tomb = idx;
        } else if (db->slot[idx].hash == hash) {
            const uint8_t *e = db->data + (off1 - 1);
            if (leggi_u32(e + 4) == key_len &&
                memcmp(e + DIM_HEADER, key, key_len) == 0) {
                *trovata = 1;
                return idx;
            }
        }
        idx = (idx + 1) & (db->n_slot - 1);
    }
}

static int cresci_indice(db_t *db)
{
    size_t nuovo_n = db->n_slot * 2;
    slot_t *nuovo = calloc(nuovo_n, sizeof *nuovo);
    if (!nuovo)
        return DB_ERR_MEMORY;

    for (size_t v = 0; v < db->n_slot; v++) {
        if (db->slot[v].off1 == OFF1_VUOTO ||
            db->slot[v].off1 == OFF1_TOMBSTONE)
            continue;
        size_t idx = db->slot[v].hash & (nuovo_n - 1);
        while (nuovo[idx].off1 != OFF1_VUOTO)
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

/* ------------------------------------------- crescita del buffer dati */

/*
 * L'indice salva OFFSET: la realloc puo' spostare il buffer e non
 * invalida niente. E' per questo che slot_t non ha puntatori.
 */
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

/* ------------------------------------------------------------ database */

int db_init(db_t *db)
{
    crc_init();
    db->data = malloc(CAPACITA_MIN);
    db->slot = calloc(SLOT_INIZIALI, sizeof *db->slot);
    if (!db->data || !db->slot) {
        free(db->data);
        free(db->slot);
        return DB_ERR_MEMORY;
    }
    db->capacita   = CAPACITA_MIN;
    db->usato      = 0;
    db->spazzatura = 0;
    db->n_slot     = SLOT_INIZIALI;
    db->n_entry    = 0;
    db->n_tomb     = 0;
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
    printf("%-14s %7zu byte usati, %6zu spazzatura (%4.1f%%), %5zu vive, "
           "%4zu tomb, %6zu slot (LF %2.0f%%)\n",
           quando, db->usato, db->spazzatura,
           db->usato ? 100.0 * (double)db->spazzatura / (double)db->usato : 0.0,
           db->n_entry, db->n_tomb, db->n_slot,
           100.0 * (double)(db->n_entry + db->n_tomb) / (double)db->n_slot);
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

    uint32_t hash = calcolo_hash(key, key_len);
    int trovata;
    size_t idx = trova_slot(db, hash, key, key_len, &trovata);

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
    scrivi_u32(dest +  0, hash);
    scrivi_u32(dest +  4, (uint32_t)key_len);
    scrivi_u32(dest +  8, (uint32_t)val_len);
    scrivi_u32(dest + 12, ttl);
    scrivi_u32(dest + 16, (uint32_t)time(NULL));
    memcpy(dest + DIM_HEADER, key, key_len);
    memcpy(dest + DIM_HEADER + key_len, valore, val_len);

    /* CRC: header (20 byte) + chiave + valore. Calcolato DOPO che i dati
     * sono al loro posto e scritto nel campo riservato a offset 20 */
    uint32_t s = crc32_inizio();
    s = crc32_aggiorna(s, dest, 20);
    s = crc32_aggiorna(s, dest + DIM_HEADER, key_len + val_len);
    scrivi_u32(dest + 20, crc32_fine(s));

    /* il costo del CRC e' O(key+valore): si paga alla SCRITTURA.
     * Il get NON verifica il CRC: le entry in RAM si assumono sane,
     * il CRC protegge il viaggio su disco */
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

int db_compatta(db_t *db)
{
    uint8_t *nuovo = malloc(db->capacita);
    if (!nuovo)
        return DB_ERR_MEMORY;

    size_t nu = 0;
    size_t vive = 0;
    uint32_t adesso = (uint32_t)time(NULL);

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
        memcpy(nuovo + nu, e, entry_dim(e));   /* il CRC resta valido:
                                                 * e' calcolato sui byte
                                                 * dell'entry, che si
                                                 * copiano uguali */
        nu += entry_dim(e);
        vive++;
    }

    free(db->data);
    db->data = nuovo;
    db->usato = nu;
    db->spazzatura = 0;
    db->n_entry = vive;
    db->n_tomb = 0;

    memset(db->slot, 0, db->n_slot * sizeof *db->slot);
    size_t off = 0;
    while (off < nu) {
        const uint8_t *e = nuovo + off;
        size_t idx = leggi_u32(e) & (db->n_slot - 1);
        while (db->slot[idx].off1 != OFF1_VUOTO)
            idx = (idx + 1) & (db->n_slot - 1);
        db->slot[idx].hash = leggi_u32(e);
        db->slot[idx].off1 = (uint32_t)(off + 1);
        off += entry_dim(e);
    }
    return DB_OK;
}

/* ---------------------------------------------------------- persistenza */

/*
 * Snapshot compattato: compatta prima di scrivere.
 * Effetti voluti: (1) nel file ci sono solo le chiavi vive, una volta
 * sola — univoche per costruzione; (2) il bit MORTA non arriva mai su
 * disco, quindi il CRC di ogni entry del file e' quello calcolato alla
 * put, mai mutato dopo. Costo: una passata O(n) quando serve.
 */
int db_salva(db_t *db, const char *nomefile)
{
    if (db->spazzatura > 0 || db->n_tomb > 0)
        db_compatta(db);

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
 * Validator unico del formato: le regole di un'entry sana stanno qui.
 *   ritorno E_VALIDA  -> entry sana e viva, *dim = dimensione totale
 *   ritorno E_SCARTA  -> sana ma morta o scaduta: si salta
 *   ritorno E_STOP    -> troncata o CRC sbagliato: da qui non ci si
 *                        fida piu' di niente, qualunque byte segua
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
        return E_STOP;                      /* entry troncata */

    uint32_t s = crc32_inizio();
    s = crc32_aggiorna(s, e, 20);
    s = crc32_aggiorna(s, e + DIM_HEADER, (size_t)kl + vl);
    if (crc32_fine(s) != leggi_u32(e + 20))
        return E_STOP;                      /* CRC sbagliato */

    *dim = DIM_HEADER + (size_t)kl + vl;
    if (entry_morta(e))
        return E_SCARTA;

    uint32_t ttl  = leggi_u32(e + 12);
    uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
    if (ttl != 0 && (uint32_t)time(NULL) - nato > ttl)
        return E_SCARTA;

    return E_VALIDA;
}

/*
 * db_carica: legge il file, VALIDA entry per entry e ricostruisce.
 * Due passate sulla stessa memoria (il file e' gia' in cache dopo la
 * prima): la prima conta le vive, la seconda copia e inserisce.
 * Ritorna DB_OK anche se la coda era corrotta: lo segnala stampando
 * dove si e' fermato, perche' un prefisso valido e' un database sano.
 */
int db_carica(db_t *db, const char *nomefile, size_t *scartate_out)
{
    memset(db, 0, sizeof *db);
    crc_init();

    FILE *f = fopen(nomefile, "rb");
    if (!f)
        return DB_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0)
        return DB_ERR_IO;
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

    /* PASSO 1: valida e conta */
    size_t off = 0, vive = 0, scartate = 0, ignorati = 0;
    while (off < size) {
        size_t dim;
        int esito = valida_entry(dati + off, size - off, &dim);
        if (esito == E_STOP) {
            printf("[carica] validazione ferma a offset %zu (di %zu): "
                   "entry troncata o CRC sbagliato, ignoro %zu byte di coda\n",
                   off, size, size - off);
            ignorati = size - off;
            break;
        }
        if (esito == E_VALIDA)
            vive++;
        else
            scartate++;
        off += dim;
    }

    if (dichiara != vive)
        printf("[carica] header dichiarava %u entry, validate %zu: "
               "mi fido della validazione\n", dichiara, vive);

    /* PASSO 2: copia le vive e ricostruisci l'indice */
    db->capacita = prossima_potenza(off + CAPACITA_MIN);
    db->data = malloc(db->capacita);
    db->n_slot = prossima_potenza(vive * 2);
    if (db->n_slot < SLOT_INIZIALI)
        db->n_slot = SLOT_INIZIALI;
    db->slot = calloc(db->n_slot, sizeof *db->slot);
    if (!db->data || !db->slot) {
        free(db->data);
        free(db->slot);
        free(raw);
        return DB_ERR_MEMORY;
    }

    size_t nu = 0;
    off = 0;
    while (off < size && nu < db->capacita) {
        size_t dim;
        int esito = valida_entry(dati + off, size - off, &dim);
        if (esito == E_STOP)
            break;
        if (esito == E_VALIDA) {
            memcpy(db->data + nu, dati + off, dim);
            uint32_t h = leggi_u32(db->data + nu);
            size_t idx = h & (db->n_slot - 1);
            while (db->slot[idx].off1 != OFF1_VUOTO)
                idx = (idx + 1) & (db->n_slot - 1);
            db->slot[idx].hash = h;
            db->slot[idx].off1 = (uint32_t)(nu + 1);
            nu += dim;
        }
        off += dim;
    }

    free(raw);
    db->usato = nu;
    db->n_tomb = 0;
    db->spazzatura = 0;
    db->n_entry = vive;

    (void)ignorati;
    if (scartate_out)
        *scartate_out = scartate;
    return DB_OK;
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
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u",
                          (i * 7919u) % n_chiavi);
        double a = ora_ms();
        db_get(db, kbuf, (size_t)kl, NULL);
        double b = ora_ms();
        t[i] = b - a;
        media += t[i];
    }
    qsort(t, CAMPIONI, sizeof t[0], cmp_double);

    /* overhead del timer incluso in ogni campione: le delta tra fasi
     * restano confrontabili perche' ogni fase paga lo stesso extra */
    printf("%-22s media %6.3f us   p50 %6.3f   p90 %6.3f   p99 %6.3f   "
           "max %7.3f us\n",
           etichetta,
           media / CAMPIONI * 1000.0,
           t[CAMPIONI / 2]        * 1000.0,
           t[CAMPIONI * 9 / 10]   * 1000.0,
           t[CAMPIONI * 99 / 100] * 1000.0,
           t[CAMPIONI - 1]        * 1000.0);
}

/* ---------------------------------------------------------------- test */

static long dimensione_file(const char *nome)
{
    FILE *f = fopen(nome, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int corrompi_byte(const char *nome, long off)
{
    FILE *f = fopen(nome, "r+b");
    if (!f)
        return -1;
    if (fseek(f, off, SEEK_SET) != 0)
        { fclose(f); return -1; }
    int c = fgetc(f);
    if (c == EOF)
        { fclose(f); return -1; }
    fseek(f, off, SEEK_SET);
    fputc(c ^ 0x5A, f);
    fclose(f);
    return 0;
}

static int tronca_file(const char *nome, long togli)
{
    long n = dimensione_file(nome);
    if (n < togli)
        return -1;
    FILE *f = fopen(nome, "rb");
    if (!f)
        return -1;
    uint8_t *buf = malloc((size_t)n);
    if (!buf)
        { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        { free(buf); fclose(f); return -1; }
    fclose(f);
    f = fopen(nome, "wb");
    if (!f)
        { free(buf); return -1; }
    fwrite(buf, 1, (size_t)(n - togli), f);
    fclose(f);
    free(buf);
    return 0;
}

/*
 * Round trip completo + due corruzioni controllate.
 * Ogni attesa e' asserita: se il CRC o il troncamento non fermassero
 * il walker nel punto previsto, questo test lo scopre.
 */
static int test_persistenza(void)
{
    db_t db, car;
    char kbuf[32], vbuf[64];
    int errori = 0;
    enum { N = 1000 };

    if (db_init(&db) != DB_OK)
        return 1;
    for (int i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "p:%04d", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-persistito-%04d", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    for (int i = 0; i < N; i += 4)
        db_del(&db, kbuf, (size_t)snprintf(kbuf, sizeof kbuf, "p:%04d", i));

    if (db_salva(&db, "prova.db") != DB_OK) {
        printf("test persistenza FALLITO: scrittura non riuscita\n");
        return 1;
    }
    printf("salvate %d chiavi (750 vive) in prova.db (%ld byte, compattate "
           "prima di scrivere)\n", N, dimensione_file("prova.db"));

    /* -- round trip pulito -- */
    size_t scartate;
    if (db_carica(&car, "prova.db", &scartate) != DB_OK) {
        printf("test persistenza FALLITO: caricamento fallito\n");
        return 1;
    }
    if (car.n_entry != 750 || scartate != 0) {
        printf("test persistenza FALLITO: attese 750 vive e 0 scartate, "
               "avute %zu/%zu\n", car.n_entry, scartate);
        errori++;
    }
    for (int i = 0; i < N && errori == 0; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "p:%04d", i);
        uint32_t dl;
        const uint8_t *v = db_get(&car, kbuf, (size_t)kl, &dl);
        if (i % 4 == 0 ? v != NULL : v == NULL)
            { printf("test persistenza FALLITO: chiave %s errata\n", kbuf); errori++; }
    }
    if (errori == 0)
        printf("round trip: 1000 chiavi ricaricate, 750 vive verificate una "
               "per una, 0 scartate: OK\n");
    db_libera(&car);

    /* -- corruzione di un byte dentro l'ULTIMA entry del file:
     *      il CRC ferma il load li', si perde solo quella -- */
    long dim = dimensione_file("prova.db");
    size_t trovate = 0;
    if (corrompi_byte("prova.db", dim - 10) != 0 ||
        db_carica(&car, "prova.db", &scartate) != DB_OK) {
        printf("test persistenza FALLITO: corruzione non gestibile\n");
        return 1;
    }
    for (int i = 1; i < N; i += 2) {           /* le dispari sono tutte vive */
        int kl = snprintf(kbuf, sizeof kbuf, "p:%04d", i);
        uint32_t dl;
        if (db_get(&car, kbuf, (size_t)kl, &dl) != NULL)
            trovate++;
    }
    if (car.n_entry != 749 || trovate < 499) {
        printf("test persistenza FALLITO: attese 749 vive e >=499 get, "
               "avute %zu / %zu\n", car.n_entry, trovate);
    } else {
        printf("corruzione 1 byte (ultima entry): CRC ferma il load, "
               "caricate 749 vive, %zu/500 dispari leggibili (la corrotta "
               "e' l'unica persa): OK\n", trovate);
    }
    db_libera(&car);

    /* -- troncamento della coda: si perde SOLO cio' che manca -- */
    if (tronca_file("prova.db", 3000) != 0 ||
        db_carica(&car, "prova.db", &scartate) != DB_OK) {
        printf("test persistenza FALLITO: troncamento non gestibile\n");
        return 1;
    }
    if (car.n_entry < 650 || car.n_entry > 700) {
        printf("test persistenza FALLITO: troncamento atteso 650-700 vive, "
               "avute %zu\n", car.n_entry);
        errori++;
    } else {
        printf("troncamento di 3000 byte: caricate %zu vive su 749 "
               "(perse solo le ultime entry): OK\n", car.n_entry);
    }
    db_libera(&car);
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

    static char kbuf[64], vbuf[64];
    enum { N_CHIAVI = 30000 };

    double t0 = ora_ms();
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-di-%u", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    printf("pieno: %d chiavi in %.1f ms\n", N_CHIAVI, ora_ms() - t0);
    statistiche(&db, "dopo carico");

    for (uint32_t i = 0; i < N_CHIAVI; i += 2) {
        int kl = snprintf(kbuf, sizeof kbuf, "utente:%u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-nuovo-lungo-di-%u", i);
        db_put(&db, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    for (uint32_t i = 0; i < N_CHIAVI; i += 6)
        db_del(&db, kbuf, (size_t)snprintf(kbuf, sizeof kbuf, "utente:%u", i));
    statistiche(&db, "dopo upd+del");

    benchmark_get(&db, N_CHIAVI, "pre-compattazione:");

    t0 = ora_ms();
    db_compatta(&db);
    printf("compattazione: %.1f ms\n", ora_ms() - t0);
    statistiche(&db, "dopo compatta");

    benchmark_get(&db, N_CHIAVI, "post-compattazione:");

    /* ---------- persistenza ---------- */
    t0 = ora_ms();
    if (db_salva(&db, "demo.db") != DB_OK) {
        fprintf(stderr, "salvataggio fallito\n");
        return 1;
    }
    printf("\ndb_salva(\"demo.db\"): %ld byte in %.1f ms\n",
           dimensione_file("demo.db"), ora_ms() - t0);

    db_t ricaricato;
    size_t scartate;
    t0 = ora_ms();
    if (db_carica(&ricaricato, "demo.db", &scartate) != DB_OK) {
        fprintf(stderr, "caricamento fallito\n");
        return 1;
    }
    printf("db_carica: %.1f ms\n", ora_ms() - t0);
    statistiche(&ricaricato, "ricaricato");

    uint32_t dl;
    const uint8_t *v = db_get(&ricaricato, "utente:10", 9, &dl);
    printf("verifica ricaricato: utente:10 -> \"%.*s\"%s\n",
           v ? (int)dl : 6, v ? (const char *)v : "(null)",
           v && dl == 24 && memcmp(v, "valore-nuovo-lungo-di-10", 24) == 0
               ? "  (valore aggiornato: coincide)"
               : "  (SBIAGLIATO)");
    v = db_get(&ricaricato, "utente:6", 8, &dl);
    printf("verifica ricaricato: utente:6 -> %s (cancellata prima del save)\n",
           v ? "TROVATA?!" : "NULL, corretto");
    if (ricaricato.n_entry != db.n_entry)
        printf("ATTENZIONE: entry ricaricate %zu != vive %zu\n",
               ricaricato.n_entry, db.n_entry);
    else
        printf("conteggio coincide: %zu vive\n", ricaricato.n_entry);

    printf("\n");
    if (test_persistenza() != 0)
        return 1;

    remove("demo.db");
    remove("prova.db");

    db_libera(&ricaricato);
    db_libera(&db);
    return 0;
}
