/*
 * demo_prefetch.c — il banco di prova del prefetcher speculativo
 * (capitolo 12).
 *
 * Non e' una demo: ogni regola del contratto [P1]..[P9] ha un gate
 * con soglia numerata, e un gate che salta ferma tutto con EXIT!=0.
 * Il banco chiama la meccanica in process e il filo MCP del capitolo
 * 11 per la prova end-to-end: il colpo attraversa davvero il filo.
 *
 *   GATE [P1] la coda e' la piu' viva: la traccia lunga predice dal
 *              pensiero recente; il pensiero oltre le 64 finestre
 *              NON predice (il cap e' parte del contratto)
 *   GATE [P2] la chiave e' COPIATA: una put dopo la predizione non
 *              muove la predizione; il costo di scoperta nasce con
 *              lei ed e' > 0
 *   GATE [P3] il dedup: una chiave gia' nella traccia non si propone
 *   GATE [P4] lo staging e' un contratto: generazione consumata una
 *              volta sola; file strappato = silenzio, parcheggio
 *              intatto; la spesa handle si accumula
 *   GATE [P5] la speculazione scade da se': vita 32 richieste, il
 *              colpo conta UNA volta, il morto senza colpo e'
 *              inquinamento contato
 *   GATE [P6] il bilancio in byte, end-to-end sul filo: hit, spesa,
 *              risparmio, bilancio > 0 e hit_rate >= 0.50 (la soglia
 *              del capitolo: sotto, la speculazione inquina)
 *   GATE [P7] il journal: la traccia esce dai pensieri recenti,
 *              dedup per testo, dal vecchio al nuovo
 *   GATE [MISURA] predici p99 < 10 ms su traccia da 8 KB;
 *              consuma p99 < 2 ms; tocca p99 < 20 us
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
# include <direct.h>
#else
# include <sys/stat.h>
#endif

#include "motore.h"
#include "mcp_server.h"
#include "prefetch.h"
#include "json.h"

#define N_FILLER     200
#define N_CAMPIONI   200

/* ----------------------------------------------------------- utilita' */

static double ora_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = { { 0, 0 } };
    LARGE_INTEGER c;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
#endif
}

static uint32_t prng(void)
{
    static uint32_t x = 0x12345678u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile(double *v, size_t n, double q)
{
    qsort(v, n, sizeof *v, cmp_double);
    size_t i = (size_t)(q * (double)(n - 1) + 0.5);
    if (i >= n)
        i = n - 1;
    return v[i];
}

static void crea_dir(const char *percorso)
{
#ifdef _WIN32
    _mkdir(percorso);
#else
    mkdir(percorso, 0700);
#endif
}

static void rimuovi_dir(const char *percorso)
{
#ifdef _WIN32
    _rmdir(percorso);
#else
    rmdir(percorso);
#endif
}

/* ------------------------------------------------- il filo del banco */

static char *grezzo(mcp_t *s, const char *req, size_t *risposta_len)
{
    char *r = NULL;
    size_t rl = 0;
    if (mcp_gestisci(s, req, strlen(req), &r, &rl) != 0) {
        fprintf(stderr, "memoria MCP\n");
        exit(1);
    }
    if (risposta_len)
        *risposta_len = rl;
    return r;
}

static jval_t *chiama(mcp_t *s, const char *req)
{
    size_t rl = 0;
    char *r = grezzo(s, req, &rl);
    if (!r) {
        fprintf(stderr, "GATE ROTTO: nessuna risposta a: %.80s\n", req);
        exit(1);
    }
    jval_t *v = json_parse(r, rl);
    if (!v) {
        fprintf(stderr, "GATE ROTTO: risposta non-JSON: %s\n", r);
        exit(1);
    }
    free(r);
    return v;
}

static jval_t *payload_di(jval_t *risp)
{
    const jval_t *res = json_get(risp, "result");
    const jval_t *content = json_get(res, "content");
    if (!content || content->tipo != J_ARR || content->n == 0) {
        fprintf(stderr, "GATE ROTTO: envelope senza content\n");
        exit(1);
    }
    size_t tl = 0;
    const char *testo = json_str(json_get(content->figli[0], "text"), &tl);
    if (!testo) {
        fprintf(stderr, "GATE ROTTO: content senza text\n");
        exit(1);
    }
    jval_t *p = json_parse(testo, tl);
    if (!p) {
        fprintf(stderr, "GATE ROTTO: payload non-JSON: %.*s\n",
                (int)tl, testo);
        exit(1);
    }
    return p;
}

static jval_t *tool(mcp_t *s, const char *nome, const char *args)
{
    char req[4096];
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
             nome, args);
    jval_t *risp = chiama(s, req);
    jval_t *p = payload_di(risp);
    json_libera(risp);
    return p;
}

/* ------------------------------------------------------------- main */

/* il riempitivo: parole neutre che non condividono bigrammi con le
 * chiavi vere (lettere a caso: la similarita' sintattica non puo'
 * confonderle con "capitolo") */
static void riempi_neutro(char *dest, size_t cap, size_t parole)
{
    size_t n = 0;
    for (size_t p = 0; p < parole; p++) {
        for (size_t l = 0; l < 6; l++) {
            if (n + 2 >= cap)
                break;
            dest[n++] = (char)('a' + prng() % 26);
        }
        if (n + 2 >= cap)
            break;                      /* il cap si rispetta sempre  */
        dest[n++] = ' ';
    }
    dest[n > 0 ? n - 1 : 0] = '\0';
}

static int contiene(const prefetch_predizione_t pred[], size_t n,
                    const char *chiave)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(pred[i].chiave, chiave) == 0)
            return (int)i + 1;
    return 0;
}

int main(void)
{
    printf("==== CAPITOLO 12: IL PREFETCHER SPECULATIVO — "
           "la memoria che guarda i pensieri ====\n\n");

    /* il motore del banco: una memoria di lavoro realistica */
    motore_t *m = motore_apri();
    if (!m) {
        fprintf(stderr, "memoria\n");
        return 1;
    }
    motore_put(m, "in-corso/capitolo-12", strlen("in-corso/capitolo-12"),
               "prefetcher speculativo: contratto P1 P9, staging a"
               " chiavi, hook pre agent",
               strlen("prefetcher speculativo: contratto P1 P9, staging"
                      " a chiavi, hook pre agent"), 0);
    motore_put(m, "in-corso/mcp-layer", strlen("in-corso/mcp-layer"),
               "server MCP dieci tool, json.c una fonte di verita'",
               strlen("server MCP dieci tool, json.c una fonte di"
                      " verita'"), 0);
    motore_put(m, "nota/post-mortem-orologio",
               strlen("nota/post-mortem-orologio"),
               "il file PDB1 non porta il clock: tick statico piu'"
               " orologio crescente fa il wrap",
               strlen("il file PDB1 non porta il clock: tick statico"
                      " piu' orologio crescente fa il wrap"), 0);
    char chiave[64], valore[64];
    for (uint32_t i = 0; i < N_FILLER; i++) {
        snprintf(chiave, sizeof chiave, "nota/voce-%03u", i);
        snprintf(valore, sizeof valore, "riempitivo %u", i);
        motore_put(m, chiave, strlen(chiave), valore, strlen(valore), 0);
    }

    /* ---- [P1] la coda e' la piu' viva: le finestre guardano la
     * coda della traccia, e oltre il cap non si vede */
    printf("[P1] la coda della traccia predice, il cap taglia...\n");
    char traccia[PREFETCH_TRACCIA_MAX];
    prefetch_predizione_t pred[PREFETCH_PREDIZIONI_MAX];

    riempi_neutro(traccia, sizeof traccia, 120);
    strcat(traccia,
           " adesso implementiamo il prefetcher speculativo del"
           " capitolo dodici la memoria che guarda i pensieri");
    size_t n = prefetch_predici(m, traccia, strlen(traccia), pred,
                                PREFETCH_PREDIZIONI_MAX);
    int pos = contiene(pred, n, "in-corso/capitolo-12");
    if (!pos || pred[pos - 1].confidenza < 0.05) {
        fprintf(stderr, "GATE [P1] ROTTO: la coda non ha predetto"
                        " in-corso/capitolo-12 (n=%zu)\n", n);
        return 1;
    }
    printf("     coda viva: predetto in-corso/capitolo-12"
           " (confidenza %.3f, posto %d)\n",
           pred[pos - 1].confidenza, pos);

    /* lo stesso pensiero, ma all'INIZIO di una traccia lunga: oltre
     * le 64 finestre non si vede, e non deve predire */
    snprintf(traccia, sizeof traccia,
             "il prefetcher speculativo del capitolo dodici");
    char neutro[2048];
    riempi_neutro(neutro, sizeof neutro, 300);
    strcat(traccia, " ");
    strcat(traccia, neutro);
    n = prefetch_predici(m, traccia, strlen(traccia), pred,
                         PREFETCH_PREDIZIONI_MAX);
    if (contiene(pred, n, "in-corso/capitolo-12")) {
        fprintf(stderr, "GATE [P1] ROTTO: il pensiero oltre il cap"
                        " di finestre ha predetto\n");
        return 1;
    }
    printf("     cap onesto: lo stesso pensiero oltre le 64 finestre"
           " non predice\n");

    /* ---- [P2] la chiave e' copiata, il costo nasce con lei */
    printf("[P2] la predizione sopravvive alla put (chiave copiata)...\n");
    riempi_neutro(traccia, sizeof traccia, 10);
    strcat(traccia,
           " il prefetcher speculativo capitolo dodici contratto");
    n = prefetch_predici(m, traccia, strlen(traccia), pred,
                         PREFETCH_PREDIZIONI_MAX);
    pos = contiene(pred, n, "in-corso/capitolo-12");
    if (!pos || pred[pos - 1].costo_scoperta == 0) {
        fprintf(stderr, "GATE [P2] ROTTO: niente predizione o costo"
                        " di scoperta nullo\n");
        return 1;
    }
    uint64_t costo = pred[pos - 1].costo_scoperta;
    char copia[PREFETCH_CHIAVE_MAX];
    snprintf(copia, sizeof copia, "%s", pred[pos - 1].chiave);
    motore_put(m, "nota/spinta", 11, "l'arena si muove", 17, 0); /* muove
                                                                   l'arena */
    if (strcmp(copia, pred[pos - 1].chiave) != 0) {
        fprintf(stderr, "GATE [P2] ROTTO: la chiave copiata si e'"
                        " mossa con l'arena\n");
        return 1;
    }
    printf("     chiave copiata intatta dopo la put; costo di"
           " scoperta %llu byte\n", (unsigned long long)costo);

    /* ---- [P3] il dedup: gia' in vista non si ripropone */
    printf("[P3] il dedup: la chiave in traccia non si ripropone...\n");
    riempi_neutro(traccia, sizeof traccia, 10);
    strcat(traccia,
           " ripenso a in-corso/capitolo-12 per il capitolo dodici");
    n = prefetch_predici(m, traccia, strlen(traccia), pred,
                         PREFETCH_PREDIZIONI_MAX);
    if (contiene(pred, n, "in-corso/capitolo-12")) {
        fprintf(stderr, "GATE [P3] ROTTO: proposta una chiave gia'"
                        " in vista\n");
        return 1;
    }
    printf("     dedup: in-corso/capitolo-12 era in traccia e non"
           " e' stato riproposto\n");

    /* ---- [P4] lo staging e' un contratto tra due processi */
    printf("[P4] lo staging: una generazione, una consumazione...\n");
    const char *staging = "banco12.staging";
    remove(staging);
    prefetch_parcheggio_t *p = prefetch_parcheggio_apri(
        PREFETCH_PARCHEGGIO_MAX, PREFETCH_TTL_RICHIESTE);
    if (!p) {
        fprintf(stderr, "memoria\n");
        return 1;
    }
    /* due predizioni fisse: il gate non dipende dal caso del banco */
    prefetch_predizione_t due[2];
    memset(due, 0, sizeof due);
    snprintf(due[0].chiave, PREFETCH_CHIAVE_MAX, "in-corso/colpita");
    due[0].chiave_len = strlen(due[0].chiave);
    due[0].confidenza = 0.5;
    due[0].costo_scoperta = 400;
    snprintf(due[1].chiave, PREFETCH_CHIAVE_MAX, "in-corso/mai-toccata");
    due[1].chiave_len = strlen(due[1].chiave);
    due[1].confidenza = 0.4;
    due[1].costo_scoperta = 300;
    if (prefetch_scrivi_staging(staging, "turn-1", due, 2, 123) != 0) {
        fprintf(stderr, "GATE [P4] ROTTO: scrittura staging fallita\n");
        return 1;
    }
    size_t parcheggiate = prefetch_consuma(p, staging, 1);
    if (parcheggiate != 2) {
        fprintf(stderr, "GATE [P4] ROTTO: parcheggiate %zu != 2\n",
                parcheggiate);
        return 1;
    }
    if (prefetch_consuma(p, staging, 2) != 0) {
        fprintf(stderr, "GATE [P4] ROTTO: la stessa generazione e'"
                        " stata consumata due volte\n");
        return 1;
    }
    if (prefetch_scrivi_staging(staging, "turn-2", due, 2, 45) != 0) {
        fprintf(stderr, "GATE [P4] ROTTO: scrittura turn-2 fallita\n");
        return 1;
    }
    /* le stesse chiavi: ri-confermate (vita rinfrescata), NON contate
     * come nuove — il ritorno e' le voci NUOVE di questa consumazione */
    if (prefetch_consuma(p, staging, 3) != 0) {
        fprintf(stderr, "GATE [P4] ROTTO: le chiavi gia' parcheggiate"
                        " sono state contate due volte\n");
        return 1;
    }
    uint64_t vive_check = 0;
    prefetch_stat(p, 3, NULL, &vive_check, NULL, NULL, NULL, NULL);
    if (vive_check != 2) {
        fprintf(stderr, "GATE [P4] ROTTO: dopo la ri-conferma le vive"
                        " sono %llu, non 2\n",
                (unsigned long long)vive_check);
        return 1;
    }
    /* il file strappato: meta' JSON, spazzatura. Silenzio [P3] */
    FILE *f = fopen(staging, "wb");
    if (f) {
        fputs("{\"gen\":\"turn-3\",\"handle\":99,\"voci\":[{\"k\":\"x",
              f);
        fclose(f);
    }
    if (prefetch_consuma(p, staging, 4) != 0) {
        fprintf(stderr, "GATE [P4] ROTTO: il file strappato e' stato"
                        " consumato come buono\n");
        return 1;
    }
    uint64_t parch = 0, vive = 0, hit = 0, morte = 0, spesa = 0, ris = 0;
    prefetch_stat(p, 4, &parch, &vive, &hit, &morte, &spesa, &ris);
    if (spesa != 123 + 45) {
        fprintf(stderr, "GATE [P4] ROTTO: spesa handle %llu != 168\n",
                (unsigned long long)spesa);
        return 1;
    }
    printf("     consumata una volta per generazione; strappato ="
           " silenzio; spesa handle %llu byte\n",
           (unsigned long long)spesa);

    /* ---- [P5] la speculazione scade da se', il colpo conta una volta */
    printf("[P5] il TTL in richieste e il colpo unico...\n");
    remove(staging);
    prefetch_parcheggio_t *pt = prefetch_parcheggio_apri(8, 32);
    if (!pt) {
        fprintf(stderr, "memoria\n");
        return 1;
    }
    if (prefetch_scrivi_staging(staging, "turn-ttl", due, 2, 10) != 0 ||
        prefetch_consuma(pt, staging, 1) != 2) {
        fprintf(stderr, "GATE [P5] ROTTO: parcheggio del TTL fallito\n");
        return 1;
    }
    if (!prefetch_tocca(pt, due[0].chiave, due[0].chiave_len, 32)) {
        fprintf(stderr, "GATE [P5] ROTTO: la viva a giro 32 (31 di"
                        " vita) non ha colpito\n");
        return 1;
    }
    if (!prefetch_tocca(pt, due[0].chiave, due[0].chiave_len, 32)) {
        fprintf(stderr, "GATE [P5] ROTTO: il secondo colpo non e'"
                        " tornato 1\n");
        return 1;
    }
    if (prefetch_tocca(pt, due[0].chiave, due[0].chiave_len, 34)) {
        fprintf(stderr, "GATE [P5] ROTTO: il morto (vita 32, giro 33)"
                        " ha colpito\n");
        return 1;
    }
    prefetch_consuma(pt, staging, 100);  /* lo sweep in batch [P6] */
    prefetch_stat(pt, 100, &parch, &vive, &hit, &morte, &spesa, &ris);
    if (hit != 1 || morte != 1 || vive != 0) {
        fprintf(stderr, "GATE [P5] ROTTO: hit %llu, morte %llu,"
                        " vive %llu (attesi 1, 1, 0)\n",
                (unsigned long long)hit, (unsigned long long)morte,
                (unsigned long long)vive);
        return 1;
    }
    if (ris != 400) {
        fprintf(stderr, "GATE [P5] ROTTO: risparmio %llu != 400\n",
                (unsigned long long)ris);
        return 1;
    }
    printf("     vita 32 richieste: colpo contato 1 volta, morta"
           " senza colpo = inquinamento contato\n");

    /* ---- [P6] il bilancio in byte, end-to-end sul filo MCP */
    printf("[P6] il bilancio attraversa il filo: get colpisce,"
           " stat misura...\n");
    motore_t *mfile = motore_apri();
    if (!mfile) {
        fprintf(stderr, "memoria\n");
        return 1;
    }
    motore_put(mfile, "in-corso/capitolo-12",
               strlen("in-corso/capitolo-12"),
               "le sei decisioni del capitolo dodici: finestre dalla"
               " traccia, staging a chiavi, hook pre agent, banco"
               " reale, gate hit rate numerato, geometria chiusa",
               strlen("le sei decisioni del capitolo dodici: finestre"
                      " dalla traccia, staging a chiavi, hook pre"
                      " agent, banco reale, gate hit rate numerato,"
                      " geometria chiusa"), 0);
    if (motore_salva(mfile, "banco12.db") != MOTORE_OK) {
        fprintf(stderr, "GATE [P6] ROTTO: salvataggio del db fallito\n");
        return 1;
    }
    motore_chiudi(mfile);

    mcp_t *s = mcp_apri("banco12.db", 1, NULL);
    if (!s) {
        fprintf(stderr, "memoria MCP\n");
        return 1;
    }
    char percorso_staging[512];
    prefetch_percorso_staging("banco12.db", percorso_staging,
                              sizeof percorso_staging);
    /* la predizione parcheggiata e' la chiave che il get tirera' */
    snprintf(due[0].chiave, PREFETCH_CHIAVE_MAX, "in-corso/capitolo-12");
    due[0].chiave_len = strlen(due[0].chiave);
    due[0].confidenza = 0.42;
    due[0].costo_scoperta = 500;
    if (prefetch_scrivi_staging(percorso_staging, "turn-e2e",
                                due, 1, 120) != 0) {
        fprintf(stderr, "GATE [P6] ROTTO: staging end-to-end fallito\n");
        return 1;
    }
    jval_t *r = tool(s, "get",
                     "{\"chiave\":\"in-corso/capitolo-12\"}");
    size_t vl = 0;
    const char *v = json_str(json_get(r, "valore"), &vl);
    if (!v || vl == 0) {
        fprintf(stderr, "GATE [P6] ROTTO: il get end-to-end e' muto\n");
        return 1;
    }
    json_libera(r);
    r = tool(s, "get", "{\"chiave\":\"in-corso/mcp-layer\"}");
    json_libera(r);                     /* il miss: niente di finto  */
    r = tool(s, "prefetch_stat", "{}");
    int hv = 0;
    hit = (uint64_t)json_num(json_get(r, "hit"), &hv);
    int sv = 0;
    spesa = (uint64_t)json_num(json_get(r, "spesa_handle"), &sv);
    int rv = 0;
    ris = (uint64_t)json_num(json_get(r, "risparmio"), &rv);
    int bv = 0;
    double bilancio = json_num(json_get(r, "bilancio"), &bv);
    int ratev = 0;
    double hit_rate = json_num(json_get(r, "hit_rate"), &ratev);
    if (!hv || !sv || !rv || !bv || !ratev) {
        fprintf(stderr, "GATE [P6] ROTTO: prefetch_stat senza numeri\n");
        return 1;
    }
    if (hit != 1 || spesa != 120 || ris != 500) {
        fprintf(stderr, "GATE [P6] ROTTO: hit %llu spesa %llu"
                        " risparmio %llu (attesi 1, 120, 500)\n",
                (unsigned long long)hit, (unsigned long long)spesa,
                (unsigned long long)ris);
        return 1;
    }
    if (bilancio <= 0 || hit_rate < 0.50) {
        fprintf(stderr, "GATE [P6] ROTTO: bilancio %.0f, hit_rate %.2f"
                        " — la speculazione inquina\n",
                bilancio, hit_rate);
        return 1;
    }
    printf("     hit 1, spesa 120, risparmio 500, bilancio %.0f,"
           " hit_rate %.2f (soglia 0.50)\n", bilancio, hit_rate);
    json_libera(r);
    mcp_chiudi(s);

    /* ---- [P7] la traccia esce dal journal della sessione */
    printf("[P7] il journal: i pensieri recenti, dedup, dal vecchio"
           " al nuovo...\n");
    crea_dir("fake_session");
    crea_dir("fake_session/journal");
    f = fopen("fake_session/journal/0000000000000001.jsonl", "wb");
    if (!f) {
        fprintf(stderr, "memoria\n");
        return 1;
    }
    fputs("{\"payload\":{\"kind\":\"tool\",\"noise\":1}}\n", f);
    fputs("{\"payload\":{\"kind\":\"completion\",\"request\":"
          "{\"model_input\":{\"messages\":{\"messages\":["
          "{\"role\":\"assistant\",\"content\":["
          "{\"type\":\"reasoning\",\"content\":[{\"text\":"
          "\"penso al prefetcher speculativo capitolo\"}]},"
          "{\"type\":\"text\",\"text\":\"primo pensiero sullo staging\"}"
          "]}]}}}}}\n", f);
    fputs("{\"payload\":{\"kind\":\"completion\",\"request\":"
          "{\"model_input\":{\"messages\":{\"messages\":["
          "{\"role\":\"assistant\",\"content\":["
          "{\"type\":\"reasoning\",\"content\":[{\"text\":"
          "\"penso al prefetcher speculativo capitolo\"}]},"
          "{\"type\":\"text\",\"text\":\"secondo pensiero sul filo\"}"
          "]}]}}}}}\n", f);
    fclose(f);
    char buf[PREFETCH_TRACCIA_MAX];
    size_t tl = prefetch_traccia_da_journal("fake_session", buf,
                                            sizeof buf);
    const char *t1 = strstr(buf, "primo pensiero");
    const char *t2 = strstr(buf, "secondo pensiero");
    const char *tr = strstr(buf, "penso al prefetcher");
    size_t occorrenze = 0;
    for (const char *c = tr; c; c = strstr(c + 1, "penso al prefetcher"))
        occorrenze++;
    if (tl == 0 || !t1 || !t2 || !tr || occorrenze != 1 ||
        !((size_t)(t1 - buf) < (size_t)(t2 - buf) &&
          (size_t)(t2 - buf) < (size_t)(tr - buf))) {
        fprintf(stderr, "GATE [P7] ROTTO: traccia dal journal sbagliata"
                        " (tl %zu, occorrenze %zu)\n", tl, occorrenze);
        return 1;
    }
    if (prefetch_traccia_da_journal("dir-inesistente", buf,
                                    sizeof buf) != 0) {
        fprintf(stderr, "GATE [P7] ROTTO: journal inesistente non"
                        " e' silenzio\n");
        return 1;
    }
    printf("     due pensieri in ordine, dedup del ripetuto, journal"
           " muto = traccia vuota\n");

    /* ---- [MISURA] i numeri del capitolo */
    printf("[MISURA] predici su 8 KB di traccia, consuma, tocca...\n");
    riempi_neutro(traccia, sizeof traccia, 1100);
    strcat(traccia,
           " il prefetcher speculativo capitolo dodici la memoria"
           " che guarda i pensieri");
    double t_pred[N_CAMPIONI], t_cons[N_CAMPIONI], t_toc[N_CAMPIONI];
    for (int i = 0; i < N_CAMPIONI; i++) {
        double t0 = ora_us();
        n = prefetch_predici(m, traccia, strlen(traccia), pred,
                             PREFETCH_PREDIZIONI_MAX);
        t_pred[i] = ora_us() - t0;
    }
    remove(staging);
    prefetch_scrivi_staging(staging, "turn-misura", pred, n, 100);
    for (int i = 0; i < N_CAMPIONI; i++) {
        double t0 = ora_us();
        prefetch_consuma(p, staging, (uint64_t)(100 + i));
        t_cons[i] = ora_us() - t0;
    }
    for (int i = 0; i < N_CAMPIONI; i++) {
        double t0 = ora_us();
        prefetch_tocca(p, "in-corso/capitolo-12", 20,
                       (uint64_t)(100 + i));
        t_toc[i] = ora_us() - t0;
    }
    double p50 = percentile(t_pred, N_CAMPIONI, 0.50);
    double p99 = percentile(t_pred, N_CAMPIONI, 0.99);
    double c50 = percentile(t_cons, N_CAMPIONI, 0.50);
    double c99 = percentile(t_cons, N_CAMPIONI, 0.99);
    double k50 = percentile(t_toc, N_CAMPIONI, 0.50);
    double k99 = percentile(t_toc, N_CAMPIONI, 0.99);
    printf("     predici:  p50 %8.1f us  p99 %8.1f us"
           "  (%d finestre max, soglia %.2f)\n",
           p50, p99, PREFETCH_FINESTRE_MAX, PREFETCH_SOGLIA);
    printf("     consuma:  p50 %8.1f us  p99 %8.1f us\n", c50, c99);
    printf("     tocca:   p50 %8.1f us  p99 %8.1f us\n", k50, k99);
    if (p99 >= 10000.0) {
        fprintf(stderr, "GATE [MISURA] ROTTO: predici p99 %.1f us"
                        " >= 10000 us\n", p99);
        return 1;
    }
    if (c99 >= 2000.0) {
        fprintf(stderr, "GATE [MISURA] ROTTO: consuma p99 %.1f us"
                        " >= 2000 us\n", c99);
        return 1;
    }
    if (k99 >= 20.0) {
        fprintf(stderr, "GATE [MISURA] ROTTO: tocca p99 %.1f us"
                        " >= 20 us\n", k99);
        return 1;
    }

    prefetch_parcheggio_chiudi(p);
    prefetch_parcheggio_chiudi(pt);
    motore_chiudi(m);
    remove(staging);
    remove("banco12.db");
    remove("banco12.db.prefetch");
    remove("fake_session/journal/0000000000000001.jsonl");
    rimuovi_dir("fake_session/journal");
    rimuovi_dir("fake_session");

    printf("\nCAPITOLO 12: tutti i gate verdi. La speculazione e'"
           " misurata: ora la sessione vera decide se vive.\n");
    return 0;
}
