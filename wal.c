/*
 * wal.c — il write-ahead log del capitolo 13. Il contratto [W1]..[W6]
 * vive in wal.h, scritto prima di questo file: qui c'e' solo la
 * meccanica che lo mantiene.
 *
 * Il segmento e' UN file append-only: niente indici, niente offset
 * salvati, niente contatori — la fonte di verita' e' il file, e ogni
 * dato derivato (il bilancio, il replay) si ricostruisce da lui [I1
 * di famiglia]. Il motore resta opaco: il WAL parla solo con l'API
 * pubblica di motore.h.
 */
#include "wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define sincronizza(fd)  _commit((fd))
#define tronca(f, pos)   (_chsize_s(_fileno((f)), (long long)(pos)) == 0 \
                          ? 0 : -1)
#else
#include <sys/types.h>                  /* off_t: la CI ha scoperto che
                                         * unistd.h non basta (gcc 15) */
#include <unistd.h>
#define sincronizza(fd)  fsync((fd))
#define tronca(f, pos)   (ftruncate(fileno((f)), (off_t)(pos)) == 0 \
                          ? 0 : -1)
#endif

#define WAL_HEADER 21u

struct wal {
    FILE *f;
    char *percorso;      /* serve al taglio: riapre dopo la creazione */
};

/* ------------------------------------------------------------- CRC-32 */
/* lo stesso polinomio del motore (capitolo 4): il contratto e' quello,
 * la tabella si ricostruisce qui perche' il motore non la esporta */

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

/* ------------------------------------------------- little-endian u32 */

static void scrivi_u32(uint8_t *dest, uint32_t v)
{
    dest[0] = (uint8_t)(v);
    dest[1] = (uint8_t)(v >> 8);
    dest[2] = (uint8_t)(v >> 16);
    dest[3] = (uint8_t)(v >> 24);
}

static uint32_t leggi_u32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

/* ------------------------------------------------------------- il record */

typedef struct {
    uint8_t        op;
    uint32_t       klen, vlen, ttl, tick;
    const uint8_t *chiave;
    const uint8_t *valore;
    size_t         dim;      /* byte consumati dal record (header incluso) */
} wal_record_t;

/* il validatore del prefisso [W3]: 1 = record sano, 0 = da qui non ci
 * si fida. Lunghezza bugiarda o CRC rotto fermano il replay sul posto */
static int leggi_record(const uint8_t *raw, size_t n, size_t off,
                        wal_record_t *r)
{
    size_t restanti = n - off;
    if (restanti < WAL_HEADER)
        return 0;
    const uint8_t *h = raw + off;
    r->klen  = leggi_u32(h + 0);
    r->vlen  = leggi_u32(h + 4);
    r->op    = h[8];
    r->ttl   = leggi_u32(h + 9);
    r->tick  = leggi_u32(h + 13);
    uint32_t crc = leggi_u32(h + 17);

    size_t corpo = (size_t)r->klen + r->vlen;
    if (corpo > restanti - WAL_HEADER)
        return 0;                       /* lunghezza bugiarda: coda monca */

    uint32_t s = crc32_inizio();
    s = crc32_aggiorna(s, h, 17);
    s = crc32_aggiorna(s, h + WAL_HEADER, corpo);
    if (crc32_fine(s) != crc)
        return 0;                       /* record rotto: si ferma qui [I5] */

    r->chiave = h + WAL_HEADER;
    r->valore = h + WAL_HEADER + r->klen;
    r->dim    = WAL_HEADER + corpo;
    return 1;
}

/* --------------------------------------------------------- il segmento */

wal_t *wal_apri(const char *percorso)
{
    crc_init();
    FILE *f = fopen(percorso, "ab+");   /* append: le scritture non
                                         * tornano mai indietro */
    if (!f)
        return NULL;
    wal_t *w = malloc(sizeof *w);
    if (!w)
        { fclose(f); return NULL; }
    size_t n = strlen(percorso);
    w->percorso = malloc(n + 1);
    if (!w->percorso)
        { fclose(f); free(w); return NULL; }
    memcpy(w->percorso, percorso, n + 1);
    w->f = f;
    return w;
}

void wal_chiudi(wal_t *w)
{
    if (!w)
        return;
    fclose(w->f);
    free(w->percorso);
    free(w);
}

/* il trittico [W2]: fwrite + fflush + fsync. Il record e' inchiostrato
 * solo quando fsync torna: prima di allora vale il prefisso sicuro */
static int wal_scrivi(wal_t *w, uint8_t op, const char *chiave, size_t klen,
                      const char *valore, size_t vlen,
                      uint32_t ttl, uint32_t tick)
{
    if (klen > 0xFFFFFFFFu || vlen > 0xFFFFFFFFu)
        return WAL_ERR_FORMATO;         /* l'header e' u32: il resto e' bugia */

    uint8_t h[WAL_HEADER];
    scrivi_u32(h + 0, (uint32_t)klen);
    scrivi_u32(h + 4, (uint32_t)vlen);
    h[8] = op;
    scrivi_u32(h + 9, ttl);
    scrivi_u32(h + 13, tick);

    uint32_t s = crc32_inizio();
    s = crc32_aggiorna(s, h, 17);
    s = crc32_aggiorna(s, (const uint8_t *)chiave, klen);
    if (vlen)
        s = crc32_aggiorna(s, (const uint8_t *)valore, vlen);
    scrivi_u32(h + 17, crc32_fine(s));

    if (fwrite(h, 1, sizeof h, w->f) != sizeof h ||
        fwrite(chiave, 1, klen, w->f) != klen ||
        (vlen && fwrite(valore, 1, vlen, w->f) != vlen))
        return WAL_ERR_IO;
    if (fflush(w->f) != 0 || sincronizza(fileno(w->f)) != 0)
        return WAL_ERR_IO;              /* il trittico, non il solo fsync */
    return WAL_OK;
}

int wal_put(wal_t *w, const char *chiave, size_t klen,
            const char *valore, size_t vlen, uint32_t ttl, uint32_t tick)
{
    return wal_scrivi(w, WAL_OP_PUT, chiave, klen, valore, vlen, ttl, tick);
}

int wal_del(wal_t *w, const char *chiave, size_t klen, uint32_t tick)
{
    return wal_scrivi(w, WAL_OP_DEL, chiave, klen, "", 0, 0, tick);
}

int wal_taglia(wal_t *w)
{
    if (fflush(w->f) != 0)
        return WAL_ERR_IO;
    if (tronca(w->f, 0) != 0)
        return WAL_ERR_IO;
    if (sincronizza(fileno(w->f)) != 0)
        return WAL_ERR_IO;              /* il taglio e' durabile o non e' */
    return WAL_OK;
}

/* ------------------------------------------------------------- il replay */

size_t wal_riavvolgi(wal_t *w, motore_t *m, size_t *scartati)
{
    if (scartati)
        *scartati = 0;
    if (fseek(w->f, 0, SEEK_END) != 0)
        return (size_t)-1;
    long dim = ftell(w->f);
    if (dim < 0)
        return (size_t)-1;
    if (fseek(w->f, 0, SEEK_SET) != 0)
        return (size_t)-1;

    uint8_t *raw = malloc(dim > 0 ? (size_t)dim : 1);
    if (!raw)
        return (size_t)-1;
    if (dim > 0 && fread(raw, 1, (size_t)dim, w->f) != (size_t)dim)
        { free(raw); return (size_t)-1; }

    size_t n = (size_t)dim, off = 0, applicati = 0;
    while (off < n) {
        wal_record_t r;
        if (!leggi_record(raw, n, off, &r))
            break;                      /* prefisso sicuro [W3] */

        /* il tick registrato avanza l'orologio PRIMA della put [W4]:
         * il nato ricostruito e' quello vero, non quello del replay */
        if (r.tick > motore_adesso(m))
            motore_avanza(m, r.tick);

        if (r.op == WAL_OP_PUT) {
            if (motore_put(m, (const char *)r.chiave, r.klen,
                           (const char *)r.valore, r.vlen, r.ttl) == MOTORE_OK)
                applicati++;
            /* rifiutata (TTL oltre finestra, memoria): si consuma e
             * non si conta — la validazione del vivo, uguale al morto */
        } else if (r.op == WAL_OP_DEL) {
            motore_del(m, (const char *)r.chiave, r.klen);
            applicati++;                /* MANCA e' legale [W5] */
        }
        /* op sconosciuta col CRC sano: non puo' nascere da wal_scrivi;
         * si consuma e si dichiara qui, silenzio invece di invenzione */

        off += r.dim;
    }

    if (off < n) {
        /* la coda non valida si dichiara e si tronca [W3]: i record
         * nuovi non finiscono mai dietro una coda mai leggibile */
        if (scartati)
            *scartati = n - off;
        if (fflush(w->f) != 0 || tronca(w->f, (long)off) != 0)
            { free(raw); return (size_t)-1; }
    }
    free(raw);
    return applicati;
}

/* ----------------------------------------------------------- il bilancio */

void wal_bilancio(wal_t *w, size_t *byte, size_t *record)
{
    size_t b = 0, r = 0;
    if (fseek(w->f, 0, SEEK_END) == 0) {
        long dim = ftell(w->f);
        if (dim > 0)
            b = (size_t)dim;
        if (b > 0 && fseek(w->f, 0, SEEK_SET) == 0) {
            uint8_t *raw = malloc(b);
            if (raw && fread(raw, 1, b, w->f) == b) {
                size_t off = 0;
                while (off < b) {
                    wal_record_t rec;
                    if (!leggi_record(raw, b, off, &rec))
                        break;          /* coda non valida: non e' un record */
                    r++;
                    off += rec.dim;
                }
            }
            free(raw);
        }
    }
    if (byte)
        *byte = b;
    if (record)
        *record = r;
}
