/*
 * motore.c — il motore unico. DIECI FILE, UN SISTEMA.
 *
 * =====================================================================
 * COSA E' QUESTO FILE
 * =====================================================================
 * Il progetto e' nato come un array di valori contigui ed e' cresciuto
 * un'idea per file: indice hash, tombstone e compattazione, CRC e
 * disco, indice appreso, memoria associativa, anello di scadenze,
 * filtri di appartenenza. Dieci capitoli separati. Questo file li
 * fonde in UN motore con UNA API (motore.h) e UN contratto.
 *
 * La colonna vertebrale e' il decimo file: l'arena e' la verita',
 * tutto il resto e' DERIVATO ricostruito da una funzione sola (derivo).
 * Attorno, tre port che avevano dimostrato di meritare il posto:
 *
 *   - dal file 6 (learned): la COMPAATTAZIONE ORDINA l'arena per chiave
 *     (fisica identica, geometria nuova: e' cio' che un merge LSM fa),
 *     e il RECINTO (512 campioni = offset) serve l'intervallo: la prima
 *     query del progetto che un indice hash NON SA FARE.
 *   - dal file 8 (ring): il TIMING WHEEL: la scadenza come LUOGO, non
 *     come calcolo. Con ri-validazione sui byte: chi non e' davvero
 *     scaduto resta al suo posto e brucia al giro giusto.
 *   - dal file 9 (quotient): solo il BLOOM (double hashing post-fix):
 *     la porta del "certamente no" davanti allo slot.
 *
 * =====================================================================
 * IL CONTRATTO (estende I1..I6 del decimo file)
 * =====================================================================
 *  [I7] OGNI ENTRY VIVA E' LA CORRENTE. Ogni morte (delete, scadenza
 *       pigra, sorpasso della put, ombra del replay) marca il bit
 *       MORTA DI ENTRATA (vedi [I3] e la lezione [T9]). Conseguenza
 *       potente: motore_intervallo e' DUE PASSEGGIATE DI BYTE, senza
 *       un solo lookup — un'entry non-morta e' per legge la versione
 *       valida della sua chiave.
 *  [I8] OROLOGIO UNICO. Niente orologio da muro, MAI: il tempo e'
 *       un intero che il chiamante avanza (avanza) o fissa (apri_da).
 *       Semantica scadenza inclusiva e unica: morta quando
 *       adesso >= nato + ttl — ovunque (put, get, validatore, anello).
 *       (Il decimo usava ">" strett: la differenza e' dichiarata.)
 *  [I9] IL BLOOM NON PUO' DIRE BUGIE IN NEGATIVO. "0" = certamente
 *       non c'e' (mai un falso negativo su una chiave viva),
 *       "1" = probabilmente. Mantenuto: insert in derivo, una
 *       ricostruzione a ogni compattazione. Il prezzo e' onesto: le
 *       cancellate rispondono "1 forse" fino alla prossima
 *       compattazione — e' un filtro, non un indice.
 *  [I10] IL DISCO E' QUELLO DEL DECIMO. Stesso MAGIA PDB1, stessa
 *       versione, stesso validatore: i file scritti dal decimo e dal
 *       motore si leggono a vicenda per costruzione.
 *
 * (I1 fonte unica, I2 identita' della voce, I3 morte atomica,
 *  I4 il disco vede solo l'ora viva e intatta, I5 prefisso sicuro,
 *  I6 risposte vive col valore: invariati dal decimo file, e ogni
 *  funzione qui li incassa o li paga.)
 *
 * =====================================================================
 * OBITUARIO: COSA RESTA FUORI E PERCHE' (la scelta e' la lezione)
 * =====================================================================
 *  - BACKSHIFT DELLA DELETE (file 8): il motore tiene tombstone e li
 *    dissolve alla compattazione (deriva_db). Il backshift restaura
 *    le catene di probing vivendo SENZA compattazione: il file 8 non
 *    ne aveva, il motore si'. Meno interazioni sottili, stesso serve.
 *  - QUOTIENT FILTER (file 9): e' un INDICE di appartenenza per
 *    sistemi le cui entry vivono altrove (o su disco). Il motore ha
 *    gia' l'indice esatto e la porta Bloom: nessuna query ha il
 *    problema che il QF risolve.
 *  - SPLINE LINEARE (file 6): il suo compito era indovinare lo SLOT
 *    di partenza del probing (uno spazio ricostruito); qui il modello
 *    approssima POSIZIONI VERE dell'arena ordinata, e il recinto a
 *    CHIAVE PIENA (ricerca binaria sui campioni, passeggiata dentro
 *    lo span) e' esatto per costruzione: l'interpolazione non
 *    comprerebbe correttezza (già garantita) ne' memoria.
 * =====================================================================
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "motore.h"

/* ------------------------------------------------ costanti di servizio */
#define DIM_HEADER     24u
#define SLOT_INIZIALI  ((size_t)1 << 10)
#define LOAD_FACTOR_10 7
#define CAPACITA_MIN   ((size_t)1 << 16)
#define CAPACITA_MAX   ((size_t)1 << 28)

#define OFF1_VUOTO     0u
#define SLOT_TOMBSTONE 0xFFFFFFFFu
#define VOCE_MORTA     0u
#define MORTA_BIT      0x80000000u

/* semantiche (file 7/10) */
#define N_BANKS        32
#define N_RIGHE        4
#define N_BUCKET_BN    17
#define MAX_CANDIDATI  8192
#define MAX_GRAMMI     66
#define TOPLIM         8
#define VOCI_INIZIALI  256

/* timing wheel (file 8) */
#define N_TICK          4096

/* recinto (nuovo, dal spirito del file 6) */
#define N_RECINTO       512

/* bloom (file 9) */
#define BLOOM_K         7u

/* formato del file: identico al decimo [I10] */
#define FORMATO_VERSIONE 1u
static const uint8_t MAGIA[4] = { 'P', 'D', 'B', '1' };

typedef struct {
    uint32_t fp;
    uint32_t id1;     /* SLOT_TOMBSTONE / 0 vuoto / voce id + 1 */
} slot_t;

typedef struct {
    uint32_t off1;    /* entry corrente + 1; 0 = morta [I3]        */
    uint32_t gram_off, gram_cnt;
} voce_t;

typedef struct { uint32_t id, prox; } ruota_nodo_t;

struct motore {
    /* la verita' [I1] */
    uint8_t *data;
    size_t    capacita, usato, spazzatura;
    slot_t  *slot;
    size_t   n_slot, n_entry, n_tomb;
    /* orologio del chiamante [I8] */
    uint32_t adesso;
    uint64_t scadute_batch;
    /* derivato semantico (file 7/10) */
    voce_t   *voci;
    size_t    n_voci, capacita_voci;
    uint32_t *firma, *gram_arena;
    size_t    gram_usati;
    uint32_t *testa[N_BANKS], *prossimo[N_BANKS], *visto;
    uint32_t  marca;
    /* bloom [I9] */
    uint8_t *bloom_bit;
    size_t   bloom_m;
    uint64_t sonde_risparmiate;
    /* timing wheel [I8] */
    uint32_t        ruota_testa[N_TICK];
    ruota_nodo_t    *ruota_pool;
    size_t           ruota_usati, ruota_capacita;
    /* recinto + confine ordine [I7] */
    uint32_t recinto[N_RECINTO];
    size_t   n_recinto;
    size_t   ordinato_fino;    /* [0, ordinato_fino) e' in ordine    */
};

/* ------------------------------------------------------------- utilita' */

static uint32_t leggi_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static void    scrivi_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }

static uint32_t mix32(uint32_t x)
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

/* ------------------------------------- bigrammi posizionali (file 7) */

static uint32_t gram_hash(char a, char b, uint32_t pos)
{
    uint32_t base = (((uint32_t)(uint8_t)a << 8) | (uint8_t)b)
                  ^ (pos * 0x9e3779b9u);
    return mix32(base + 0xabcde987u);
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
    return mix32(base ^ (k * 0x9e3779b9u + 0x85ebca6bu));
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
            bh = mix32(bh ^ mini[r]);
        firma[b] = bh;
    }
}

/* ---------------------------------------------- bloom (file 9, post-fix) */

/* l'unica verita' su una chiave: fingerprint a 64 bit (file 9) */
static uint64_t fp64(const char *s, size_t len)
{
    uint32_t a = 2166136261u, b = 0x9e3779b9u;
    for (size_t i = 0; i < len; i++) {
        a ^= (uint32_t)(uint8_t)s[i];
        a *= 16777619u;
        b = mix32(b + (uint32_t)(uint8_t)s[i] * (uint32_t)(i + 1));
    }
    uint64_t h1 = mix32(a);
    uint64_t h2 = mix32(b ^ a) | 1u;
    return (h1 << 32) ^ h2 ^ mix32((uint32_t)(h1 * h2));
}

/* double hashing: h_i = (h1 + i*h2) % m — due hash indipendenti,
 * la lezione del file 9 (14.7% -> 0.85% di falsi positivi) */
static int bloom_contiene(const motore_t *m, const char *s, size_t len)
{
    uint64_t fp = fp64(s, len);
    uint64_t h1 = (uint64_t)mix32((uint32_t)(fp >> 32)) << 16
                ^ mix32((uint32_t)fp);
    uint64_t h2 = mix32((uint32_t)(fp ^ (fp >> 32))) | 1u;
    uint64_t nn = (uint64_t)m->bloom_m;
    for (uint32_t i = 0; i < BLOOM_K; i++) {
        uint64_t h = (h1 + (uint64_t)i * h2) % nn;
        if (!(m->bloom_bit[(size_t)(h >> 3)] & (1u << (h & 7))))
            return 0;
    }
    return 1;
}

static void bloom_inserisci(motore_t *m, const char *s, size_t len)
{
    uint64_t fp = fp64(s, len);
    uint64_t h1 = (uint64_t)mix32((uint32_t)(fp >> 32)) << 16
                ^ mix32((uint32_t)fp);
    uint64_t h2 = mix32((uint32_t)(fp ^ (fp >> 32))) | 1u;
    uint64_t nn = (uint64_t)m->bloom_m;
    for (uint32_t i = 0; i < BLOOM_K; i++) {
        uint64_t h = (h1 + (uint64_t)i * h2) % nn;
        m->bloom_bit[(size_t)(h >> 3)] |= (uint8_t)(1u << (h & 7));
    }
}

/* ridimensiona e AZZERA. Chiamare SOLO subito prima di una ricostruzione
 * completa (compattazione / load): i bit non migrano [I9] */
static int bloom_ridimensiona(motore_t *m, size_t bit)
{
    if (bit < 1024)
        bit = 1024;
    size_t bytes = (bit + 7) / 8;
    if (m->bloom_m != bit) {
        uint8_t *nb = calloc(bytes, 1);
        if (!nb)
            return MOTORE_ERR_MEMORIA;
        free(m->bloom_bit);
        m->bloom_bit = nb;
        m->bloom_m = bit;
    } else {
        memset(m->bloom_bit, 0, bytes);
    }
    return MOTORE_OK;
}

/* -------------------------------------------- timing wheel (file 8) */

/* spinge una voce nella cella della sua morte [I8]. I nodi non si
 * sganciano mai da soli: bruciano (o vengono sepolti come voci gia'
 * morte) al momento del fuoco, con ri-validazione sui BYTE */
static int ruota_prega(motore_t *m, uint32_t id, uint32_t scadenza)
{
    if (m->ruota_usati >= m->ruota_capacita) {
        size_t nuova = m->ruota_capacita ? m->ruota_capacita * 2 : 256;
        ruota_nodo_t *np = realloc(m->ruota_pool, nuova * sizeof *np);
        if (!np)
            return MOTORE_ERR_MEMORIA;
        m->ruota_pool = np;
        m->ruota_capacita = nuova;
    }
    uint32_t cella = scadenza & (N_TICK - 1);
    m->ruota_pool[m->ruota_usati].id   = id;
    m->ruota_pool[m->ruota_usati].prox = m->ruota_testa[cella];
    m->ruota_testa[cella] = (uint32_t)(m->ruota_usati + 1);
    m->ruota_usati++;
    return MOTORE_OK;
}

/* ------------------------------------------------------ morte atomica */

/*
 * L'UNICA morte [I3]: bit MORTA, spazzatura, voce a zero, contatore.
 * Le strade: delete, scadenza pigra al get/chiedi/ranking, fuoco
 * dell'anello. Lo slot NON si tocca qui (solo la del tombstone-a).
 */
static void muore(motore_t *m, size_t id)
{
    size_t off = m->voci[id].off1 - 1;
    segna_morta(m->data, off);
    m->spazzatura += entry_dim(m->data + off);
    m->voci[id].off1 = VOCE_MORTA;
    m->n_entry--;
}

/* -------------------------------------------------------- indice esatto */

/* il probe NON matcha mai una voce morta (la lezione [T2] del decimo:
 * altrimenti leggerebbe data[-1]) */
static size_t trova_slot(const motore_t *m, uint32_t hash,
                         const char *key, size_t key_len, int *trovata)
{
    size_t idx = hash & (m->n_slot - 1);
    size_t tomb = (size_t)-1;
    *trovata = 0;

    for (;;) {
        uint32_t id1 = m->slot[idx].id1;

        if (id1 == OFF1_VUOTO)
            return tomb == (size_t)-1 ? idx : tomb;

        if (id1 != SLOT_TOMBSTONE &&
            m->slot[idx].fp == hash &&
            m->voci[id1 - 1].off1 != VOCE_MORTA) {
            const uint8_t *e = m->data + m->voci[id1 - 1].off1 - 1;
            if (leggi_u32(e + 4) == key_len &&
                memcmp(e + DIM_HEADER, key, key_len) == 0) {
                *trovata = 1;
                return idx;
            }
        }
        if (id1 == SLOT_TOMBSTONE && tomb == (size_t)-1)
            tomb = idx;
        idx = (idx + 1) & (m->n_slot - 1);
    }
}

static int cresci_slot(motore_t *m)
{
    size_t nuovo_n = m->n_slot * 2;
    slot_t *nuovo = calloc(nuovo_n, sizeof *nuovo);
    if (!nuovo)
        return MOTORE_ERR_MEMORIA;
    for (size_t v = 0; v < m->n_slot; v++)
        if (m->slot[v].id1 != OFF1_VUOTO &&
            m->slot[v].id1 != SLOT_TOMBSTONE) {
            size_t idx = m->slot[v].fp & (nuovo_n - 1);
            while (nuovo[idx].id1 != OFF1_VUOTO)
                idx = (idx + 1) & (nuovo_n - 1);
            nuovo[idx] = m->slot[v];
        }
    free(m->slot);
    m->slot = nuovo;
    m->n_slot = nuovo_n;
    m->n_tomb = 0;
    printf("[motore] slot raddoppiati a %zu\n", nuovo_n);
    return MOTORE_OK;
}

/* ------------------------------------------------ crescite di memoria */

static int assicura_spazio(motore_t *m, size_t dim)
{
    if (dim <= m->capacita - m->usato)
        return MOTORE_OK;
    size_t nuova = m->capacita;
    while (dim > nuova - m->usato && nuova < CAPACITA_MAX)
        nuova *= 2;
    if (dim > nuova - m->usato)
        return MOTORE_ERR_PIENO;
    uint8_t *p = realloc(m->data, nuova);
    if (!p)
        return MOTORE_ERR_MEMORIA;
    m->data = p;
    m->capacita = nuova;
    return MOTORE_OK;
}

static int assicura_voci(motore_t *m)
{
    if (m->n_voci < m->capacita_voci)
        return MOTORE_OK;
    size_t nuova = m->capacita_voci * 2;

    voce_t *nv = realloc(m->voci, nuova * sizeof *nv);
    if (!nv) return MOTORE_ERR_MEMORIA;
    m->voci = nv;

    uint32_t *nf = realloc(m->firma, nuova * N_BANKS * sizeof *nf);
    if (!nf) return MOTORE_ERR_MEMORIA;
    m->firma = nf;

    uint32_t *ng = realloc(m->gram_arena, nuova * MAX_GRAMMI * sizeof *ng);
    if (!ng) return MOTORE_ERR_MEMORIA;
    m->gram_arena = ng;

    uint32_t *nvv = realloc(m->visto, nuova * sizeof *nvv);
    if (!nvv) return MOTORE_ERR_MEMORIA;
    memset(nvv + m->capacita_voci, 0, (nuova - m->capacita_voci) * sizeof *nvv);
    m->visto = nvv;

    for (uint32_t b = 0; b < N_BANKS; b++) {
        uint32_t *np = realloc(m->prossimo[b], nuova * sizeof *np);
        if (!np) return MOTORE_ERR_MEMORIA;
        memset(np + m->capacita_voci, 0,
               (nuova - m->capacita_voci) * sizeof *np);
        m->prossimo[b] = np;
    }
    m->capacita_voci = nuova;
    return MOTORE_OK;
}

/* ------------------------------------------------------------- derivo */

/*
 * derivo — la strada UNICA dell'ingresso [I1]: esatto, grammi, firme,
 * bucket, BLOOM e ANELLO passano tutti da qui. Tre chiamanti:
 *   derivo(m, off, 1) dalla put: la voce si gira sulla nuova e la
 *                    sorpassata muore con bit E conto [T9]
 *   derivo(m, off, 0) dal load: la voce resta alla prima, la copia in
 *                    piu' e' un'ombra che muore qui [T7]
 *   derivo(m, off, 0) dalla compattazione: ricostruzione dal nulla
 * Regge anche l'anello [I8]: ogni derivazione di una entry con ttl
 * prenota la morte della voce nella cella giusta — e il bloom [I9]:
 * ogni voce nata accende i suoi bit.
 * Se fallisce (solo memoria) nessun pubblicato era ancora visibile:
 * la put NON avanza usato [T4] e il mondo resta coerente.
 */
static int derivo(motore_t *m, size_t off, int aggiorna)
{
    const uint8_t *e = m->data + off;
    uint32_t fp   = leggi_u32(e);
    uint32_t klen = leggi_u32(e + 4);
    uint32_t ttl  = leggi_u32(e + 12);
    const char *key = (const char *)(e + DIM_HEADER);

    if ((m->n_entry + m->n_tomb + 1) * 10 > m->n_slot * LOAD_FACTOR_10)
        if (cresci_slot(m) != MOTORE_OK)
            return MOTORE_ERR_MEMORIA;

    int trovata;
    size_t idx = trova_slot(m, fp, key, klen, &trovata);

    if (trovata) {
        size_t id = (size_t)m->slot[idx].id1 - 1;
        if (aggiorna) {
            if (ttl != 0) {
                uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
                if (ruota_prega(m, (uint32_t)id, nato + ttl) != MOTORE_OK)
                    return MOTORE_ERR_MEMORIA;   /* nulla di pubblicato */
            }
            uint32_t vecchio = m->voci[id].off1;
            segna_morta(m->data, vecchio - 1);   /* [T9]               */
            m->spazzatura += entry_dim(m->data + vecchio - 1);
            m->voci[id].off1 = (uint32_t)(off + 1);
        } else {
            segna_morta(m->data, off);           /* ombra [T7]         */
            m->spazzatura += entry_dim(m->data + off);
        }
        return MOTORE_OK;
    }

    if (assicura_voci(m) != MOTORE_OK)
        return MOTORE_ERR_MEMORIA;

    size_t id = m->n_voci;
    uint32_t grammi[MAX_GRAMMI];
    uint32_t ng = costruisci_grammi(grammi, key, klen);

    /* lavori preparatori (possono sbagliare senza conseguenze) */
    memcpy(m->gram_arena + m->gram_usati, grammi, ng * sizeof *grammi);
    firma_di_grammi(m->firma + id * N_BANKS, grammi, ng);
    bloom_inserisci(m, key, klen);               /* [I9]               */
    if (ttl != 0) {
        uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
        if (ruota_prega(m, (uint32_t)id, nato + ttl) != MOTORE_OK)
            return MOTORE_ERR_MEMORIA;           /* grammi/firma in
                                                  * spicolo, voce mai
                                                  * nata: nessuno li
                                                  * leggera' mai */
    }

    /* pubblicazione (irreversibile) */
    m->gram_usati += ng;
    m->voci[id].off1     = (uint32_t)(off + 1);
    m->voci[id].gram_off = (uint32_t)(m->gram_usati - ng);
    m->voci[id].gram_cnt = ng;
    for (uint32_t b = 0; b < N_BANKS; b++) {
        size_t bh = m->firma[id * N_BANKS + b] & (((size_t)1 << N_BUCKET_BN) - 1);
        m->prossimo[b][id] = m->testa[b][bh];
        m->testa[b][bh] = (uint32_t)(id + 1);
    }
    if (m->slot[idx].id1 == SLOT_TOMBSTONE)
        m->n_tomb--;
    m->n_voci++;
    m->slot[idx].fp  = fp;
    m->slot[idx].id1 = (uint32_t)(id + 1);
    m->n_entry++;                                /* derivo possiede il contatore */
    return MOTORE_OK;
}

/*
 * deriva_db: azzera OGNI derivato e ricostruisce derivo per derivo,
 * camminando l'arena (la verita' non sta negli slot [I1]). Azzera anche
 * bloom e anello: sono derivati pure loro, oggi. Il recinto NO: nasce
 * solo dalla compattazione, che e' l'unica che produce l'ordine.
 */
static int deriva_db(motore_t *m)
{
    memset(m->slot, 0, m->n_slot * sizeof *m->slot);
    for (uint32_t b = 0; b < N_BANKS; b++)
        memset(m->testa[b], 0, ((size_t)1 << N_BUCKET_BN) * sizeof **m->testa);
    memset(m->ruota_testa, 0, sizeof m->ruota_testa);
    if (m->bloom_m)
        memset(m->bloom_bit, 0, (m->bloom_m + 7) / 8);
    m->n_entry = m->n_tomb = m->n_voci = 0;
    m->gram_usati = 0;
    m->ruota_usati = 0;

    size_t off = 0;
    while (off < m->usato) {
        if (derivo(m, off, 0) != MOTORE_OK)
            return MOTORE_ERR_MEMORIA;
        off += entry_dim(m->data + off);
    }
    return MOTORE_OK;
}

/* ------------------------------------------- recinto (parto dal file 6) */

static int confronta_chiave(const uint8_t *e, const char *k, size_t klen)
{
    uint32_t kl = leggi_u32(e + 4);
    size_t n = kl < klen ? kl : klen;
    int c = memcmp(e + DIM_HEADER, k, n);
    if (c)
        return c;
    if (kl == klen)
        return 0;
    return kl < klen ? -1 : 1;
}

/*
 * cerca_recinto: PRIMO campione con chiave >= da. La garanzia del
 * recinto: tutto cio' che sta PRIMA del campione precedente ha chiave
 * <= chiave del campione precedente < da. Quindi la passeggiata puo'
 * partire dal campione precedente senza mai perdersi un candidato:
 * esatto per costruzione (nessuna interpolazione richiesta: dopo la
 * lezione del file 6, qui la "feature" e' la CHIAVE PIENA).
 */
static size_t cerca_recinto(const motore_t *m, const char *da, size_t da_len)
{
    size_t lo = 0, hi = m->n_recinto;    /* primo campione >= da */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (confronta_chiave(m->data + m->recinto[mid], da, da_len) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo == 0 ? 0 : m->recinto[lo - 1];
}

static void costruisci_recinto(motore_t *m)
{
    m->n_recinto = 0;
    if (m->ordinato_fino == 0 || m->n_entry == 0)
        return;
    size_t vive = m->n_entry;
    size_t off = 0, i = 0, taken = 0;
    while (off < m->ordinato_fino && taken < N_RECINTO) {
        if (i * N_RECINTO >= taken * vive)
            m->recinto[taken++] = (uint32_t)off;
        off += entry_dim(m->data + off);
        i++;
    }
    m->n_recinto = taken;
}

/* ------------------------------------------------ la vita del motore */

static motore_t *alloca_motore(size_t stima_voci)
{
    motore_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    crc_init();

    m->capacita = CAPACITA_MIN;
    m->data = malloc(m->capacita);
    m->n_slot = SLOT_INIZIALI;
    m->slot = calloc(m->n_slot, sizeof *m->slot);

    m->capacita_voci = stima_voci < VOCI_INIZIALI ? VOCI_INIZIALI : stima_voci;
    m->voci       = malloc(m->capacita_voci * sizeof *m->voci);
    m->firma      = malloc(m->capacita_voci * N_BANKS * sizeof *m->firma);
    m->gram_arena = malloc(m->capacita_voci * MAX_GRAMMI * sizeof *m->gram_arena);
    m->visto      = calloc(m->capacita_voci, sizeof *m->visto);
    m->marca      = 1;

    if (!m->data || !m->slot || !m->voci || !m->firma ||
        !m->gram_arena || !m->visto)
        goto memoria;

    for (uint32_t b = 0; b < N_BANKS; b++) {
        m->testa[b]    = calloc((size_t)1 << N_BUCKET_BN, sizeof **m->testa);
        m->prossimo[b] = calloc(m->capacita_voci, sizeof **m->prossimo);
        if (!m->testa[b] || !m->prossimo[b])
            goto memoria;
    }

    if (bloom_ridimensiona(m, 12 * m->capacita_voci) != MOTORE_OK)
        goto memoria;

    m->adesso = 0;
    return m;

memoria:
    motore_chiudi(m);
    return NULL;
}

motore_t *motore_apri(void)
{
    return alloca_motore(VOCI_INIZIALI);
}

void motore_chiudi(motore_t *m)
{
    if (!m)
        return;
    free(m->data);
    free(m->slot);
    free(m->voci);
    free(m->firma);
    free(m->gram_arena);
    free(m->visto);
    for (uint32_t b = 0; b < N_BANKS; b++) {
        free(m->testa[b]);
        free(m->prossimo[b]);
    }
    free(m->bloom_bit);
    free(m->ruota_pool);
    free(m);
}

/* ------------------------------------------------------------ API base */

int motore_put(motore_t *m, const char *chiave, size_t klen,
               const char *valore, size_t vlen, uint32_t ttl)
{
    if (ttl > N_TICK)
        return MOTORE_ERR_TTL;           /* finestra dell'anello [I8] */

    size_t dim = DIM_HEADER + klen + vlen;
    int r = assicura_spazio(m, dim);
    if (r != MOTORE_OK)
        return r;

    size_t off = m->usato;
    uint8_t *dest = m->data + off;
    scrivi_u32(dest +  0, calcolo_hash(chiave, klen));
    scrivi_u32(dest +  4, (uint32_t)klen);
    scrivi_u32(dest +  8, (uint32_t)vlen);
    scrivi_u32(dest + 12, ttl);
    scrivi_u32(dest + 16, m->adesso);   /* l'orologio e' del chiamante */
    memcpy(dest + DIM_HEADER, chiave, klen);
    memcpy(dest + DIM_HEADER + klen, valore, vlen);

    uint32_t s = crc32_inizio();
    s = crc32_aggiorna(s, dest, 20);
    s = crc32_aggiorna(s, dest + DIM_HEADER, klen + vlen);
    scrivi_u32(dest + 20, crc32_fine(s));

    if (derivo(m, off, 1) != MOTORE_OK)
        return MOTORE_ERR_MEMORIA;       /* arena intatta [T4] */

    m->usato += dim;
    return MOTORE_OK;
}

const uint8_t *motore_get(motore_t *m, const char *chiave, size_t klen,
                          uint32_t *valore_len)
{
    /* la porta del "certamente no" [I9]: mai un falso negativo */
    if (!bloom_contiene(m, chiave, klen)) {
        m->sonde_risparmiate++;
        return NULL;
    }

    int trovata;
    size_t idx = trova_slot(m, calcolo_hash(chiave, klen), chiave, klen, &trovata);
    if (!trovata)
        return NULL;
    size_t id = (size_t)m->slot[idx].id1 - 1;

    const uint8_t *e = m->data + m->voci[id].off1 - 1;
    uint32_t ttl  = leggi_u32(e + 12);
    uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
    if (ttl != 0 && (uint32_t)(m->adesso - nato) >= ttl) {
        muore(m, id);                    /* cintura e bretelle [I8]   */
        return NULL;
    }
    if (valore_len)
        *valore_len = leggi_u32(e + 8);
    return e + DIM_HEADER + klen;
}

int motore_del(motore_t *m, const char *chiave, size_t klen)
{
    int trovata;
    size_t idx = trova_slot(m, calcolo_hash(chiave, klen), chiave, klen, &trovata);
    if (!trovata)
        return MOTORE_ERR_MANCA;
    size_t id = (size_t)m->slot[idx].id1 - 1;
    if (m->voci[id].off1 == VOCE_MORTA)
        return MOTORE_ERR_MANCA;         /* gia' scaduta per conto suo */
    muore(m, id);
    m->slot[idx].id1 = SLOT_TOMBSTONE;
    m->n_tomb++;
    return MOTORE_OK;
}

void motore_avanza(motore_t *m, uint32_t adesso)
{
    if (adesso < m->adesso)
        return;                          /* l'orologio non torna indietro */
    while (m->adesso < adesso) {
        m->adesso++;
        uint32_t t = m->adesso;
        uint32_t cella = t & (N_TICK - 1);
        uint32_t n1 = m->ruota_testa[cella];
        uint32_t prev1 = 0;
        while (n1 != 0) {
            ruota_nodo_t *nodo = &m->ruota_pool[n1 - 1];
            uint32_t next1 = nodo->prox;
            uint32_t id = nodo->id;
            int uccidi = 0, butta = 0;

            if (m->voci[id].off1 == VOCE_MORTA) {
                butta = 1;                    /* voce morta altrove    */
            } else {
                const uint8_t *e = m->data + m->voci[id].off1 - 1;
                uint32_t ttl  = leggi_u32(e + 12);
                uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
                if (ttl != 0 && (uint32_t)(t - nato) >= ttl)
                    uccidi = butta = 1;       /* scade ORA, sul serio  */
            }
            if (uccidi) {
                muore(m, id);                 /* l'unica morte [I3]    */
                m->scadute_batch++;
            }
            if (butta) {                      /* sgancio del nodo      */
                if (prev1 == 0)
                    m->ruota_testa[cella] = next1;
                else
                    m->ruota_pool[prev1 - 1].prox = next1;
            } else {
                prev1 = n1;                   /* vive: gira di nuovo  */
            }
            n1 = next1;
        }
    }
}

uint32_t motore_adesso(const motore_t *m) { return m->adesso; }

int motore_presente(motore_t *m, const char *chiave, size_t klen)
{
    return bloom_contiene(m, chiave, klen);
}

/* ------------------------------------------------- la compattazione */

/* qsort senza contesto portabile: single-thread dichiarato */
static const motore_t *g_m_per_confronto;

static int cmp_off_chiave(const void *pa, const void *pb)
{
    uint32_t oa = *(const uint32_t *)pa, ob = *(const uint32_t *)pb;
    const uint8_t *ea = g_m_per_confronto->data + oa;
    const uint8_t *eb = g_m_per_confronto->data + ob;
    uint32_t la = leggi_u32(ea + 4), lb = leggi_u32(eb + 4);
    size_t n = la < lb ? la : lb;
    int c = memcmp(ea + DIM_HEADER, eb + DIM_HEADER, n);
    if (c)
        return c;
    return (la > lb) - (la < lb);
}

/*
 * LA COMPAATTAZIONE CHE IMPARA: butta la spazzatura (fisica del file
 * 3) e ORDINA l'arena per chiave (geometria del merge LSM). Il recinto
 * nasce qui — solo l'ordine puo' essere campionato. E [I7] fa il
 * resto: un'entry sopravvissuta e' LA corrente della sua chiave.
 */
int motore_compatta(motore_t *m)
{
    uint32_t *offsets = malloc((m->n_entry + 1) * sizeof *offsets);
    if (!offsets)
        return MOTORE_ERR_MEMORIA;

    size_t vive = 0, off = 0;
    while (off < m->usato) {
        const uint8_t *e = m->data + off;
        size_t dim = entry_dim(e);
        if (!entry_morta(e)) {
            uint32_t ttl  = leggi_u32(e + 12);
            uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
            if (ttl == 0 || (uint32_t)(m->adesso - nato) < ttl) {
                offsets[vive++] = (uint32_t)off;
            }
        }
        off += dim;
    }

    uint8_t *nuovo = malloc(m->capacita);
    if (!nuovo)
        { free(offsets); return MOTORE_ERR_MEMORIA; }

    g_m_per_confronto = m;
    qsort(offsets, vive, sizeof *offsets, cmp_off_chiave);
    g_m_per_confronto = NULL;

    size_t nu = 0;
    for (size_t i = 0; i < vive; i++) {
        const uint8_t *e = m->data + offsets[i];
        size_t dim = entry_dim(e);
        memcpy(nuovo + nu, e, dim);      /* i byte viaggiano col CRC   */
        nu += dim;
    }

    free(m->data);
    free(offsets);
    m->data = nuovo;
    m->usato = nu;
    m->spazzatura = 0;
    m->n_tomb = 0;
    m->ordinato_fino = nu;               /* l'arena intera e' ordinata */

    if (bloom_ridimensiona(m, 10 * (vive ? vive : 1024)) != MOTORE_OK)
        return MOTORE_ERR_MEMORIA;
    if (deriva_db(m) != MOTORE_OK)
        return MOTORE_ERR_MEMORIA;
    costruisci_recinto(m);
    return MOTORE_OK;
}

/* ------------------------------------------------------ persistenza */

enum { E_VALIDA = 0, E_SCARTA = 1, E_STOP = -1 };

static int valida_entry(const uint8_t *e, size_t restanti, uint32_t adesso,
                        size_t *dim)
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
        return E_STOP;                    /* da qui non ci si fida [I5] */

    *dim = DIM_HEADER + (size_t)kl + vl;
    uint32_t ttl  = leggi_u32(e + 12);
    uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
    if (entry_morta(e))
        return E_SCARTA;
    if (ttl != 0 && (uint32_t)(adesso - nato) >= ttl)
        return E_SCARTA;                  /* gia' scaduta: sepolta qui */
    return E_VALIDA;
}

int motore_salva(motore_t *m, const char *nome)
{
    if (m->spazzatura > 0 || m->n_tomb > 0)
        if (motore_compatta(m) != MOTORE_OK)
            return MOTORE_ERR_MEMORIA;

    FILE *f = fopen(nome, "wb");
    if (!f)
        return MOTORE_ERR_IO;

    uint8_t header[12];
    memcpy(header, MAGIA, 4);
    scrivi_u32(header + 4, FORMATO_VERSIONE);
    scrivi_u32(header + 8, (uint32_t)m->n_entry);

    int r = MOTORE_OK;
    if (fwrite(header, 1, sizeof header, f) != sizeof header ||
        fwrite(m->data, 1, m->usato, f) != m->usato)
        r = MOTORE_ERR_IO;
    if (fclose(f) != 0)
        r = MOTORE_ERR_IO;
    return r;
}

/* e' gia' tutto in ordine? (file del decimo: no; del motore: si') */
static int arena_gia_ordinata(const motore_t *m)
{
    const uint8_t *prev = NULL;
    size_t off = 0;
    while (off < m->usato) {
        const uint8_t *e = m->data + off;
        if (prev && confronta_chiave(prev, (const char *)e + DIM_HEADER,
                                     leggi_u32(e + 4)) >= 0)
            return 0;
        prev = e;
        off += entry_dim(e);
    }
    return 1;
}

motore_t *motore_apri_da(const char *nome, uint32_t adesso, size_t *scartate)
{
    crc_init();
    FILE *f = fopen(nome, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
        { fclose(f); return NULL; }
    long dim_file = ftell(f);
    if (dim_file < (long)(sizeof MAGIA + 8))
        { fclose(f); return NULL; }
    rewind(f);

    uint8_t *raw = malloc((size_t)dim_file);
    if (!raw)
        { fclose(f); return NULL; }
    if (fread(raw, 1, (size_t)dim_file, f) != (size_t)dim_file)
        { free(raw); fclose(f); return NULL; }
    fclose(f);

    if (memcmp(raw, MAGIA, 4) != 0 || leggi_u32(raw + 4) != FORMATO_VERSIONE)
        { free(raw); return NULL; }
    uint32_t dichiara = leggi_u32(raw + 8);

    const uint8_t *dati = raw + 12;
    size_t size = (size_t)dim_file - 12;

    /* PASSO 1: valida e conta */
    size_t off = 0, vive = 0, scartate_n = 0, fermati_a = 0, validi = 0;
    int fermati = 0;
    while (off < size) {
        size_t dim;
        int esito = valida_entry(dati + off, size - off, adesso, &dim);
        if (esito == E_STOP) {
            fermati = 1;
            fermati_a = off;
            break;
        }
        if (esito == E_VALIDA) {
            vive++;
            validi += dim;                    /* l'arena va DOTATA        */
        } else {
            scartate_n++;
        }
        off += dim;
    }
    if (fermati)
        printf("[motore] validazione ferma a offset %zu (di %zu): "
               "troncata o CRC, ignoro %zu byte di coda [I5]\n",
               fermati_a, size, size - fermati_a);
    if (dichiara != vive)
        printf("[motore] header dichiarava %u, validate %zu: "
               "mi fido della validazione\n", dichiara, vive);

    /* PASSO 2: copia e deriva */
    motore_t *m = alloca_motore(vive ? vive : VOCI_INIZIALI);
    if (!m)
        { free(raw); return NULL; }
    m->adesso = adesso;

    /* il conteggio del passo 1 dota l'arena PRIMA del memcpy: senza
     * questo il buffer da 64 KB riceverebbe megabyte (il bug del
     * warning 'prossima_potenza mai usata') */
    if (assicura_spazio(m, validi ? validi : CAPACITA_MIN) != MOTORE_OK)
        { free(raw); motore_chiudi(m); return NULL; }

    if (bloom_ridimensiona(m, 10 * (vive ? vive : 1024)) != MOTORE_OK)
        { free(raw); motore_chiudi(m); return NULL; }

    size_t nu = 0;
    off = 0;
    while (off < size) {
        size_t dim;
        int esito = valida_entry(dati + off, size - off, adesso, &dim);
        if (esito == E_STOP)
            break;
        if (esito == E_VALIDA) {
            memcpy(m->data + nu, dati + off, dim);
            /* derivo fallita = ENOMEM a meta' ricostruzione: meglio
             * meno dati che un motore incoerente */
            if (derivo(m, nu, 0) != MOTORE_OK)
                { free(raw); motore_chiudi(m); return NULL; }
            nu += dim;
        }
        off += dim;
    }

    free(raw);
    m->usato = nu;
    m->n_tomb = 0;

    /* il file di un motore e' ordinato (per costruzione): il recinto
     * risorge gratis. Un file del decimo file non lo e': la passeggiata
     * restera' di bytes, onesta */
    if (nu > 0 && arena_gia_ordinata(m)) {
        m->ordinato_fino = nu;
        costruisci_recinto(m);
    }

    if (scartate)
        *scartate = scartate_n;
    return m;
}

/* ------------------------------------------------------- chiedi (LSH) */

size_t motore_chiedi(motore_t *m, const char *query, size_t qlen,
                     size_t k, motore_risposta_t out[], size_t *n_candidati)
{
    if (k > TOPLIM)
        k = TOPLIM;
    if (k == 0)
        return 0;
    if (n_candidati)
        *n_candidati = 0;

    int trovata;
    size_t idx = trova_slot(m, calcolo_hash(query, qlen), query, qlen, &trovata);
    if (trovata) {
        size_t id = (size_t)m->slot[idx].id1 - 1;
        const uint8_t *e = m->data + m->voci[id].off1 - 1;
        uint32_t ttl  = leggi_u32(e + 12);
        uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
        if (ttl != 0 && (uint32_t)(m->adesso - nato) >= ttl) {
            muore(m, id);                 /* l'esatto non mente [I6]   */
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
    uint32_t ng = costruisci_grammi(grammi, query, qlen);
    if (ng == 0)
        return 0;

    uint32_t firma[N_BANKS];
    firma_di_grammi(firma, grammi, ng);

    m->marca++;
    static uint32_t cand[MAX_CANDIDATI];
    uint32_t n_cand = 0;
    for (uint32_t b = 0; b < N_BANKS; b++) {
        size_t bh = firma[b] & (((size_t)1 << N_BUCKET_BN) - 1);
        for (uint32_t e = m->testa[b][bh]; e != 0;
             e = m->prossimo[b][e - 1]) {
            uint32_t c = e - 1;
            if (m->visto[c] == m->marca)
                continue;
            m->visto[c] = m->marca;
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
        if (m->voci[id].off1 == VOCE_MORTA)
            continue;                    /* un morto tace [I6]         */
        const uint8_t *e = m->data + m->voci[id].off1 - 1;
        uint32_t ttl  = leggi_u32(e + 12);
        uint32_t nato = leggi_u32(e + 16) & ~MORTA_BIT;
        if (ttl != 0 && (uint32_t)(m->adesso - nato) >= ttl) {
            muore(m, id);                 /* scade al ranking [I3][T8] */
            continue;
        }
        double j = jaccard(grammi, ng,
                           m->gram_arena + m->voci[id].gram_off,
                           m->voci[id].gram_cnt);
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

/* ---------------------------------------------------- intervallo [I7] */

size_t motore_intervallo(motore_t *m, const char *da, size_t da_len,
                         const char *a, size_t a_len, size_t k_max,
                         motore_risposta_t out[], size_t *totali,
                         uint32_t *passi_seek)
{
    size_t tot = 0, riempite = 0, passi = 0;

    /* range rovesciato: le passeggiate lo scoprirebbero da sole,
     * ma dirlo subito e' gratis */
    if (memcmp(da, a, da_len < a_len ? da_len : a_len) > 0)
        return 0;

    /* PARTE ORDINATA: il recinto porta a inizio span [I7] */
    size_t off = m->n_recinto > 0 ? cerca_recinto(m, da, da_len) : 0;
    while (off < m->ordinato_fino) {
        const uint8_t *e = m->data + off;
        if (confronta_chiave(e, a, a_len) > 0)
            break;                       /* ordine: stop garantito     */
        if (!entry_morta(e) &&
            confronta_chiave(e, da, da_len) >= 0) {
            tot++;
            if (riempite < k_max) {
                out[riempite].chiave     = e + DIM_HEADER;
                out[riempite].chiave_len = leggi_u32(e + 4);
                out[riempite].valore     = e + DIM_HEADER + out[riempite].chiave_len;
                out[riempite].valore_len = leggi_u32(e + 8);
                out[riempite].confidenza = 1.0;
                riempite++;
            }
        }
        off += entry_dim(e);
        passi++;
    }

    /* LA CODA (i giovani: nati dopo l'ultimo merge) */
    off = m->ordinato_fino;
    while (off < m->usato) {
        const uint8_t *e = m->data + off;
        if (!entry_morta(e) &&
            confronta_chiave(e, a, a_len) <= 0 &&
            confronta_chiave(e, da, da_len) >= 0) {
            tot++;
            if (riempite < k_max) {
                out[riempite].chiave     = e + DIM_HEADER;
                out[riempite].chiave_len = leggi_u32(e + 4);
                out[riempite].valore     = e + DIM_HEADER + out[riempite].chiave_len;
                out[riempite].valore_len = leggi_u32(e + 8);
                out[riempite].confidenza = 1.0;
                riempite++;
            }
        }
        off += entry_dim(e);
        passi++;
    }

    if (passi_seek)
        *passi_seek = (uint32_t)passi;
    if (totali)
        *totali = tot;
    return riempite;
}

/* ------------------------------------------------ l'auto-audit di [I7] */

/*
 * Il motore sa verificarsi da solo: conta le entry vive camminando la
 * verita' (l'arena) e le confronta col contatore che ogni morte ha
 * aggiornato. Se questi due numeri divergono, o una morte non ha
 * lasciato il segno (rompe [I7]) o un contatore mente (rompe [I3]).
 */
size_t motore_audit(const motore_t *m, size_t *dichiarate)
{
    size_t vive = 0, off = 0;
    while (off < m->usato) {
        const uint8_t *e = m->data + off;
        if (!entry_morta(e))
            vive++;
        off += entry_dim(e);
    }
    if (dichiarate)
        *dichiarate = m->n_entry;
    return vive;
}

/* ---------------------------------------------------- statistiche */

void motore_statistiche(const motore_t *m, const char *etichetta)
{
    double mn = m->n_voci ? (double)m->bloom_m / (double)m->n_voci : 0.0;
    printf("%-14s %8zu byte, %6zu spazzatura (%4.1f%%), %5zu vive, %5zu voci, "
           "%6zu slot (LF %2.0f%%), ordine %s (%zu/%zu, recinto %zu), "
           "bloom m/n=%.1f, scadute batch %llu, sonde risparmiate %llu\n",
           etichetta, m->usato, m->spazzatura,
           m->usato ? 100.0 * (double)m->spazzatura / (double)m->usato : 0.0,
           m->n_entry, m->n_voci, m->n_slot,
           100.0 * (double)(m->n_entry + m->n_tomb) / (double)m->n_slot,
           m->ordinato_fino ? "si'" : "no",
           m->ordinato_fino, m->usato, m->n_recinto,
           mn,
           (unsigned long long)m->scadute_batch,
           (unsigned long long)m->sonde_risparmiate);
}
