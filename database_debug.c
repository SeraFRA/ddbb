/*
 * database_debug.c
 * Versione debuggata di database.c, mantenendo la stessa architettura:
 * database = array contiguo di entry a dimensione variabile.
 *
 * Layout di ogni entry (per costruzione, senza padding):
 *   [0..4)   hash      (0 = slot libero / fine dati)
 *   [4..8)   key_len
 *   [8..12)  data_len
 *   [12..16) ttl
 *   [16..16+key_len)        chiave
 *   [16+key_len .. +data_len) valore
 *
 * Ogni correzione rispetto a database.c e' marcata con "FIX:".
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DB_SIZE     500u   /* FIX: niente numeri magici sparsi nel codice */
#define DIM_HEADER  16u    /* FIX: header = 4 campi x 4 byte = 16, non 8 */

/* Codici di errore espliciti invece di funzioni void che non possono fallire */
enum {
    DB_OK           =  0,
    DB_ERR_MEMORY   = -1,  /* malloc fallita */
    DB_ERR_PIENO    = -2,  /* nessuno slot libero abbastanza grande */
    DB_ERR_MANCA    = -3   /* chiave non trovata */
};

typedef struct {
    uint32_t len;
    char frase[];
} stringa_t;

typedef struct {
    uint32_t hash;
    uint32_t key_len;
    uint32_t data_len;
    uint32_t ttl;
    uint8_t data[];        /* chiave (key_len byte) + valore (data_len byte) */
} dic_entry;

static void errore(const char *msg)
{
    fprintf(stderr, "errore: %s\n", msg);
    exit(EXIT_FAILURE);
}

/*
 * FIX: leggi/scrivi u32 tramite helper. L'offset avanza da solo dentro il
 * chiamante: elimina la meta' degli errori "entry += 4 su puntatore struct".
 * memcpy resta preferibile al cast *(uint32_t*)p perche' il buffer di byte
 * non garantisce l'allineamento.
 */
static uint32_t leggi_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* FIX: hash FNV-1a invece della somma dei caratteri.
 * La somma dava lo stesso hash per tutti gli anagrammi ("ab" == "ba"). */
static uint32_t calcolo_hash(const stringa_t *s)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < s->len; i++) {
        h ^= (uint32_t)(uint8_t)s->frase[i];
        h *= 16777619u;
    }
    return h;
}

/* FIX: le struct con flexible array member vanno allocate con
 * sizeof(struct) + lunghezza. Sullo stack "stringa_t s;" riserva solo 4
 * byte (len) e ogni scrittura in frase[] e' un overflow silenzioso. */
static stringa_t *stringa_nuova(const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    stringa_t *p = malloc(sizeof(*p) + len);
    if (!p)
        return NULL;
    p->len = len;
    memcpy(p->frase, s, len);
    return p;
}

/* FIX: firma const stringa_t* invece di stringa_t per valore:
 * una struct con FAM passata per valore copia solo il campo len. */
static void printstringa(const stringa_t *p)
{
    for (uint32_t i = 0; i < p->len; i++)
        putchar(p->frase[i]);
}

/* Costruisce un'entry gia' serializzata: si compilano i CAMPI e si copia
 * il payload qui, non si cammina a byte con "entry += 4". */
static dic_entry *crea_entry(const stringa_t *key, const stringa_t *valore, uint32_t ttl)
{
    dic_entry *e = malloc(sizeof(*e) + key->len + valore->len);
    if (!e)
        return NULL;

    e->hash     = calcolo_hash(key);
    e->key_len  = key->len;
    e->data_len = valore->len;
    e->ttl      = ttl;   /* FIX: il ttl ora entra davvero nell'entry (prima: ignorato) */

    /* FIX: e->data e' il puntatore giusto (non "&data"), e il valore
     * parte a offset key_len, dopo la chiave, come nel layout. */
    memcpy(e->data,                    key->frase,    key->len);
    memcpy(e->data + key->len,         valore->frase, valore->len);
    return e;
}

/* FIX: il loop originale cercava "hash uguale" e usciva solo trovandolo:
 * con database vuoto camminava oltre i 500 byte per sempre. Ora cerca
 * lo slot libero (hash == 0) e controlla sempre i confini del buffer. */
static int salva_in_database(const dic_entry *e, uint8_t *db, size_t dim_db)
{
    size_t off = 0;

    while (off + DIM_HEADER <= dim_db) {
        uint32_t h = leggi_u32(db + off);

        if (h == 0) {   /* slot libero: ci sta l'entry? */
            size_t dim_entry = DIM_HEADER + e->key_len + e->data_len;
            if (off + dim_entry > dim_db)
                return DB_ERR_PIENO;
            memcpy(db + off, e, dim_entry);
            printf("[debug] entry scritta a offset %zu (%zu byte)\n",
                   off, dim_entry);
            return DB_OK;
        }

        /* slot occupato: salta l'intera entry usando le sue lunghezze */
        size_t dim_entry = DIM_HEADER
                        + leggi_u32(db + off + 4)
                        + leggi_u32(db + off + 8);
        if (off + dim_entry > dim_db)   /* entry troncata: buffer corrotto */
            return DB_ERR_PIENO;
        off += dim_entry;
    }
    return DB_ERR_PIENO;
}

/* Ricerca: hash uguale NON basta (collisioni), si confronta anche la chiave */
static int leggi_valore(const stringa_t *key, const uint8_t *db, size_t dim_db)
{
    size_t   off     = 0;
    uint32_t attesa  = calcolo_hash(key);

    while (off + DIM_HEADER <= dim_db) {
        uint32_t h  = leggi_u32(db + off);
        uint32_t kl = leggi_u32(db + off + 4);
        uint32_t dl = leggi_u32(db + off + 8);

        if (h == 0)
            return DB_ERR_MANCA;   /* fine dei dati: smetti di camminare */

        if (h == attesa && kl == key->len &&
            memcmp(db + off + DIM_HEADER, key->frase, kl) == 0) {
            printf("[debug] trovata a offset %zu -> valore \"", off);
            for (uint32_t i = 0; i < dl; i++)
                putchar(db[off + DIM_HEADER + kl + i]);
            printf("\"\n");
            return DB_OK;
        }
        off += DIM_HEADER + kl + dl;
    }
    return DB_ERR_MANCA;
}

/* Strumento di debug: dump del contenuto del database entry per entry */
static void dump_database(const uint8_t *db, size_t dim_db)
{
    size_t off = 0;
    printf("---- dump database (%zu byte) ----\n", dim_db);
    while (off + DIM_HEADER <= dim_db) {
        uint32_t h   = leggi_u32(db + off);
        if (h == 0)
            break;
        uint32_t kl  = leggi_u32(db + off + 4);
        uint32_t dl  = leggi_u32(db + off + 8);
        uint32_t ttl = leggi_u32(db + off + 12);
        printf("[off=%3zu] hash=%08x key_len=%u data_len=%u ttl=%u "
               "key=\"%.*s\" valore=\"%.*s\"\n",
               off, h, kl, dl, ttl,
               (int)kl, (char *)(db + off + DIM_HEADER),
               (int)dl, (char *)(db + off + DIM_HEADER + kl));
        off += DIM_HEADER + kl + dl;
    }
    printf("---- dati validi fino a offset %zu ----\n\n", off);
}

/* Strumento di debug: dump esadecimale dei byte grezzi di una entry */
static void dump_hex(const char *etichetta, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    printf("%s (%zu byte):\n", etichetta, n);
    for (size_t i = 0; i < n; i++)
        printf("%02x%s", p[i], (i % 16 == 15 || i + 1 == n) ? "\n" : " ");
    printf("\n");
}

int main(void)
{
    uint8_t database[DB_SIZE];
    memset(database, 0, sizeof database);   /* FIX: memset, non memcpy(_, 0, _) */

    stringa_t *chiave   = stringa_nuova("utente:1");
    stringa_t *valore   = stringa_nuova("francesco");
    stringa_t *chiave2  = stringa_nuova("citta:capitale");
    stringa_t *valore2 = stringa_nuova("roma");
    if (!chiave || !valore || !chiave2 || !valore2)
        errore("allocazione stringa fallita");

    /* FIX: ttl con valore definito: leggere una variabile non inizializzata e' UB */
    uint32_t ttl = 60u;

    dic_entry *e1 = crea_entry(chiave, valore, ttl);
    dic_entry *e2 = crea_entry(chiave2, valore2, ttl);
    if (!e1 || !e2)
        errore("allocazione entry fallita");

    dump_hex("entry 1 in memoria", e1, DIM_HEADER + chiave->len + valore->len);

    if (salva_in_database(e1, database, sizeof database) != DB_OK)
        errore("database pieno");
    if (salva_in_database(e2, database, sizeof database) != DB_OK)
        errore("database pieno");

    dump_database(database, sizeof database);

    printf("ricerca \"utente:1\": ");
    if (leggi_valore(chiave, database, sizeof database) != DB_OK) {
        printf("chiave \"");
        printstringa(chiave);
        printf("\" NON trovata\n");
    }

    stringa_t *assente = stringa_nuova("chiave-inesistente");
    printf("ricerca \"");
    if (assente)
        printstringa(assente);
    printf("\": ");
    if (assente && leggi_valore(assente, database, sizeof database) != DB_OK)
        printf("NON trovata (corretto)\n");

    free(e1);
    free(e2);
    free(chiave);
    free(valore);
    free(chiave2);
    free(valore2);
    free(assente);

    return 0;
}
