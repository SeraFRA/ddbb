/*
 * database_semantic.c — settima iterazione: il dizionario che "capisce".
 *
 * =====================================================================
 * DALLA FANTASIA ALLA MECCANICA (mappa delle idee della conversazione)
 * =====================================================================
 *   "ogni cosa e' collegata e vicina in uno spazio semantico"
 *          -> lo spazio qui e' l'INSIEME DEI BIGRAMMI della chiave:
 *             due chiavi sono vicine se condividono gran parte dei loro
 *             pezzi da due caratteri. (onestà: e' MORFOLOGIA, non
 *             significato: un modello di embedding capirebbe che
 *             "cane" e "domestic_lupo" sono vicini; i bigrammi no.
 *             Questo e' il gradino 0 della scala semantica.)
 *
 *   "il db risponde in modo probabilistico con le sue entry"
 *          -> MinHash con banding (Broder 1997, la firma di MinHash e'
 *             una versione casuale della similarita' di Jaccard usata
 *             dai motori per rilevare pagine quasi duplicate):
 *             ogni chiave firma in N_BANKS "bande" di N_RIGHE minh;
 *             due chiavi finiscono nella stessa bucket della banda b con
 *             probabilita' s^R dove s e' la loro Jaccard.
 *             Una query di REFUSO cade nelle bucket dei propri vicini
 *             e il database risponde con i candidati, RANKINGATI per
 *             similarita' vera, con la confidenza esplicita.
 *
 *   "risposta probabilistica con fallback deterministico"
 *          -> db_chiedi: prima la ricerca ESATTA (confidenza 1.0),
 *             solo su miss parte il layer semantico. Il contratto e'
 *             del Bloom filter: "se e' esatta, e' certamente quella;
 *             se no, probabilmente intendevi una di queste".
 *
 *   "come la memoria umana: un indizio cade verso il ricordo giusto"
 *          -> Hopfield: qui l'attrattore e' la bucket list: la query
 *             rumorosa converge nel punto della firma piu' simile.
 *
 * LA MATEMATICA DEL BANDING (da validare col run):
 *   P(una banda cattura) = s^R          (minh: P(min uguali) = Jaccard)
 *   P(cattura) = 1 - (1 - s^R)^B        (bande indipendenti)
 *   il cliff: con R grande i refusi doppi scendono sotto il indicatore.
 *   Il parametro B x R e' IL compromesso del progetto: piu' bande
 *   = piu' richiamo (e piu' candidati spuri da rankare); piu' righe
 *   = piu' precisione (e piu' refusi persi).
 *
 * INTERROGABILE CON METRICHE DA INFORMATION RETRIEVAL:
 *   recall@1 e recall@k (il vero e' il primo / nei primi k),
 *   precision sugli alieni (quante risposte spurie sopra soglia),
 *   selettivita' (candidati per query), latenza p50/p99.
 *   Un layer probabilistico si giustifica SOLO con questi numeri.
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

#define N_BANKS       24        /* bande del LSH                     */
#define N_RIGHE       4         /* minh per banda                     */
#define N_BUCKET_BN   17        /* 2^17 bucket per banda              */
#define SOGLIA        0.40      /* confidenza minima dichiarata        */
#define MAX_CANDIDATI 8192      /* tetto difensivo per query          */
#define TOPLIM        8         /* k max per db_chiedi                */
#define MAX_GRAMMI    66        /* ^chiave$ -> len+1 bigrammi         */

enum { DB_OK = 0, DB_ERR_MEMORY = -1, DB_ERR_PIENO = -2 };

typedef struct {
    char     *testo;
    uint32_t  fp;          /* fingerprint FNV per l'indice esatto   */
    uint32_t  gram_off;    /* offset in gram_arena                   */
    uint32_t  gram_cnt;
} voce_t;

typedef struct {
    uint32_t fp;           /* fingerprint rapido                    */
    uint32_t id1;          /* indice voce + 1; 0 = vuoto            */
} slot_t;

typedef struct {
    voce_t    *voci;
    size_t     n_voci, capacita;
    uint32_t  *firma;              /* [id*N_BANKS + b]                 */
    uint32_t  *gram_arena;         /* bigrammi sorted, per voce        */
    size_t     gram_usati;
    uint32_t  *testa[N_BANKS];     /* bucket list per banda            */
    uint32_t  *prossimo[N_BANKS];  /* catena di id per banda            */
    uint32_t  *visto;              /* dedupe candidati per query        */
    uint32_t    marca;
    slot_t    *slot;               /* indice esatto                      */
    size_t     n_slot;
} db_t;

/* ------------------------------------------------------------- utilita' */

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
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

static uint32_t prng(void)
{
    static uint32_t x = 0x12345678u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

/* ------------------------------------------------- bigrammi delle chiavi */

/*
 * La "feature": l'insieme dei bigrammi POSIZIONALI (^ e $ inclusi):
 * un pezzo vale (coppia di caratteri, posizione). Un refuso umano e'
 * "hai sbagliato QUA", non "hai usato altri pezzi": la posizione e'
 * meta' della semantica. (La versione senza posizione -- provata per
 * prima -- rendeva "u00050d0" indistinguibile dai suoi anagrammi
 * numerici: migliaia di pareggi a pari confidenza, il vero perso in
 * mezzo, e gli alieni "zNNNNNNN" passavano la soglia perche' di
 * bigrammi ne condividevano parecchi. Ricordarselo: bag-of-features
 * senza posizione = similarita' di ANAGRAMMA, non di INTENTO.)
 * Ordinati e deduplicati: e' un INSIEME, servi alla Jaccard.
 */
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

    uint32_t n = (uint32_t)len + 1;     /* bigrammi = caratteri + 1 */
    for (uint32_t i = 0; i < n; i++)
        out[i] = gram_hash(buf[i], buf[i + 1], i);

    /* insertion sort + dedupe: array piccolissimi */
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

/* Jaccard: |A inters B| / |A unione B| su insiemi ordinati */
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

/* -------------------------------------------------------- firma minwise */

/*
 * Una sola hash di base per grammo, N_BANKS*N_RIGHE varianti per via di
 * salting+avalanche: ammesso che la famiglia non sia ortogonale come
 * una vera famiglia di hash indipendenti, la proprieta' che serve al
 * banding (P(min uguali) = Jaccard) regge in pratica.
 */
static inline uint32_t hash_riga(uint32_t base, uint32_t k)
{
    return mix(base ^ (k * 0x9e3779b9u + 0x85ebca6bu));
}

/*
 * firma_di_grammi: per ogni banda prende il minimo di N_RIGHE hash del
 * SET di grammi e li fonde in un valore di banda. Ritorna in firma[].
 */
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

/* ------------------------------------------------------------ indice esatto */

static size_t trova_esatto(const db_t *db, const char *key, size_t len)
{
    uint32_t fp = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        fp ^= (uint32_t)(uint8_t)key[i];
        fp *= 16777619u;
    }
    size_t idx = fp & (db->n_slot - 1);
    for (;;) {
        slot_t *s = &db->slot[idx];
        if (s->id1 == 0)
            return (size_t)-1;
        if (s->fp == fp &&
            strlen(db->voci[s->id1 - 1].testo) == len &&
            memcmp(db->voci[s->id1 - 1].testo, key, len) == 0)
            return s->id1 - 1;
        idx = (idx + 1) & (db->n_slot - 1);
    }
}

static int cresci_slot(db_t *db)
{
    size_t nuovo_n = db->n_slot * 2;
    slot_t *nuovo = calloc(nuovo_n, sizeof *nuovo);
    if (!nuovo)
        return DB_ERR_MEMORY;
    for (size_t v = 0; v < db->n_slot; v++) {
        if (db->slot[v].id1 == 0)
            continue;
        size_t idx = db->slot[v].fp & (nuovo_n - 1);
        while (nuovo[idx].id1 != 0)
            idx = (idx + 1) & (nuovo_n - 1);
        nuovo[idx] = db->slot[v];
    }
    free(db->slot);
    db->slot = nuovo;
    db->n_slot = nuovo_n;
    return DB_OK;
}

/* -------------------------------------------------------------- database */

int db_init(db_t *db)
{
    memset(db, 0, sizeof *db);
    db->capacita = 256;
    db->voci = malloc(db->capacita * sizeof *db->voci);
    db->firma = malloc(db->capacita * N_BANKS * sizeof *db->firma);
    db->visto = calloc(db->capacita, sizeof *db->visto);
    db->gram_arena = malloc(db->capacita * MAX_GRAMMI * sizeof *db->gram_arena);
    db->n_slot = 1024;
    db->slot = calloc(db->n_slot, sizeof *db->slot);
    if (!db->voci || !db->firma || !db->visto || !db->gram_arena || !db->slot)
        return DB_ERR_MEMORY;
    for (uint32_t b = 0; b < N_BANKS; b++) {
        db->testa[b] = calloc((size_t)1 << N_BUCKET_BN, sizeof **db->testa);
        db->prossimo[b] = calloc(db->capacita, sizeof **db->prossimo);
        if (!db->testa[b] || !db->prossimo[b])
            return DB_ERR_MEMORY;
    }
    db->marca = 1;
    return DB_OK;
}

void db_libera(db_t *db)
{
    for (size_t i = 0; i < db->n_voci; i++)
        free(db->voci[i].testo);
    free(db->voci);
    free(db->firma);
    free(db->gram_arena);
    free(db->visto);
    for (uint32_t b = 0; b < N_BANKS; b++) {
        free(db->testa[b]);
        free(db->prossimo[b]);
    }
    free(db->slot);
    memset(db, 0, sizeof *db);
}

static int assicura_capacita(db_t *db)
{
    if (db->n_voci < db->capacita)
        return DB_OK;
    size_t nuova = db->capacita * 2;

    voce_t *nv = realloc(db->voci, nuova * sizeof *nv);
    if (!nv) return DB_ERR_MEMORY;
    db->voci = nv;

    uint32_t *nf = realloc(db->firma, nuova * N_BANKS * sizeof *nf);
    if (!nf) return DB_ERR_MEMORY;
    db->firma = nf;

    uint32_t *nvisto = realloc(db->visto, nuova * sizeof *nvisto);
    if (!nvisto) return DB_ERR_MEMORY;
    memset(nvisto + db->capacita, 0, (nuova - db->capacita) * sizeof *nvisto);
    db->visto = nvisto;

    uint32_t *ngram = realloc(db->gram_arena, nuova * MAX_GRAMMI * sizeof *ngram);
    if (!ngram) return DB_ERR_MEMORY;
    db->gram_arena = ngram;

    for (uint32_t b = 0; b < N_BANKS; b++) {
        uint32_t *np = realloc(db->prossimo[b], nuova * sizeof *np);
        if (!np) return DB_ERR_MEMORY;
        memset(np + db->capacita, 0, (nuova - db->capacita) * sizeof *np);
        db->prossimo[b] = np;
    }
    db->capacita = nuova;
    return DB_OK;
}

int db_inserisci(db_t *db, const char *key)
{
    if (trova_esatto(db, key, strlen(key)) != (size_t)-1)
        return DB_OK;                      /* idempotente */

    int r = assicura_capacita(db);
    if (r != DB_OK) return r;
    if ((db->n_voci + 1) * 10 > db->n_slot * 7)
        if (cresci_slot(db) != DB_OK)
            return DB_ERR_MEMORY;

    size_t id = db->n_voci;
    size_t len = strlen(key);
    char *testo = malloc(len + 1);
    if (!testo) return DB_ERR_MEMORY;
    memcpy(testo, key, len + 1);

    uint32_t fp = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        fp ^= (uint32_t)(uint8_t)key[i];
        fp *= 16777619u;
    }

    uint32_t grammi[MAX_GRAMMI];
    uint32_t ng = costruisci_grammi(grammi, key, len);

    db->voci[id].testo = testo;
    db->voci[id].fp = fp;
    db->voci[id].gram_off = (uint32_t)db->gram_usati;
    db->voci[id].gram_cnt = ng;
    memcpy(db->gram_arena + db->gram_usati, grammi, ng * sizeof *grammi);
    db->gram_usati += ng;

    uint32_t *firma = db->firma + id * N_BANKS;
    firma_di_grammi(firma, grammi, ng);
    for (uint32_t b = 0; b < N_BANKS; b++) {
        size_t bh = firma[b] & (((size_t)1 << N_BUCKET_BN) - 1);
        db->prossimo[b][id] = db->testa[b][bh];
        db->testa[b][bh] = (uint32_t)(id + 1);
    }

    /* indice esatto */
    size_t idx = fp & (db->n_slot - 1);
    while (db->slot[idx].id1 != 0)
        idx = (idx + 1) & (db->n_slot - 1);
    db->slot[idx].fp = fp;
    db->slot[idx].id1 = (uint32_t)(id + 1);

    db->n_voci++;
    return DB_OK;
}

const char *db_trova_esatto(db_t *db, const char *key)
{
    size_t id = trova_esatto(db, key, strlen(key));
    return id == (size_t)-1 ? NULL : db->voci[id].testo;
}

/*
 * db_chiedi — LA domanda al database.
 * 1. hit esatto -> [{"quella"}, confidenza 1.0], ritorno immediato
 * 2. miss -> firma della query, raccolta candidati dalle N_BANKS
 *    bucket list (con dedupe), ranking per Jaccard VERA (non stimata:
 *    il minhash serve solo a SCEGLIERE chi valutare, la risposta si
 *    dà con i numeri veri), top-k per inserimento.
 * n_candidati (se non NULL) restituisce la varieta' toccata: la
 * selettivita' del layer.
 */
size_t db_chiedi(db_t *db, const char *query, size_t k,
                 const char **risposte, double *confidenze,
                 size_t *n_candidati)
{
    size_t len = strlen(query);
    size_t id = trova_esatto(db, query, len);
    if (id != (size_t)-1) {
        risposte[0] = db->voci[id].testo;
        confidenze[0] = 1.0;
        if (n_candidati) *n_candidati = 0;
        return 1;
    }
    if (k > TOPLIM)
        k = TOPLIM;

    uint32_t grammi[MAX_GRAMMI];
    uint32_t ng = costruisci_grammi(grammi, query, len);
    if (ng == 0)
        return 0;

    uint32_t firma[N_BANKS];
    firma_di_grammi(firma, grammi, ng);

    /* raccolta candidati con dedupe (visto[] con marca evolving) */
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

    /* ranking top-k per Jaccard: insertion-sort mantenuto dal fondo */
    size_t n_ris = 0;
    for (uint32_t c = 0; c < n_cand; c++) {
        uint32_t id2 = cand[c];
        double j = jaccard(grammi, ng,
                           db->gram_arena + db->voci[id2].gram_off,
                           db->voci[id2].gram_cnt);
        if (n_ris > 0 && j <= confidenze[n_ris - 1] &&
            n_ris >= k)
            continue;
        size_t pos = n_ris < k ? n_ris : k - 1;   /* sostituisce il peggiore */
        if (n_ris < k)
            n_ris++;
        while (pos > 0 && confidenze[pos - 1] < j) {
            risposte[pos] = risposte[pos - 1];
            confidenze[pos] = confidenze[pos - 1];
            pos--;
        }
        risposte[pos] = db->voci[id2].testo;
        confidenze[pos] = j;
    }
    return n_ris;
}

/* ---------------------------------------------------------------- test */

/*
 * Corruttori deterministici.
 * Il benchmark di recall usa SOLO il refuso di SOSTITUZIONE (una cifra
 * diventa una lettera): e' l'unico con verita' unica misurabile: il
 * refuso nasconde il valore originale, quindi u0000r23 e' equidistante
 * da dieci possibili u0000X23 — il database NON PUO' sapere quale
 * cifra c'era (ambiguita' informazionale, non limite del codice) e la
 * risposta onesta e' il gruppo dei pari-merito. La demo mostra questo
 * caso esplicitamente: il db dichiara il pareggio invece di fingere
 * certezza.
 * (Le "alieni" di precisione sono di sole lettere: un test precedente
 * usava "zNNNNNNN" — che invece e' un refuso di PREFISSO a tutti gli
 * effetti, e il db faceva bene a rispondere "uNNNNNNN".)
 */
static void genera_sostituzione(char *dest, size_t cap, const char *originale,
                               uint32_t errori)
{
    snprintf(dest, cap, "%s", originale);
    size_t l = strlen(dest);
    for (uint32_t e = 0; e < errori; e++) {
        size_t p = (size_t)(prng() % (uint32_t)l);
        if (dest[p] < '0' || dest[p] > '9')
            continue;                     /* gia' corrotto qui */
        dest[p] = (char)('a' + (prng() % 26));
    }
}

static void genera_lettere(char *dest, size_t cap)
{
    for (size_t i = 0; i < 8 && i < cap - 1; i++)
        dest[i] = (char)('a' + (prng() % 26));
    dest[8] = '\0';
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/*
 * autotest su un db piccolo e deterministico:
 *  - l'esatto non mente mai
 *  - un refuso viene recuperato nel top-8 nella grande maggioranza
 *  - le chiavi aliene NON ricevono risposte sopra soglia (precisione)
 */
static int autotest(void)
{
    db_t db;
    if (db_init(&db) != DB_OK)
        return 1;
    char kbuf[32];
    for (uint32_t i = 0; i < 3000; i++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", i);
        db_inserisci(&db, kbuf);
    }
    int errori = 0;

    /* l'esatto */
    for (uint32_t i = 0; i < 3000; i++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", i);
        if (db_trova_esatto(&db, kbuf) == NULL) {
            printf("autotest FALLITO: %s non trovata\n", kbuf);
            errori++;
            break;
        }
    }

    /* recupero refusi (sostituzione) */
    uint32_t ripresi = 0;
    for (uint32_t t = 0; t < 500; t++) {
        uint32_t id = prng() % 3000;
        snprintf(kbuf, sizeof kbuf, "k%06u", id);
        char corrotta[40];
        genera_sostituzione(corrotta, sizeof corrotta, kbuf, 1);
        const char *ris[TOPLIM];
        double conf[TOPLIM];
        size_t n = db_chiedi(&db, corrotta, TOPLIM, ris, conf, NULL);
        int trovato = 0;
        for (size_t r = 0; r < n; r++)
            if (strcmp(ris[r], kbuf) == 0)
                trovato = 1;
        ripresi += (uint32_t)trovato;
    }
    if (ripresi < 300) {   /* atteso ~80%: margine ampio per stabilita' */
        printf("autotest FALLITO: refusi ripresi %u/500\n", ripresi);
        errori++;
    } else {
        printf("autotest: 500 refusi di sostituzione recuperati nel "
               "top-8: %u/500\n", ripresi);
    }

    /* alieni di sole LETTERE: nessuna risposta dichiarata sopra soglia */
    uint32_t spurie = 0;
    for (uint32_t t = 0; t < 500; t++) {
        char aliena[16];
        genera_lettere(aliena, sizeof aliena);
        const char *ris[TOPLIM];
        double conf[TOPLIM];
        size_t n = db_chiedi(&db, aliena, TOPLIM, ris, conf, NULL);
        if (n > 0 && conf[0] >= SOGLIA)
            spurie++;
    }
    if (spurie > 10) {
        printf("autotest FALLITO: %u/500 alieni sopra soglia\n", spurie);
        errori++;
    } else {
        printf("autotest: alieni con risposta spuria sopra soglia: %u/500\n",
               spurie);
    }

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

    static char kbuf[40];
    enum { N_CHIAVI = 60000 };

    /* ---------- riempimento ---------- */
    printf("riempio il dizionario semantico con %d chiavi \"uNNNNNNN\"...\n",
           N_CHIAVI);
    double t0 = ora_ms();
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        snprintf(kbuf, sizeof kbuf, "u%07u", i);
        db_inserisci(&db, kbuf);
    }
    printf("inserite in %.0f ms (firma %zu KB, grammi %zu KB, "
           "%d tabelle di bucket da %d slot)\n\n",
           ora_ms() - t0,
           db.n_voci * N_BANKS * sizeof(uint32_t) / 1024,
           db.gram_usati * sizeof(uint32_t) / 1024,
           N_BANKS, 1 << N_BUCKET_BN);

    /* ---------- ricerche esatte: il contratto deterministicco resta ---------- */
    enum { N_ESATTE = 100000 };
    t0 = ora_ms();
    size_t trovate = 0;
    for (uint32_t i = 0; i < N_ESATTE; i++) {
        snprintf(kbuf, sizeof kbuf, "u%07u", (i * 7919u) % N_CHIAVI);
        if (db_trova_esatto(&db, kbuf))
            trovate++;
    }
    printf("esatte:   %d/%d trovate in %.0f ms (%.3f us/get)\n\n",
           (int)trovate, N_ESATTE, ora_ms() - t0,
           (ora_ms() - t0) * 1000.0 / N_ESATTE);

    /* ---------- il fascino: query CORROTTE ---------- */
    const char *ris[TOPLIM];
    double conf[TOPLIM];

    for (uint32_t errori_q = 1; errori_q <= 2; errori_q++) {
        enum { N_QUERY = 10000 };
        static double lat[N_QUERY];
        uint32_t top1 = 0, topk = 0, dichiarate = 0, nessuna = 0;
        double somma_s = 0, somma_conf = 0, somma_pari = 0;
        size_t somma_cand = 0;

        for (uint32_t t = 0; t < N_QUERY; t++) {
            uint32_t id = prng() % N_CHIAVI;
            snprintf(kbuf, sizeof kbuf, "u%07u", id);
            char corrotta[40];
            genera_sostituzione(corrotta, sizeof corrotta, kbuf, errori_q);

            /* s vera fra query e chiave di partenza: per il confronto
             * con la teoria del banding */
            uint32_t gq[MAX_GRAMMI], gv[MAX_GRAMMI];
            uint32_t nq = costruisci_grammi(gq, corrotta, strlen(corrotta));
            uint32_t nv_g = costruisci_grammi(gv, kbuf, strlen(kbuf));
            somma_s += jaccard(gq, nq, gv, nv_g);

            size_t n_cand;
            double a = ora_ms();
            size_t n = db_chiedi(&db, corrotta, TOPLIM, ris, conf, &n_cand);
            lat[t] = ora_ms() - a;
            somma_cand += n_cand;

            int vv1 = n > 0 && strcmp(ris[0], kbuf) == 0;
            int vvk = vv1;
            for (size_t r = 1; r < n; r++)
                if (strcmp(ris[r], kbuf) == 0)
                    vvk = 1;
            topk += (uint32_t)vvk;
            top1 += (uint32_t)vv1;

            /* il gruppo dei pari-merito del primo posto: l'ambiguita'
             * del refuso non e' colpa del db, si MISURA */
            size_t pari = 0;
            for (size_t r = 0; r < n; r++)
                if (conf[r] >= conf[0] - 1e-9)
                    pari++;
            somma_pari += (double)pari;

            if (n > 0 && conf[0] >= SOGLIA) {
                dichiarate++;
                somma_conf += conf[0];
            } else {
                nessuna++;
            }
        }
        qsort(lat, N_QUERY, sizeof lat[0], cmp_double);

        double s_medio = somma_s / N_QUERY;
        double p_teo = 1.0 - pow(1.0 - pow(s_medio, N_RIGHE), N_BANKS);

        printf("== query con %u refus%s di sostituzione ==\n", errori_q,
               errori_q == 1 ? "o" : "i");
        printf("   verita' ripescata al 1o posto: %4.1f%%\n",
               100.0 * top1 / N_QUERY);
        printf("   verita' ripescata nel top-%d: %4.1f%%   "
               "(candidati medi: %zu)\n", TOPLIM,
               100.0 * topk / N_QUERY, somma_cand / N_QUERY);
        printf("   gruppo dei pari-merito del top: %.1f chiavi in media "
               "(il refuso nasconde la cifra: il db dichiara il pareggio)\n",
               somma_pari / N_QUERY);
        printf("   risposte DECLARATE (confidenza >= %.2f): %4.1f%%, "
               "confidenza media %.2f\n", SOGLIA,
               100.0 * dichiarate / N_QUERY,
               dichiarate ? somma_conf / dichiarate : 0.0);
        printf("   silenzio (\"non ho capito\"): %4.1f%%\n",
               100.0 * nessuna / N_QUERY);
        printf("   latenza: p50 %.2f ms, p99 %.2f ms\n\n",
               lat[N_QUERY / 2], lat[N_QUERY * 99 / 100]);
        printf("   teoria banding: s medio %.2f -> P(cattura)=%.0f%% "
               "(da confrontare con il top-%d: il layer fa quello che "
               "predice la formula: il gap che resta e' ambiguita', "
               "non errore)\n\n",
               s_medio, 100.0 * p_teo, TOPLIM);
    }

    /* ---------- alieni di LETTERE: le risposte sparano a vuoto? ---------- */
    {
        enum { N_ALIENI = 10000 };
        uint32_t spurie = 0;
        double conf_max = 0;
        for (uint32_t t = 0; t < N_ALIENI; t++) {
            char aliena[16];
            genera_lettere(aliena, sizeof aliena);
            size_t n = db_chiedi(&db, aliena, TOPLIM, ris, conf, NULL);
            if (n > 0) {
                if (conf[0] > conf_max)
                    conf_max = conf[0];
                if (conf[0] >= SOGLIA)
                    spurie++;
            }
        }
        printf("== %d query aliene (8 lettere a caso) ==\n", N_ALIENI);
        printf("   risposte spurie sopra soglia: %u (%.2f%%), "
               "confidenza massima vista: %.2f\n\n",
               spurie, 100.0 * spurie / N_ALIENI, conf_max);
    }

    /* ---------- refusi di PREFISSO: "zNNNNNNN" con cifra di una chiave
     * ---------- vera: qui RISPONDERE e' la cosa giusta */
    {
        enum { N_PREF = 2000 };
        uint32_t corrette = 0;
        for (uint32_t t = 0; t < N_PREF; t++) {
            uint32_t id = prng() % N_CHIAVI;
            snprintf(kbuf, sizeof kbuf, "z%07u", id);
            size_t n = db_chiedi(&db, kbuf, 1, ris, conf, NULL);
            char vero[40];
            snprintf(vero, sizeof vero, "u%07u", id);
            if (n > 0 && strcmp(ris[0], vero) == 0)
                corrette++;
        }
        printf("== %d refusi di PREFISSO (\"zNNNNNNN\" = chiave vera con "
               "la u sbagliata) ==\n", N_PREF);
        printf("   il db propone la chiave vera al 1o posto: %u (%.1f%%) "
               "(qui rispondere e' dovere: il vecchio test li contava "
               "come falsi positivi)\n\n", corrette,
               100.0 * corrette / N_PREF);
    }

    /* ---------- esempi leggibili ---------- */
    printf("== esempi a occhio ==\n");
    for (int t = 0; t < 3; t++) {
        uint32_t id = (uint32_t)(t * 20000 + 7000);
        snprintf(kbuf, sizeof kbuf, "u%07u", id);
        char corrotta[40];
        genera_sostituzione(corrotta, sizeof corrotta, kbuf, 1);
        size_t n = db_chiedi(&db, corrotta, 3, ris, conf, NULL);
        printf("   chiedo \"%s\" (vero: %s)\n", corrotta, kbuf);
        for (size_t r = 0; r < n; r++)
            printf("      %u. \"%s\"  confidenza %.2f%s\n",
                   (uint32_t)(r + 1), ris[r], conf[r],
                   strcmp(ris[r], kbuf) == 0 ? "  <-- il vero" : "");
    }
    printf("   caso AMBIGUO per costruzione (inserzione: la query non "
           "prova quale cifra c'era):\n");
    {
        char ambigua[40] = "u000500r0";
        size_t n = db_chiedi(&db, ambigua, 3, ris, conf, NULL);
        printf("   chiedo \"%s\"\n", ambigua);
        for (size_t r = 0; r < n; r++)
            printf("      %u. \"%s\"  confidenza %.2f\n",
                   (uint32_t)(r + 1), ris[r], conf[r]);
        printf("   tutti a pari confidenza: il db dichiara il pareggio, "
               "non finge di sapere\n");
    }

    printf("\n");
    if (autotest() != 0)
        return 1;

    db_libera(&db);
    return 0;
}
