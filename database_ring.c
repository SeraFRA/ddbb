/*
 * database_ring.c — ottava iterazione: la scadenza a geometria di anello.
 *
 * =====================================================================
 * L'IDEA (modella: timing wheel del kernel Linux)
 * =====================================================================
 * Fino a qui la scadenza era un CAMPO da controllare a ogni get
 * ("adesso - nato > ttl?") e la memoria si recuperava alla compattazione:
 * pigra. Funziona, ma le chiavi scadute occupano indice e RAM finche'
 * qualcuno non le legge.
 *
 * Il rovescio della medaglia: far diventare lo scadere un LUOGO, non un
 * calcolo. Un anello di N_TICK celle: la entry con ttl T, inserita al
 * tick corrente, viene appesa alla catena di anello[(ora+T) % N_TICK].
 * Le entrry si ordinano DA SOLE per momento di morte: far scadere non
 * e' cercare, e' AVANZARE UNA TESTINA. db_avanza() attraversa i tick
 * svuotando le catene che incontra: costo O(1) ammortizzato per tick,
 * O(1) per entry scaduta, e le morte escono anche dall'INDICE (con la
 * delete a backshift, qui sotto).
 *
 * IL PROBLEMA CLASSICO: l'anello copre N_TICK di futuro; un ttl piu'
 * lungo non ci starebbe (la stessa cella servira' a scadenze di cicli
 * successivi). Il trucco del kernel e' il RIACCODO pigro: quando la
 * cella viene aperta, chi non e' davvero scaduto (scadenza > tick
 * corrente) resta al suo posto e morira' al giro giusto. Qui il ttl e'
 * limitato a N_TICK: marco indossato.
 *
 * =====================================================================
 * LA DELETE SENZA TOMBSTONE: il BACKSHIFT
 * =====================================================================
 * Tecnica nuova per il progetto: l'indice esatto non usa piu' tombstone
 * (e non paga piu' ricostruzioni per ripulirli) ma lo SPOSTAMENTO
 * ALL'INDIETRO. Quando una chiave libera lo slot i, la catena di probing
 * che dipende da i si spezzerebbe: si guarda la entry j successiva e ci
 * si chiede: la sua casa (slot canonico k) e' raggiungibile PRIMA
 * attraversando i che attraversando j? Se la catena di j parte da k,
 * passa per i e arriva a j, allora j PUO' traslocare nel buco: il guado
 * segue j e si ripete. La condizione e' tutta in un confronto di distanze
 * cicliche:
 *
 *     ((i - k) & mask) < ((j - k) & mask)      ==> trasloca
 *
 * Senza tombstone: catene sempre corte e RIPULITE, mai ricostruzioni.
 * Il costo: ogni delete e' O(lunghezza della parte di catena riparata).
 * La delete dell'anello usa lo stesso codice della delete manuale.
 *
 * =====================================================================
 * L'OROLOGIO E' INIETTATO
 * =====================================================================
 * Il tempo reale rende i test non riproducibili: qui il "tempo" e' un
 * intero che il chiamante avanza. In produzione l'avanzamento sarebbe
 * un thread periodico o il tick del loop di rete: il meccanismo non
 * cambia. (Cintura e bretelle: anche il get butta giu' chi e' scaduto
 * se l'orologio non e' mai stato avanzato.)
 * =====================================================================
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wpedantic"
# include <windows.h>
# pragma GCC diagnostic pop
#endif

#define N_TICK          4096
#define SLOT_INIZIALI   ((size_t)1 << 10)
#define LOAD_FACTOR_10  7
#define MAX_TTL         N_TICK

enum {
    DB_OK           =  0,
    DB_ERR_MEMORY   = -1,
    DB_ERR_TTL      = -2,   /* ttl oltre la finestra dell'anello */
    DB_ERR_MANCA    = -3
};

typedef struct voce {
    char     *chiave;
    char     *valore;
    uint32_t  fp;         /* FNV della chiave                    */
    uint32_t  scadenza;    /* tick di morte; 0 = senza scadenza   */
    struct voce *anello_prox, *anello_prev;   /* lista della cella */
} voce_t;

typedef struct {
    uint32_t fp;
    voce_t  *voce;         /* NULL = slot vuoto                   */
} slot_t;

typedef struct {
    slot_t    *slot;
    size_t     n_slot;
    size_t     n_vive;
    voce_t    *anello[N_TICK];   /* catene per tick                */
    uint32_t   tick;
    uint64_t   scadute_totali;
} db_t;

/* ------------------------------------------------------------- timer */

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
    static uint32_t x = 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

static uint32_t fnv(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)(uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------ indice */

static size_t trova_voce(const db_t *db, const char *key, size_t klen, uint32_t fp)
{
    size_t idx = fp & (db->n_slot - 1);
    for (;;) {
        voce_t *v = db->slot[idx].voce;
        if (v == NULL)
            return (size_t)-1;
        if (db->slot[idx].fp == fp &&
            strlen(v->chiave) == klen &&
            memcmp(v->chiave, key, klen) == 0)
            return idx;
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
        if (db->slot[v].voce == NULL)
            continue;
        size_t idx = db->slot[v].fp & (nuovo_n - 1);
        while (nuovo[idx].voce != NULL)
            idx = (idx + 1) & (nuovo_n - 1);
        nuovo[idx] = db->slot[v];
    }
    free(db->slot);
    db->slot = nuovo;
    db->n_slot = nuovo_n;
    return DB_OK;
}

/*
 * BACKSHIFT — il cuore del file.
 * Svuota lo slot `i` e ripara la catena. BUG FIX (trovato dal
 * validatore): la prima versione si FERMAVA al primo candidato non
 * spostabile (else break). Sbagliato: le case lungo una catena non
 * sono monotòne — A puo' stare a casa sua (insostituibile nel buco)
 * mentre B, due slot piu' in la', ha casa PRIMA del buco e va spostato
 * comunque. La scansione prosegue fino al primo slot VUOTO (fine del
 * gruppo): ogni candidato sposta se la sua casa k raggiunge il buco i
 * prima di arrivare a j: distanza ciclica (i-k) < (j-k).
 */
static void svuota_slot(db_t *db, size_t i)
{
    db->slot[i].voce = NULL;
    size_t buco = i;
    size_t j = (i + 1) & (db->n_slot - 1);
    while (db->slot[j].voce != NULL) {
        size_t k = db->slot[j].fp & (db->n_slot - 1);
        size_t d_buco = (buco - k) & (db->n_slot - 1);
        size_t d_ora  = (j - k) & (db->n_slot - 1);
        if (d_buco < d_ora) {
            db->slot[buco] = db->slot[j];     /* trasloca indietro */
            db->slot[j].voce = NULL;
            buco = j;                         /* il guado lo segue */
        }
        j = (j + 1) & (db->n_slot - 1);
    }
}

/* ---------------------------------------------------------- anello */

static void anello_aggancia(db_t *db, voce_t *v)
{
    voce_t **cella = &db->anello[v->scadenza % N_TICK];
    v->anello_prev = NULL;
    v->anello_prox = *cella;
    if (*cella != NULL)
        (*cella)->anello_prev = v;
    *cella = v;
}

static void anello_sgancia(db_t *db, voce_t *v)
{
    voce_t **cella = &db->anello[v->scadenza % N_TICK];
    if (v->anello_prev != NULL)
        v->anello_prev->anello_prox = v->anello_prox;
    else
        *cella = v->anello_prox;
    if (v->anello_prox != NULL)
        v->anello_prox->anello_prev = v->anello_prev;
}

/* ---------------------------------------------------- vita e morte */

/* VALIDATORE (solo diagnostica): ogni entry deve essere raggiungibile
 * dal proprio slot canonico senza incontrare un buco: l'invariante del
 * probing lineare. La prima violazione dice chi rompe. */
static void valida_tabella(const db_t *db, const char *dove)
{
    for (size_t s = 0; s < db->n_slot; s++) {
        const voce_t *v = db->slot[s].voce;
        if (v == NULL)
            continue;
        size_t da = db->slot[s].fp & (db->n_slot - 1);
        for (size_t p = da; ; p = (p + 1) & (db->n_slot - 1)) {
            if (db->slot[p].voce == NULL) {
                fprintf(stderr, "[VALIDA] ROTTURA %s: entry %s a slot %zu "
                        "irraggiungibile da %zu (buco a %zu) vive=%zu\n",
                        dove, v->chiave, s, da, p, db->n_vive);
                abort();
            }
            if (p == s)
                break;
        }
    }
}

static void uccidi_voce(db_t *db, voce_t *v)
{
    if (v->scadenza != 0)
        anello_sgancia(db, v);
    size_t s_idx = trova_voce(db, v->chiave, strlen(v->chiave), v->fp);
    if (s_idx == (size_t)-1) {
        /* invariante rotto: l'indice ha perso una voce viva */
        fprintf(stderr, "rottura indice: %s non trovata\n", v->chiave);
        abort();
    }
    svuota_slot(db, s_idx);
    free(v->chiave);
    free(v->valore);
    free(v);
    db->n_vive--;
}

int db_init(db_t *db)
{
    memset(db, 0, sizeof *db);
    db->n_slot = SLOT_INIZIALI;
    db->slot = calloc(db->n_slot, sizeof *db->slot);
    if (!db->slot)
        return DB_ERR_MEMORY;
    return DB_OK;
}

void db_libera(db_t *db)
{
    /* BUG FIX (doppio free): la prima versione liberava le voci DAL
     * GIRO DELL'ANELLO e poi di nuovo DAL GIRO DEGLI SLOT (stessi nodi):
     * puntatori pending, segfault alla fine dei test. La verita' e'
     * piu' semplice: ogni voce VIVA ha esattamente UNO slot (l'invariante
     * dell'indice): il teardown passa SOLO dagli slot, l'anello con le
     * sue catene e' solo il meccanismo di scadenza, non un proprietario. */
    for (size_t i = 0; i < db->n_slot; i++) {
        voce_t *v = db->slot[i].voce;
        if (v != NULL) {
            free(v->chiave);
            free(v->valore);
            free(v);
            db->slot[i].voce = NULL;
        }
    }
    free(db->slot);
    memset(db, 0, sizeof *db);
}

int db_put(db_t *db, const char *key, size_t klen,
           const char *valore, size_t vlen, uint32_t ttl)
{
    if (ttl > MAX_TTL)
        return DB_ERR_TTL;

    uint32_t fp = fnv(key, klen);
    size_t idx = trova_voce(db, key, klen, fp);
    if (idx != (size_t)-1) {
        /* update: nuovo valore, nuova scadenza */
        voce_t *v = db->slot[idx].voce;
        char *nv = malloc(vlen + 1);
        if (!nv)
            return DB_ERR_MEMORY;
        memcpy(nv, valore, vlen);
        nv[vlen] = '\0';
        free(v->valore);
        v->valore = nv;
        if (v->scadenza != 0)
            anello_sgancia(db, v);
        v->scadenza = ttl ? db->tick + ttl : 0;
        if (ttl)
            anello_aggancia(db, v);
        return DB_OK;
    }

    if ((db->n_vive + 1) * 10 > db->n_slot * LOAD_FACTOR_10)
        if (cresci_indice(db) != DB_OK)
            return DB_ERR_MEMORY;

    voce_t *v = malloc(sizeof *v);
    char *k = malloc(klen + 1), *val = malloc(vlen + 1);
    if (!v || !k || !val) {
        free(v); free(k); free(val);
        return DB_ERR_MEMORY;
    }
    memcpy(k, key, klen); k[klen] = '\0';
    memcpy(val, valore, vlen); val[vlen] = '\0';
    v->chiave = k;
    v->valore = val;
    v->fp = fp;
    v->scadenza = ttl ? db->tick + ttl : 0;

    size_t nu = fp & (db->n_slot - 1);
    while (db->slot[nu].voce != NULL)
        nu = (nu + 1) & (db->n_slot - 1);
    db->slot[nu].fp = fp;
    db->slot[nu].voce = v;

    if (ttl)
        anello_aggancia(db, v);
    db->n_vive++;
    return DB_OK;
}

int db_del(db_t *db, const char *key, size_t klen)
{
    uint32_t fp = fnv(key, klen);
    size_t idx = trova_voce(db, key, klen, fp);
    if (idx == (size_t)-1)
        return DB_ERR_MANCA;
    uccidi_voce(db, db->slot[idx].voce);
    return DB_OK;
}

const char *db_get(db_t *db, const char *key, size_t klen)
{
    uint32_t fp = fnv(key, klen);
    size_t idx = trova_voce(db, key, klen, fp);
    if (idx == (size_t)-1)
        return NULL;
    voce_t *v = db->slot[idx].voce;
    if (v->scadenza != 0 && v->scadenza <= db->tick) {
        /* il tempo non e' mai stato avanzato: scade qui, da solo */
        db->scadute_totali++;
        uccidi_voce(db, v);
        return NULL;
    }
    return v->valore;
}

/*
 * db_avanza — il muscolo dell'anello: attraversa i tick, cella dopo
 * cella. BUG FIX (caccia all'heap corruption, exit 0xC0000374): la
 * prima versione staccava la CELLA (testa a NULL) e poi "sganciava"
 * le vittime azzerando i LORO puntatori prev/prox — lasciando intatti
 * i puntatori dei VICINI verso i nodi liberati: catene con link a
 * memoria morta, prima scrittura = heap corrotto. Ora le vittime si
 * sganciano con l'unlink completo DALLA CATENA VIVA (anello_sgancia
 * dentro uccidi_voce): i vicini si aggiustano, i futures restano
 * attaccati alla cella e muoiono al giro giusto — niente orfani.
 */
void db_avanza(db_t *db, uint32_t tick_target)
{
    while (db->tick < tick_target) {
        db->tick++;
        voce_t *v = db->anello[db->tick % N_TICK];
        while (v != NULL) {
            voce_t *prox = v->anello_prox;
            if (v->scadenza <= db->tick) {
                db->scadute_totali++;
                uccidi_voce(db, v);   /* unlink completo + backshift */
            }
            v = prox;
        }
    }
}

void db_statistiche(const db_t *db)
{
    printf("tick=%u  vive=%zu  scadute in tutto=%llu  slot=%zu (LF %.0f%%)\n",
           db->tick, db->n_vive,
           (unsigned long long)db->scadute_totali,
           db->n_slot, 100.0 * (double)db->n_vive / (double)db->n_slot);
}

/* ---------------------------------------------------------------- test */

static int autotest(void)
{
    db_t db;
    char kbuf[24];
    int errori = 0;
    if (db_init(&db) != DB_OK)
        return 1;

    /* scenario deterministico: ATTENZIONE alla semantica: db_avanza
     * prende il tick TARGET assoluto, non un delta. (Il primo autotest
     * sbagliava proprio qui: avanza(6) dal tick 4 arriva a tick 6,
     * non a 10: le asserzioni sembravano assurde e il colpevole era
     * il chiamante.) */
    db_put(&db, "breve", 5, "vb", 2, 5);
    db_put(&db, "media", 5, "vm", 2, 10);
    db_put(&db, "eterna", 6, "ve", 2, 0);

    db_avanza(&db, 4);
    if (db_get(&db, "breve", 5) == NULL) {
        printf("autotest FALLITO: breve morta al tick 4 (scade a 5)\n");
        errori++;
    }
    db_avanza(&db, 10);
    if (db_get(&db, "breve", 5) != NULL) {
        printf("autotest FALLITO: breve viva dopo la scadenza\n");
        errori++;
    }
    if (db_get(&db, "media", 5) != NULL) {
        printf("autotest FALLITO: media viva al tick 10 (scade a 10)\n");
        errori++;
    }
    if (db_get(&db, "eterna", 6) == NULL) {
        printf("autotest FALLITO: eterna scomparsa (ttl=0)\n");
        errori++;
    }

    /* backshift sotto tempesta: mille chiavi in indice piccolo che
     * muoiono TUTTE INSIEME al tick 40: i sopravvissuti (le eterne)
     * devono restare raggiungibili, il validatore confema l'invariante
     * dell'indice dopo ogni riparazione */
    enum { N = 4000 };
    for (uint32_t i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "x%05u", i);
        db_put(&db, kbuf, (size_t)kl, "v", 1, 30);
    }
    db_avanza(&db, 40);
    valida_tabella(&db, "autotest post-tempesta");
    if (db_get(&db, "eterna", 6) == NULL) {
        printf("autotest FALLITO: eterna trascinata nella tempesta\n");
        errori++;
    }
    for (uint32_t i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "x%05u", i);
        if (db_get(&db, kbuf, (size_t)kl) != NULL) {
            printf("autotest FALLITO: %s non scaevenuta al tick %u\n",
                   kbuf, db.tick);
            errori++;
            break;
        }
    }
    /* reinserimento immediato nei nodi riciclati */
    for (uint32_t i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "x%05u", i);
        db_put(&db, kbuf, (size_t)kl, "v2", 2, 9);
    }
    valida_tabella(&db, "autotest post-reinserimento");
    for (uint32_t i = 0; i < N; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "x%05u", i);
        const char *r = db_get(&db, kbuf, (size_t)kl);
        if (r == NULL || strcmp(r, "v2") != 0) {
            printf("autotest FALLITO: %s persa dopo reinserimento\n", kbuf);
            errori++;
            break;
        }
    }
    /* il ttl oltre la finestra rifiutato, non accettato in silenzio */
    if (db_put(&db, "troppo", 6, "v", 1, MAX_TTL + 1) != DB_ERR_TTL) {
        printf("autotest FALLITO: ttl oltre finestra accettato\n");
        errori++;
    }

    if (errori == 0)
        printf("autotest: scadenze deterministiche, backshift sotto "
               "tempesta (%d scelte sincrone, invariante verificata), "
               "riciclo nodi: OK\n", N);
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
    static char kbuf[32];

    /* --- scenario: 200000 inserti con orologio che avanza --- */
    enum { N_PUT = 200000 };
    printf("scenario: %d voci con ttl, l'orologio avanza mentre scrivi\n\n",
           N_PUT);

    double t0 = ora_ms();
    uint32_t ultimo = 0;
    for (uint32_t i = 0; i < N_PUT; i++) {
        uint32_t tick = i / 64;                 /* 3125 tick di scena */
        if (tick > ultimo) {
            db_avanza(&db, tick);
            ultimo = tick;
        }
        int kl = snprintf(kbuf, sizeof kbuf, "k%07u", i);
        db_put(&db, kbuf, (size_t)kl, "valore", 6, 1 + (prng() % 1200));
    }
    printf("inserite %d voci con avanzamento a fianco: %.0f ms "
           "(orologio a tick %u)\n", N_PUT, ora_ms() - t0, db.tick);
    db_statistiche(&db);

    /* --- il resto dell'anello: quanto costa finire il giro? --- */
    t0 = ora_ms();
    uint64_t prima = db.scadute_totali;
    db_avanza(&db, db.tick + 2000);
    double av_ms = ora_ms() - t0;
    printf("avanzati 2000 tick in %.2f ms: %llu scadenze gestite (%.1f "
           "scadenze/ms, O(1) per scadenza: zero scansioni)\n",
           av_ms,
           (unsigned long long)(db.scadute_totali - prima),
           (double)(db.scadute_totali - prima) / (av_ms > 0.01 ? av_ms : 0.01));
    db_statistiche(&db);

    /* --- il caso estremo: 50000 voci con la STESSA scadenza --- */
    enum { N_STESSA = 50000 };
    t0 = ora_ms();
    for (uint32_t i = 0; i < N_STESSA; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "stessa%06u", i);
        db_put(&db, kbuf, (size_t)kl, "v", 1, 500);
    }
    uint32_t ora_tick = db.tick;
    printf("\n%d voci con la stessa scadenza (tick %u): inserimento %.0f ms\n",
           N_STESSA, ora_tick + 500, ora_ms() - t0);

    t0 = ora_ms();
    uint64_t pr2 = db.scadute_totali;
    db_avanza(&db, ora_tick + 500);
    double tempesta = ora_ms() - t0;
    printf("il tick mortale: 50000 scadenze insieme gestite in %.1f ms "
           "(%.3f us per scadenza, backshift esaustivo)\n",
           tempesta,
           tempesta * 1000.0 / (double)(db.scadute_totali - pr2));
    db_statistiche(&db);

    printf("\n");
    if (autotest() != 0)
        return 1;

    db_libera(&db);
    return 0;
}
