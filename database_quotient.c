/*
 * database_quotient.c — nona iterazione: appartenenza a bit.
 *
 * =====================================================================
 * I DUE FRATELLI DI QUESTO FILE
 * =====================================================================
 * Filtro di BLOOM (1970): un array di m bit, k hash per chiave;
 * inserire = accendere k bit; chiedere = guardarli. Se anche UNO
 * spento -> "sicuramente non c'e'"; se tutti accesi -> "probabilmente
 * c'e'". Zero falsi negativi, falsi positivi a pagamento:
 *
 *       FP = (1 - e^(-k n / m))^k       k ottimale = (m/n) ln 2
 *
 * A 1% di falsi positivi costa ~9.6 bit per chiave: la biblioteca dei
 * bit. MA: non si cancella niente mai (un bit acceso puo' servire ad
 * altre chiavi) e non si possono elencare le chiavi.
 *
 * QUOTIENT FILTER (Bender et al. 2012): il fingerprint a 64 bit si
 * spezza in QUOZIENTE (bit alti = la "casa" dove dovrebbe stare) e
 * RESTO (chi, dei fratelli della casa, e' lui). Le chiavi della stessa
 * casa stanno vicine in RUN ORDINATE per casa, gli spostamenti sono
 * corti (il filtro resta cache-friendly), e in cambio di ~9 volte i
 * bit del Bloom compra: CANCELLAZIONE, ENUMERAZIONE, MERGE.
 *
 * =====================================================================
 * IL POST MORTEM DELL'IMPLEMENTAZIONE (la lezione piu' densa del file)
 * =====================================================================
 * La prima versione usava il decoding canonico a meta-bit
 * (occupied/continuation/shifted, con un EXPL aggiunto): cinque difetti
 * sbucati uno dopo l'altro, tutti della stessa famiglia — due
 * interpretazioni della stessa memoria che divergono:
 *   - il bit della casa portato in giro con l'elemento durante i
 *     traslochi (le case cambiavano quartiere);
 *   - il CONT letto come "c'e' altro dopo" invece che "continuo la run
 *     precedente" (le run piu' lunghe di uno non si leggevano);
 *   - un enumeratore col suo decodificatore parallelo (dicendo una
 *     storia diversa dalla ricerca);
 *   - la camminata a sinistra fermata da elementi a casa loro in mezzo
 *     ai blocchi (conteggio run sbagliato);
 *   - i confini del cluster calcolati dal primo slot non-elemento
 *     (ricostruzione che azzerava tutto).
 *
 * Verdetto d'ingegneria: quel decoding ESISTE per comprimere i bit per
 * slot, ma qui lo slot era gia' un uint64_t: comprimeva niente e
 * pagava tutto. Quindi la versione finale archivia il fingerprint
 * INTERO nel resto: LA CASA VIAGGIA CON L'ELEMENTO, e le operazioni
 * leggono case esplicite. I meta-bit restano (EXPL, CONT, SHFT
 * ricalcolato a ogni scrittura, OCC come cache della casa) e OGNI
 * scrittura dei meta riparte dal contenuto: impossibile che meta e
 * contenuto raccontino storie diverse. Ridondanza: qb bit per
 * elemento. E' il prezzo, dichiarato, della leggibilita' — e in
 * QUESTA implementazione non costa nemmeno memoria: lo slot era
 * comunque un uint64_t.
 * =====================================================================
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wpedantic"
# include <windows.h>
# pragma GCC diagnostic pop
#endif

#define M_OCC  0x01   /* la casa (lo slot) ha almeno un elemento        */
#define M_CONT 0x02   /* proseguo la run dello slot precedente          */
#define M_SHFT 0x04   /* elemento fuori casa                            */
#define M_EXPL 0x08   /* questo slot tiene davvero un elemento          */

enum { OK = 0, ERR_MEM = -1 };

typedef struct {
    uint8_t  *bit;
    size_t    m;
    uint32_t  k;
    size_t    n_inseriti;
} bloom_t;

typedef struct {
    uint64_t *rem;      /* fingerprint INTERO: casa nei bit alti       */
    uint8_t  *meta;
    size_t    n;
    uint32_t  qb;
    size_t    n_elem;
    uint64_t  spostamenti;
} qf_t;

/* ------------------------------------------------------------- base */

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

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* L'unica verita' su cosa e' una chiave: il suo fingerprint a 64 bit */
static uint64_t fp64(const char *s, size_t len)
{
    uint32_t a = 2166136261u, b = 0x9e3779b9u;
    for (size_t i = 0; i < len; i++) {
        a ^= (uint32_t)(uint8_t)s[i];
        a *= 16777619u;
        b = mix32(b + (uint32_t)(uint8_t)s[i] * (uint32_t)(i + 1));
    }
    uint64_t h1 = mix32(a);
    uint64_t h2 = mix32(b ^ a) | 1u;   /* dispari: percorre tutto l'anello */
    return (h1 << 32) ^ h2 ^ mix32((uint32_t)(h1 * h2));
}

/* ---------------------------------------------------------- BLOOM */

static int bloom_init(bloom_t *bl, size_t m_bit, uint32_t k)
{
    bl->m = m_bit;
    bl->k = k;
    bl->n_inseriti = 0;
    bl->bit = calloc((m_bit + 7) / 8, 1);
    return bl->bit ? OK : ERR_MEM;
}

static void bloom_libera(bloom_t *bl) { free(bl->bit); bl->bit = NULL; }

/*
 * DOUBLE HASHING (k hash da 2): h_i = (h1 + i*h2) % m.
 * Il primo giro di debug usava una derivazione con i bit bassi di base
 * condivisi fra tutti gli hash: le sette porte cadevano nel solito
 * quartiere e i falsi positivi erano 14.7% contro 0.8% teorici. Due
 * hash indipendenti, il resto e' aritmetica.
 */
static void bloom_inserisci(bloom_t *bl, const char *s, size_t len)
{
    uint64_t fp = fp64(s, len);
    uint64_t h1 = mix32((uint32_t)(fp >> 32)) << 16 ^ mix32((uint32_t)fp);
    uint64_t h2 = mix32((uint32_t)(fp ^ (fp >> 32))) | 1u;
    uint64_t nn = (uint64_t)bl->m;
    for (uint32_t i = 0; i < bl->k; i++) {
        uint64_t h = (h1 + (uint64_t)i * h2) % nn;
        bl->bit[(size_t)(h >> 3)] |= (uint8_t)(1u << (h & 7));
    }
    bl->n_inseriti++;
}

static int bloom_contiene(const bloom_t *bl, const char *s, size_t len)
{
    uint64_t fp = fp64(s, len);
    uint64_t h1 = mix32((uint32_t)(fp >> 32)) << 16 ^ mix32((uint32_t)fp);
    uint64_t h2 = mix32((uint32_t)(fp ^ (fp >> 32))) | 1u;
    uint64_t nn = (uint64_t)bl->m;
    for (uint32_t i = 0; i < bl->k; i++) {
        uint64_t h = (h1 + (uint64_t)i * h2) % nn;
        if (!(bl->bit[(size_t)(h >> 3)] & (1u << (h & 7))))
            return 0;
    }
    return 1;
}

/* -------------------------------------------------- QUOTIENT FILTER */

static int qf_init(qf_t *qf, uint32_t qb)
{
    qf->qb = qb;
    qf->n = (size_t)1 << qb;
    qf->rem = calloc(qf->n, sizeof *qf->rem);
    qf->meta = calloc(qf->n, sizeof *qf->meta);
    qf->n_elem = 0;
    qf->spostamenti = 0;
    return (qf->rem && qf->meta) ? OK : ERR_MEM;
}

static void qf_libera(qf_t *qf)
{
    free(qf->rem);
    free(qf->meta);
    qf->rem = NULL;
    qf->meta = NULL;
}

/* la casa di un elemento archiviato: i bit alti del suo fingerprint */
static size_t casa_elem(const qf_t *qf, size_t slot)
{
    return (size_t)(qf->rem[slot] >> (64 - qf->qb));
}

/*
 * fine_run: il primo slot DOPO la run che contiene `da`. La run
 * finisce dove il PROSSIMO slot non e' una prosecuzione (M_CONT).
 * (La semantica giusta del CONT, imparata alla dura: il bit sta sul
 * proseguimento, non sul capofila.)
 */
static size_t fine_run(const qf_t *qf, size_t da)
{
    size_t p = da;
    for (;;) {
        size_t succ = (p + 1) & (qf->n - 1);
        if ((qf->meta[succ] & M_EXPL) && (qf->meta[succ] & M_CONT))
            p = succ;
        else
            return succ;
    }
}

/*
 * caccia: trova il capofila della run della casa q (o il punto di
 * nascita). Mai a sinistra della casa: la delete canonica (swappa in
 * coda alla run, il buco cammina solo su elementi gia' dislocati)
 * non sposta MAI niente a sinistra della propria casa, quindi la
 * casa svuota-solo-flag non esiste piu' come problema: se lo slot
 * e' libero ma la bandiera M_OCC e' accesa, la run esiste ed e' alla
 * sua destra: si inizia la caccia da q+1.
 */
static size_t caccia_run(const qf_t *qf, size_t q, int *trovata)
{
    size_t maschera_n = qf->n - 1;
    size_t p = q;
    *trovata = 0;
    if (!(qf->meta[p] & M_EXPL) && (qf->meta[p] & M_OCC))
        p = (p + 1) & maschera_n;           /* la run esiste: sta a destra */

    for (;;) {
        if (!(qf->meta[p] & M_EXPL))
            return p;                       /* qui nascera' */
        if (qf->meta[p] & M_CONT) {
            if (casa_elem(qf, p) == q) {
                while (qf->meta[p] & M_CONT)
                    p = (p + qf->n - 1) & maschera_n;
                *trovata = 1;               /* capofila della run di q */
                return p;
            }
            p = fine_run(qf, p);            /* run di un'altra casa */
            continue;
        }
        size_t h = casa_elem(qf, p);
        if (h == q) {
            *trovata = 1;
            return p;
        }
        if (h > q)
            return p;                       /* la nostra va prima di qui */
        p = fine_run(qf, p);                /* h < q: salta la run */
    }
}

/* casa di un fingerprint NON ancora archiviato */
static size_t casa_fp(const qf_t *qf, uint64_t fp)
{
    return (size_t)(fp >> (64 - qf->qb));
}

/*
 * qf_insert: trova il punto (fine della run di q se esiste, altrimenti
 * il suo posto), poi TRASLOCO A CATENA verso destra fino al primo slot
 * libero. Con case esplicite ogni elemento porta la sua casa: i bit
 * M_SHFT si ricalcolano da soli, M_CONT del portato si conserva.
 */
static int qf_insert(qf_t *qf, uint64_t fp)
{
    if (qf->n_elem * 10 >= qf->n * 9)
        return ERR_MEM;                      /* tetto di sicurezza al 90% */

    size_t q = casa_fp(qf, fp);
    size_t maschera_n = qf->n - 1;

    int esiste;
    size_t punto = caccia_run(qf, q, &esiste);
    int prosecuzione = esiste;               /* in coda alla sua run */
    if (esiste) {
        /* BUG FIX (il falso negativo di mem00057): caccia_run restituisce
         * l'INIZIO della run; l'appending va alla FINE. Senza questo
         * salto il nuovo elemento calzava addosso al capofila con un
         * M_CONT falso ("continuo la run prima di me") e la ricerca
         * d'estate smetteva di riconoscerla. */
        punto = fine_run(qf, punto);
    }

    qf->meta[q] |= M_OCC;                    /* da ora q ha elementi */

    uint64_t in_corsa = fp;
    while (qf->meta[punto] & M_EXPL) {
        qf->spostamenti++;
        uint64_t salvato_rem = qf->rem[punto];
        int      salvato_pros = (qf->meta[punto] & M_CONT) ? 1 : 0;

        uint8_t m = (uint8_t)(M_EXPL | (prosecuzione ? M_CONT : 0));
        if (casa_fp(qf, in_corsa) != punto)
            m |= M_SHFT;
        qf->rem[punto] = in_corsa;
        qf->meta[punto] = (uint8_t)((qf->meta[punto] & M_OCC) | m);

        in_corsa = salvato_rem;
        prosecuzione = salvato_pros;
        punto = (punto + 1) & maschera_n;
    }
    uint8_t m = (uint8_t)(M_EXPL | (prosecuzione ? M_CONT : 0));
    if (casa_fp(qf, in_corsa) != punto)
        m |= M_SHFT;
    qf->rem[punto] = in_corsa;
    qf->meta[punto] = (uint8_t)((qf->meta[punto] & M_OCC) | m);
    qf->n_elem++;
    return OK;
}

static int qf_contains(const qf_t *qf, uint64_t fp)
{
    size_t q = casa_fp(qf, fp);
    if (!(qf->meta[q] & M_OCC))
        return 0;                            /* la casa non ha mai avuto */
    int esiste;
    size_t p = caccia_run(qf, q, &esiste);
    if (!esiste)
        return 0;
    for (;;) {
        if (qf->rem[p] == fp)
            return 1;
        size_t succ = (p + 1) & (qf->n - 1);
        if (!(qf->meta[succ] & M_CONT))
            return 0;                        /* fine della run */
        p = succ;
    }
}

/*
 * qf_delete - LA FORMA CANONICA, dopo la lezione piu' lunga del file.
 * Prima versione: compattava tutto il blocco verso sinistra: elementi
 * finiti a SINISTRA della propria casa, la caccia che non li trovava
 * piu', tampone su tampone. La forma canonica (Bender) invece:
 *   1. la vittima scambia il suo resto con l'ULTIMO della sua run:
 *      il buco nasce a fine run, mai in mezzo (una run non si spezza)
 *   2. il buco cammina a destra SOLO fintantoche' il vicino e' un
 *      elemento gia' DISLOCATO (M_SHFT): spostarlo indietro lo
 *      avvicina a casa sua; quando il vicino e' a casa sua il buco si
 *      ferma: da quel punto in poi e' un confine legale fra cluster.
 * Nessun elemento passa mai a sinistra della propria casa: la caccia
 * non ha bisogno di guardare a sinistra. I meta si ricalcolano dal
 * contenuto ad ogni mossa.
 */
static int qf_delete(qf_t *qf, uint64_t fp)
{
    size_t q = casa_fp(qf, fp);
    size_t maschera_n = qf->n - 1;
    if (!(qf->meta[q] & M_OCC))
        return 0;
    int esiste;
    size_t p = caccia_run(qf, q, &esiste);
    if (!esiste)
        return 0;

    /* vittima dentro la run */
    size_t v = p;
    for (;;) {
        if (qf->rem[v] == fp)
            break;
        size_t succ = (v + 1) & maschera_n;
        if (!(qf->meta[succ] & M_CONT))
            return 0;                       /* la run finisce: non c'e' */
        v = succ;
    }

    /* 1. il buco va in coda alla run: scambio col resto dell'ultimo */
    size_t coda = fine_run(qf, p);          /* primo slot dopo la run */
    coda = (coda + qf->n - 1) & maschera_n; /* ultimo slot della run */
    if (coda != v)
        qf->rem[v] = qf->rem[coda];         /* il resto della coda prende
                                              il posto della vittima */
    v = coda;
    qf->rem[v] = 0;
    qf->meta[v] = (uint8_t)(qf->meta[v] & M_OCC);

    /* 2. il buco cammina a destra dentro i soli elementi dislocati */
    for (;;) {
        size_t j = (v + 1) & maschera_n;
        if (!(qf->meta[j] & M_EXPL))
            break;                          /* fine cluster: buco legale */
        if (!(qf->meta[j] & M_SHFT))
            break;                          /* a casa sua: non si tocca */
        uint64_t rem_moved = qf->rem[j];
        size_t casa_moved = (size_t)(rem_moved >> (64 - qf->qb));
        uint8_t m = (uint8_t)M_EXPL;
        if (v != casa_moved)
            m |= M_SHFT;
        size_t prev = (v + qf->n - 1) & maschera_n;
        if ((qf->meta[prev] & M_EXPL) && casa_elem(qf, prev) == casa_moved)
            m |= M_CONT;
        qf->rem[v] = rem_moved;
        qf->meta[v] = (uint8_t)((qf->meta[v] & M_OCC) | m);
        qf->rem[j] = 0;
        qf->meta[j] = (uint8_t)(qf->meta[j] & M_OCC);
        qf->spostamenti++;
        v = j;
    }

    /* la casa q ha ancora elementi? conta dentro la ex-run */
    {
        size_t rimasti = 0;
        size_t i = p;
        for (;;) {
            if ((qf->meta[i] & M_EXPL) && casa_elem(qf, i) == q)
                rimasti++;
            if (!(qf->meta[(i + 1) & maschera_n] & M_CONT))
                break;
            i = (i + 1) & maschera_n;
        }
        if (rimasti == 0)
            qf->meta[q] &= (uint8_t)~M_OCC;
    }
    qf->n_elem--;
    return 1;
}

/* enumerazione: ogni elemento porta la sua casa: si elencano i fp */
static size_t qf_enumera(const qf_t *qf, uint64_t *out, size_t cap)
{
    size_t scritti = 0;
    for (size_t i = 0; i < qf->n; i++) {
        if (!(qf->meta[i] & M_EXPL))
            continue;
        if (scritti < cap)
            out[scritti] = qf->rem[i];
        scritti++;
    }
    return scritti;
}

/* ---------------------------------------------------------------- test */

static int autotest(void)
{
    enum { N = 700 };
    qf_t qf, gemello;
    if (qf_init(&qf, 10) != OK || qf_init(&gemello, 10) != OK)
        return 1;
    int errori = 0;
    char kbuf[32];
    static uint64_t viste[2048], attese[2048];

    for (uint32_t i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%05u", i);
        qf_insert(&qf, fp64(kbuf, (size_t)kl));
        qf_insert(&gemello, fp64(kbuf, (size_t)kl));   /* determinismo */
    }
    if (memcmp(qf.meta, gemello.meta, qf.n) != 0 ||
        memcmp(qf.rem, gemello.rem, qf.n * sizeof(uint64_t)) != 0) {
        printf("autotest FALLITO: due filtri identici, stati diversi\n");
        errori++;
    }
    for (uint32_t i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%05u", i);
        uint64_t fpj = fp64(kbuf, (size_t)kl);
        if (!qf_contains(&qf, fpj)) {
            printf("autotest FALLITO: falso negativo su %s (q=%zu "
                   "resto=%08llx)\n", kbuf,
                   (size_t)(fpj >> (64 - qf.qb)),
                   (unsigned long long)fpj);
            errori++;
            break;
        }
        attese[i] = fpj;
    }
    size_t n_viste = qf_enumera(&qf, viste, 2048);
    if (n_viste != N) {
        printf("autotest FALLITO: enumerate conta %zu su %d\n", n_viste, N);
        errori++;
    } else {
        qsort(viste, N, sizeof viste[0], cmp_u64);
        qsort(attese, N, sizeof attese[0], cmp_u64);
        if (memcmp(viste, attese, N * sizeof *viste) != 0) {
            printf("autotest FALLITO: enumerate ricompone fingerprint "
                   "diversi\n");
            errori++;
        }
    }
    /* delete della meta' e verifica dei superstiti */
    for (uint32_t i = 0; i < N && errori == 0; i += 2) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%05u", i);
        if (qf_delete(&qf, fp64(kbuf, (size_t)kl)) != 1) {
            printf("autotest FALLITO: delete di %s non riuscita\n", kbuf);
            errori++;
            break;
        }
    }
    for (uint32_t i = 0; i < N && errori == 0; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%05u", i);
        int atteso = (i % 2) == 1;
        if (qf_contains(&qf, fp64(kbuf, (size_t)kl)) != atteso) {
            printf("autotest FALLITO: %s %s dopo le delete\n", kbuf,
                   atteso ? "persa" : "risorta");
            errori++;
        }
    }
    n_viste = qf_enumera(&qf, viste, 2048);
    if (n_viste != N / 2 && errori == 0) {
        printf("autotest FALLITO: enumerate post-delete: %zu su %d\n",
               n_viste, N / 2);
        errori++;
    }
    /* reinserisci una cancellata */
    {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%05u", (uint32_t)0);
        qf_insert(&qf, fp64(kbuf, (size_t)kl));
        if (!qf_contains(&qf, fp64(kbuf, (size_t)kl))) {
            printf("autotest FALLITO: reinserimento di mem00000 perso\n");
            errori++;
        }
    }
    /* bloom: mai falsi negativi */
    bloom_t bl;
    if (bloom_init(&bl, 8192, 4) != OK)
        return 1;
    for (uint32_t i = 0; i < 1000; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%05u", i);
        bloom_inserisci(&bl, kbuf, (size_t)kl);
    }
    for (uint32_t i = 0; i < 1000; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%05u", i);
        if (!bloom_contiene(&bl, kbuf, (size_t)kl)) {
            printf("autotest FALLITO: bloom con falso negativo\n");
            errori++;
            break;
        }
    }
    if (errori == 0)
        printf("autotest: QF determinismo, zero falsi negativi, enumerate "
               "esatto, delete di meta' e verifiche, reinserimento; bloom "
               "immacolato: OK (spostamenti %llu)\n",
               (unsigned long long)qf.spostamenti);
    qf_libera(&qf);
    qf_libera(&gemello);
    bloom_libera(&bl);
    return errori;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    if (getenv("SKIP_MAIN"))
        return autotest() != 0;

    enum { NMEMBRI = 200000 };
    static char kbuf[32];
    static uint64_t viste_main[200001];

    printf("== %d membri veri, %d estranei ==\n\n", NMEMBRI, NMEMBRI);

    /* ------------------------------------------- BLOOM: 10 bit/chiave */
    size_t m_bloom = (size_t)NMEMBRI * 10;
    uint32_t k_bloom = 7;
    bloom_t bl;
    if (bloom_init(&bl, m_bloom, k_bloom) != OK) {
        fprintf(stderr, "memoria\n");
        return 1;
    }
    double t0 = ora_ms();
    for (uint32_t i = 0; i < NMEMBRI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%07u", i);
        bloom_inserisci(&bl, kbuf, (size_t)kl);
    }
    double t_ins_bloom = ora_ms() - t0;

    size_t fp_bloom = 0;
    t0 = ora_ms();
    for (uint32_t i = 0; i < NMEMBRI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "abs%07u", i);
        if (bloom_contiene(&bl, kbuf, (size_t)kl))
            fp_bloom++;
    }
    double t_chiedi_bloom = ora_ms() - t0;
    double teo = pow(1.0 - exp(-(double)k_bloom / 10.0), k_bloom) * 100.0;

    printf("BLOOM (m/n=10 bit/chiave, k=%u, %.0f KB):\n"
           "   200000 inserimenti in %6.1f ms\n"
           "   200000 estranei: falsi positivi %zu (%.3f%%, teoria %.3f%%)\n"
           "   %.3f us/chiamata; MAI cancellabile: i bit accesi non si\n"
           "   spengono perche' non sappiamo chi li condivide\n\n",
           k_bloom, (double)((m_bloom + 7) / 8) / 1024.0,
           t_ins_bloom, fp_bloom,
           100.0 * (double)fp_bloom / NMEMBRI, teo,
           t_chiedi_bloom * 2.0 * 1000.0 / NMEMBRI);

    /* ------------------------------------------- QUOTIENT FILTER 2^18 */
    qf_t qf;
    if (qf_init(&qf, 18) != OK) {
        fprintf(stderr, "memoria\n");
        return 1;
    }
    t0 = ora_ms();
    for (uint32_t i = 0; i < NMEMBRI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%07u", i);
        qf_insert(&qf, fp64(kbuf, (size_t)kl));
    }
    double t_ins_qf = ora_ms() - t0;

    size_t mancati = 0;
    t0 = ora_ms();
    for (uint32_t i = 0; i < NMEMBRI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%07u", i);
        if (!qf_contains(&qf, fp64(kbuf, (size_t)kl)))
            mancati++;
    }
    double t_veri_qf = ora_ms() - t0;

    size_t fp_qf = 0;
    for (uint32_t i = 0; i < NMEMBRI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "abs%07u", i);
        if (qf_contains(&qf, fp64(kbuf, (size_t)kl)))
            fp_qf++;
    }

    printf("QUOTIENT FILTER (2^18 slot, riempimento %.0f%%, %.0f KB):\n"
           "   200000 inserimenti in %6.1f ms (spostamenti %llu)\n"
           "   membri: falsi negativi %zu (zero per costruzione)\n"
           "   estranei: falsi positivi %zu (64 bit interi: e' pioggia)\n"
           "   %.3f us/chiamata sui membri\n\n",
           100.0 * (double)qf.n_elem / (double)qf.n,
           (qf.n * (sizeof(uint64_t) + 1)) / 1024.0,
           t_ins_qf, (unsigned long long)qf.spostamenti, mancati, fp_qf,
           t_veri_qf * 2.0 * 1000.0 / NMEMBRI);

    printf("== contabilita' dei bit per chiave ==\n");
    printf("   BLOOM: 10.0 bit/chiave    -> guarda e non tocca\n");
    printf("   QF:    %.1f bit/chiave  -> guarda, cancella, enumera, "
           "merge\n",
           (double)(qf.n * (sizeof(uint64_t) + 1)) * 8.0
               / (double)qf.n_elem);
    printf("   il QF paga ~9 volte i bit del Bloom e compra le "
           "operazioni che il Bloom non sa fare. Scegliere e' questo.\n\n");

    /* delete di meta' e verifica dei superstiti */
    t0 = ora_ms();
    for (uint32_t i = 0; i < NMEMBRI; i += 2) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%07u", i);
        qf_delete(&qf, fp64(kbuf, (size_t)kl));
    }
    double t_del = ora_ms() - t0;
    size_t errori_delete = 0;
    for (uint32_t i = 0; i < NMEMBRI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "mem%07u", i);
        int atteso = (i % 2) == 1;
        if (qf_contains(&qf, fp64(kbuf, (size_t)kl)) != atteso)
            errori_delete++;
    }
    printf("delete di 100000 elementi: %6.1f ms (%.3f us/delete) — "
           "errori %zu, enumerazione %zu sopravvissuti su 100000 attesi\n\n",
           t_del, t_del / 100.0, errori_delete,
           qf_enumera(&qf, viste_main, 200001));

    printf("\n");
    if (autotest() != 0)
        return 1;

    qf_libera(&qf);
    bloom_libera(&bl);
    return 0;
}
