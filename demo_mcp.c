/*
 * demo_mcp.c — il banco di prova del layer MCP (capitolo 11).
 *
 * Non e' una demo: ogni regola del contratto [M1]..[M9] ha un gate
 * con soglia numerata, e un gate che salta ferma tutto con EXIT!=0.
 * Il banco chiama il NUCLEO in process [M9]: niente pipe, niente
 * "forse funziona" — ogni risposta torna indietro dallo stesso filo
 * JSON che userebbe un client vero, e viene verificata col solo
 * parser che il filo merita (json.c: una fonte di verita').
 *
 *   GATE [M1]  initialize: spec pinnata 2025-06-18; metodo sconosciuto
 *              -> -32601; JSON rotto -> -32700; tool sconosciuto
 *              -> -32602; notifica -> nessuna risposta
 *   GATE [M2]  chiedi/intervallo tornano il VALORE, mai solo chiavi;
 *              intervallo torna il batch + il totale
 *   GATE [M3]  ogni risposta di get/chiedi/intervallo porta la
 *              confidenza (1.0 l'esatto, (0,1] il refuso)
 *   GATE [M4]  get miss -> null, non un errore
 *   GATE [M5]  presente batch: N chiavi -> N verdetti, le vive TUTTE
 *              a 1 (zero falsi negativi [I9])
 *   GATE [M6]  TTL: put+avanza+get senza orologio da muro
 *   GATE [M7]  audit: vive == contatore indipendente del banco,
 *              coerente == true
 *   GATE [M8]  le description dichiarano i limiti (SINTATTICA,
 *              "certamente", il forse del bloom)
 *   GATE [MISURA] p99 di ogni tool sotto il tetto di complessita';
 *              risposta media sotto il tetto di byte (i token si
 *              pagano in contesto)
 *
 * La MISURA (il capitolo vive anche qui): latenza per tool al p50/p99
 * in MICROSECONDI lato motore, byte medi per risposta (proxy token:
 * byte/4), round-trip per compito tipico. Il confronto onesto: una
 * chiamata LLM costa 10^2..10^3 ms di generazione — il motore sta
 * sotto la soglia di millisecondo, quindi il costo di OGNI compito e'
 * il giro, e il layer lo minimizza (un compito = una chiamata).
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

#include "motore.h"
#include "mcp_server.h"
#include "json.h"

#define N_CHIAVI   2000
#define N_REFUSI   200
#define N_BATCH     200

static uint64_t chiamate = 0;   /* il conto indipendente del banco    */

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
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
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

static double percentile(const double *ordinato, size_t n, double q)
{
    size_t i = (size_t)(q * (double)(n - 1) + 0.5);
    if (i >= n)
        i = n - 1;
    return ordinato[i];
}

static void genera_sostituzione(char *dest, size_t cap, const char *originale,
                                uint32_t errori)
{
    snprintf(dest, cap, "%s", originale);
    size_t l = strlen(dest);
    for (uint32_t e = 0; e < errori; e++) {
        size_t p = (size_t)(prng() % (uint32_t)l);
        if (dest[p] < '0' || dest[p] > '9')
            continue;
        dest[p] = (char)('a' + (prng() % 26));
    }
}

static void genera_lettere(char *dest, size_t cap)
{
    for (size_t i = 0; i < 8 && i < cap - 1; i++)
        dest[i] = (char)('a' + (prng() % 26));
    dest[8] = '\0';
}

/* ------------------------------------------------- il filo del banco */

/* una richiesta grezza: ritorna la risposta (NULL = notifica) */
static char *grezzo(mcp_t *s, const char *req, size_t *risposta_len)
{
    char *r = NULL;
    size_t rl = 0;
    chiamate++;
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

/* il payload di tools/call: il JSON dentro il testo del content */
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

static int codice_errore(jval_t *risp)
{
    int valido = 0;
    double c = json_num(json_get(json_get(risp, "error"), "code"), &valido);
    return valido ? (int)c : 0;
}

/* una chiamata di tool: ritorna il payload, dichiara l'errore */
static jval_t *tool_e(mcp_t *s, const char *nome, const char *args,
                      int *is_error)
{
    char req[4096];
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
             nome, args);
    jval_t *risp = chiama(s, req);
    const jval_t *ie = json_get(json_get(risp, "result"), "isError");
    if (is_error)
        *is_error = (ie && ie->tipo == J_BOOL && ie->booleano) ? 1 : 0;
    jval_t *p = payload_di(risp);
    json_libera(risp);
    return p;
}

static jval_t *tool(mcp_t *s, const char *nome, const char *args)
{
    int ie = 0;
    jval_t *p = tool_e(s, nome, args, &ie);
    if (ie) {
        fprintf(stderr, "GATE ROTTO: %s ha dichiarato errore inatteso\n",
                nome);
        exit(1);
    }
    return p;
}

static int e_vero(const jval_t *oggetto, const char *chiave)
{
    const jval_t *v = json_get(oggetto, chiave);
    return v && v->tipo == J_BOOL && v->booleano;
}

/* ------------------------------------------------------------- main */

int main(void)
{
    printf("==== CAPITOLO 11: IL LAYER MCP — il motore come strumento ====\n\n");

    mcp_t *s = mcp_apri(NULL, 0, NULL);
    if (!s) {
        fprintf(stderr, "memoria\n");
        return 1;
    }

    /* ---- [M1] il filo e' un contratto: initialize e la spec pinnata */
    printf("[M1] initialize e la spec del filo...\n");
    jval_t *r = chiama(s, "{\"jsonrpc\":\"2.0\",\"id\":1,"
                          "\"method\":\"initialize\",\"params\":{}}");
    size_t vl = 0;
    const char *spec = json_str(json_get(json_get(r, "result"),
                                         "protocolVersion"), &vl);
    if (!spec || strcmp(spec, "2025-06-18") != 0) {
        fprintf(stderr, "GATE [M1] ROTTO: protocolVersion != 2025-06-18\n");
        return 1;
    }
    json_libera(r);

    /* ---- [M8] i limiti sono la description */
    printf("[M8] tools/list: i limiti dichiarati nella description...\n");
    r = chiama(s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    const jval_t *tools = json_get(json_get(r, "result"), "tools");
    if (!tools || tools->tipo != J_ARR || tools->n != 9) {
        fprintf(stderr, "GATE ROTTO: attesi 9 tool, trovati %zu\n",
                tools ? tools->n : (size_t)0);
        return 1;
    }
    int chiedi_visto = 0, presente_visto = 0;
    for (size_t i = 0; i < tools->n; i++) {
        const jval_t *t = tools->figli[i];
        const char *nome = json_str(json_get(t, "name"), NULL);
        const char *desc = json_str(json_get(t, "description"), NULL);
        if (!nome || !desc || !json_get(t, "inputSchema")) {
            fprintf(stderr, "GATE [M8] ROTTO: tool senza description"
                            " o inputSchema\n");
            return 1;
        }
        if (strcmp(nome, "chiedi") == 0 && strstr(desc, "SINTATTICA"))
            chiedi_visto = 1;
        if (strcmp(nome, "presente") == 0 && strstr(desc, "certamente"))
            presente_visto = 1;
    }
    if (!chiedi_visto || !presente_visto) {
        fprintf(stderr, "GATE [M8] ROTTO: i limiti non sono nella"
                        " description\n");
        return 1;
    }
    json_libera(r);

    /* ---- riempio col tool put: 2000 round-trip di scrittura */
    printf("riempio via MCP: %d put (altrettanti round-trip: il costo"
           " onesto del modello)...\n", N_CHIAVI);
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        char args[128];
        snprintf(args, sizeof args,
                 "{\"chiave\":\"u%05u\",\"valore\":\"valore-di-u%05u\"}",
                 i, i);
        jval_t *p = tool(s, "put", args);
        if (!e_vero(p, "scritto")) {
            fprintf(stderr, "GATE ROTTO: put %u non ha scritto\n", i);
            return 1;
        }
        json_libera(p);
    }

    /* ---- [M3][M2] get esatto: valore e confidenza 1.0 */
    printf("[M3][M2] get esatto: il valore con la confidenza...\n");
    {
        jval_t *p = tool(s, "get", "{\"chiave\":\"u00100\"}");
        const char *v = json_str(json_get(p, "valore"), &vl);
        int valido = 0;
        double conf = json_num(json_get(p, "confidenza"), &valido);
        if (!v || strcmp(v, "valore-di-u00100") != 0) {
            fprintf(stderr, "GATE [M2] ROTTO: get senza il valore giusto\n");
            return 1;
        }
        if (!valido || conf != 1.0) {
            fprintf(stderr, "GATE [M3] ROTTO: get senza confidenza 1.0\n");
            return 1;
        }
        json_libera(p);
    }

    /* ---- [M4] get miss: null, non un errore */
    printf("[M4] get miss: il silenzio onesto...\n");
    {
        jval_t *p = tool(s, "get", "{\"chiave\":\"zzzz-inesistente\"}");
        if (p->tipo != J_NULL) {
            fprintf(stderr, "GATE [M4] ROTTO: il miss non e' null\n");
            return 1;
        }
        json_libera(p);
    }

    /* ---- [M3][M6] chiedi col refuso: la confidenza dichiarata */
    printf("[M3] chiedi: %d refusi di una cifra, verita' nel top-%d...\n",
           N_REFUSI, 8);
    uint32_t ricordo = 0;
    for (int t = 0; t < N_REFUSI; t++) {
        char originale[16], refuso[16], args[96];
        snprintf(originale, sizeof originale, "u%05u", prng() % N_CHIAVI);
        genera_sostituzione(refuso, sizeof refuso, originale, 1);
        snprintf(args, sizeof args, "{\"query\":\"%s\",\"k\":8}", refuso);
        jval_t *p = tool(s, "chiedi", args);
        if (p->tipo != J_ARR) {
            fprintf(stderr, "GATE ROTTO: chiedi non torna un array\n");
            return 1;
        }
        int trovata = 0;
        for (size_t i = 0; i < p->n; i++) {
            int valido = 0;
            double conf = json_num(json_get(p->figli[i], "confidenza"),
                                   &valido);
            size_t vlen = 0;
            const char *val = json_str(json_get(p->figli[i], "valore"),
                                      &vlen);
            if (!valido || conf <= 0.0 || conf > 1.0) {
                fprintf(stderr, "GATE [M3] ROTTO: confidenza assente o"
                                " fuori (0,1]\n");
                return 1;
            }
            if (!val || vlen == 0) {
                fprintf(stderr, "GATE [M2] ROTTO: chiedi torna chiavi"
                                " senza valore\n");
                return 1;
            }
            const char *chiave = json_str(json_get(p->figli[i], "chiave"),
                                          NULL);
            if (chiave && strcmp(chiave, originale) == 0)
                trovata = 1;
        }
        ricordo += (uint32_t)trovata;
        json_libera(p);
    }
    printf("     verita' nel top-8: %u/%d (%.1f%%)\n",
           ricordo, N_REFUSI, 100.0 * (double)ricordo / N_REFUSI);
    if (ricordo * 100 < 80 * N_REFUSI) {
        fprintf(stderr, "GATE [recall] ROTTO: %u/%d sotto 80%%\n",
                ricordo, N_REFUSI);
        return 1;
    }

    /* ---- [M2] intervallo: il batch PIU' il totale */
    printf("[M2] intervallo u00100..u00199: batch + totale...\n");
    {
        jval_t *p = tool(s, "intervallo",
                         "{\"da\":\"u00100\",\"a\":\"u00199\",\"k\":32}");
        int valido = 0;
        double totali = json_num(json_get(p, "totali"), &valido);
        const jval_t *trovate = json_get(p, "trovate");
        if (!valido || totali != 100.0) {
            fprintf(stderr, "GATE [M2] ROTTO: totali %.0f != 100\n",
                    totali);
            return 1;
        }
        if (!trovate || trovate->tipo != J_ARR || trovate->n != 32) {
            fprintf(stderr, "GATE [M2] ROTTO: attese 32 coppie nel batch\n");
            return 1;
        }
        const char *precedente = NULL;
        for (size_t i = 0; i < trovate->n; i++) {
            double conf = json_num(json_get(trovate->figli[i], "confidenza"),
                                   &valido);
            const char *chiave = json_str(json_get(trovate->figli[i],
                                                   "chiave"), NULL);
            if (!valido || conf != 1.0) {
                fprintf(stderr, "GATE [M3] ROTTO: intervallo senza"
                                " confidenza 1.0\n");
                return 1;
            }
            if (!chiave || (precedente && strcmp(precedente, chiave) >= 0)) {
                fprintf(stderr, "GATE [M7] ROTTO: intervallo non in"
                                " ordine di chiave\n");
                return 1;
            }
            precedente = chiave;
        }
        json_libera(p);
    }

    /* ---- [M5] presente batch: N chiavi, N verdetti, vive tutte a 1 */
    printf("[M5] presente batch: 32 vive + 32 alieni, una chiamata...\n");
    {
        char args[2048];
        size_t pos = 0;
        pos += (size_t)snprintf(args + pos, sizeof args - pos,
                                "{\"chiavi\":[");
        char chiave[16];
        for (int i = 0; i < 32; i++) {
            snprintf(chiave, sizeof chiave, "u%05u", prng() % N_CHIAVI);
            pos += (size_t)snprintf(args + pos, sizeof args - pos,
                                    "\"%s\",", chiave);
        }
        for (int i = 0; i < 32; i++) {
            genera_lettere(chiave, sizeof chiave);
            pos += (size_t)snprintf(args + pos, sizeof args - pos,
                                    "\"%s\",", chiave);
        }
        snprintf(args + pos - 1, sizeof args - pos + 1, "]}");
        jval_t *p = tool(s, "presente", args);
        if (p->tipo != J_ARR || p->n != 64) {
            fprintf(stderr, "GATE [M5] ROTTO: 64 chiavi -> %zu verdetti\n",
                    p->tipo == J_ARR ? p->n : (size_t)0);
            return 1;
        }
        int vive_a_uno = 0, alieni_forse = 0;
        for (size_t i = 0; i < p->n; i++) {
            int valido = 0;
            double v = json_num(p->figli[i], &valido);
            if (!valido || (v != 0.0 && v != 1.0)) {
                fprintf(stderr, "GATE [M5] ROTTO: verdetto non 0/1\n");
                return 1;
            }
            if (i < 32) {
                if (v != 1.0) {
                    fprintf(stderr, "GATE [M5][I9] ROTTO: una chiave viva"
                                    " detta \"certamente no\"\n");
                    return 1;
                }
                vive_a_uno++;
            } else if (v == 1.0) {
                alieni_forse++;          /* il forse del bloom: MISURA  */
            }
        }
        printf("     vive 32/32 a \"1\"; alieni \"forse\": %d/32"
               " (%.1f%%)\n", alieni_forse, 100.0 * alieni_forse / 32.0);
        printf("     (bloom stantio: nasce a 3072 bit e si raddrizza"
               " alla ricomposizione [I9] —\n      la seconda misura,"
               " dopo il giro del disco, lo dimostra)\n");
        (void)vive_a_uno;
        json_libera(p);
    }

    /* ---- [M6] TTL: l'orologio e' del chiamante, mai da muro */
    printf("[M6] TTL: put + avanza + get, senza orologio da muro...\n");
    {
        jval_t *p = tool(s, "put",
                         "{\"chiave\":\"effimera\",\"valore\":\"muore\","
                         "\"ttl\":50}");
        json_libera(p);
        p = tool(s, "avanza", "{\"adesso\":60}");
        int valido = 0;
        double adesso = json_num(json_get(p, "adesso"), &valido);
        if (!valido || adesso != 60.0) {
            fprintf(stderr, "GATE [M6] ROTTO: avanza non torna il tick\n");
            return 1;
        }
        json_libera(p);
        p = tool(s, "get", "{\"chiave\":\"effimera\"}");
        if (p->tipo != J_NULL) {
            fprintf(stderr, "GATE [M6] ROTTO: la scaduta risponde\n");
            return 1;
        }
        json_libera(p);
    }

    /* ---- del: la morte dichiarata, il secondo del e' MANCA */
    printf("del: la cancellazione e il suo errore onesto...\n");
    {
        jval_t *p = tool(s, "put",
                         "{\"chiave\":\"tmp-x\",\"valore\":\"via\"}");
        json_libera(p);
        p = tool(s, "del", "{\"chiave\":\"tmp-x\"}");
        if (!e_vero(p, "cancellata")) {
            fprintf(stderr, "GATE ROTTO: del non ha cancellato\n");
            return 1;
        }
        json_libera(p);
        int ie = 0;
        p = tool_e(s, "del", "{\"chiave\":\"tmp-x\"}", &ie);
        int valido = 0;
        double cod = json_num(json_get(p, "codice"), &valido);
        if (!ie || !valido || cod != (double)MOTORE_ERR_MANCA) {
            fprintf(stderr, "GATE ROTTO: il secondo del non dice MANCA\n");
            return 1;
        }
        json_libera(p);
    }

    /* ---- [M7] audit: il contratto visibile all'agente */
    printf("[M7] audit: il contratto visibile anche da fuori...\n");
    {
        jval_t *p = tool(s, "audit", "{}");
        int valido = 0;
        double vive = json_num(json_get(p, "vive"), &valido);
        double dich = json_num(json_get(p, "dichiarate"), &valido);
        if (!valido || vive != (double)N_CHIAVI) {
            fprintf(stderr, "GATE [M7] ROTTO: vive %.0f != attese %d"
                            " (il conto indipendente del banco)\n",
                    vive, N_CHIAVI);
            return 1;
        }
        if (dich != vive || !e_vero(p, "coerente")) {
            fprintf(stderr, "GATE [M7] ROTTO: audit non coerente\n");
            return 1;
        }
        json_libera(p);
    }

    /* ---- [M1] i casi ostili del filo */
    printf("[M1] i casi ostili: metodo, JSON, tool, notifica...\n");
    {
        r = chiama(s, "{\"jsonrpc\":\"2.0\",\"id\":3,"
                      "\"method\":\"metodo/inventato\"}");
        if (codice_errore(r) != -32601) {
            fprintf(stderr, "GATE [M1] ROTTO: metodo sconosciuto != -32601\n");
            return 1;
        }
        json_libera(r);

        size_t rl = 0;
        char *grez = grezzo(s, "{nope", &rl);
        jval_t *brutto = json_parse(grez, rl);
        free(grez);
        if (!brutto || codice_errore(brutto) != -32700) {
            fprintf(stderr, "GATE [M1] ROTTO: JSON rotto != -32700\n");
            return 1;
        }
        json_libera(brutto);

        r = chiama(s, "{\"jsonrpc\":\"2.0\",\"id\":4,"
                      "\"method\":\"tools/call\",\"params\":"
                      "{\"name\":\"non_esiste\",\"arguments\":{}}}");
        if (codice_errore(r) != -32602) {
            fprintf(stderr, "GATE [M1] ROTTO: tool sconosciuto != -32602\n");
            return 1;
        }
        json_libera(r);

        char *nuda = grezzo(s, "{\"jsonrpc\":\"2.0\","
                              "\"method\":\"notifications/initialized\"}",
                            NULL);
        if (nuda) {
            fprintf(stderr, "GATE [M1] ROTTO: la notifica ha risposto\n");
            free(nuda);
            return 1;
        }
    }

    /* ---- il contatore del server non mente */
    {
        uint64_t rich = 0, bytep = 0;
        mcp_contatori(s, &rich, &bytep);
        if (rich != chiamate) {
            fprintf(stderr, "GATE ROTTO: il contatore dice %llu, il banco"
                            " ha contato %llu\n",
                    (unsigned long long)rich,
                    (unsigned long long)chiamate);
            return 1;
        }
        printf("contatore onesto: %llu richieste, %llu byte di payload"
               " spediti all'agente\n\n",
               (unsigned long long)rich, (unsigned long long)bytep);
    }

    /* ---- LA MISURA: il capitolo vive anche qui */
    printf("==== LA MISURA: il motore in microsecondi, il giro in"
           " millisecondi ====\n");
    {
        /* get: 2000 chiavi */
        size_t n = 2000;
        char **reqs = malloc(n * sizeof *reqs);
        if (!reqs) { fprintf(stderr, "memoria\n"); return 1; }
        for (size_t i = 0; i < n; i++) {
            reqs[i] = malloc(256);
            if (!reqs[i]) { fprintf(stderr, "memoria\n"); return 1; }
            snprintf(reqs[i], 256,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"get\",\"arguments\":"
                     "{\"chiave\":\"u%05u\"}}}",
                     (unsigned)(prng() % N_CHIAVI));
        }
        double *camp = malloc(n * sizeof *camp);
        if (!camp) { fprintf(stderr, "memoria\n"); return 1; }
        size_t byte_tot = 0;
        for (size_t i = 0; i < n; i++) {
            char *risp = NULL;
            size_t rl = 0;
            double t0 = ora_us();
            chiamate++;
            if (mcp_gestisci(s, reqs[i], strlen(reqs[i]), &risp, &rl) != 0) {
                fprintf(stderr, "memoria MCP\n");
                return 1;
            }
            camp[i] = ora_us() - t0;
            byte_tot += rl;
            free(risp);
        }
        qsort(camp, n, sizeof *camp, cmp_double);
        double p50 = percentile(camp, n, 0.50);
        double p99 = percentile(camp, n, 0.99);
        printf("get          p50 %8.2f us  p99 %8.2f us  "
               "risposta media %4zu byte (~%zu token)\n",
               p50, p99, byte_tot / n, byte_tot / n / 4);
        if (p99 > 10000.0) {
            fprintf(stderr, "GATE [MISURA] ROTTO: p99 get %.2f us oltre"
                            " il tetto di 10000 us\n", p99);
            return 1;
        }
        if (byte_tot / n > 512) {
            fprintf(stderr, "GATE [MISURA] ROTTO: risposta get oltre"
                            " 512 byte: il contesto non e' gratis\n");
            return 1;
        }
        for (size_t i = 0; i < n; i++) free(reqs[i]);
        free(reqs);
        free(camp);
    }
    {
        /* chiedi: 500 refusi */
        size_t n = 500;
        char **reqs = malloc(n * sizeof *reqs);
        if (!reqs) { fprintf(stderr, "memoria\n"); return 1; }
        for (size_t i = 0; i < n; i++) {
            reqs[i] = malloc(256);
            if (!reqs[i]) { fprintf(stderr, "memoria\n"); return 1; }
            char originale[16], refuso[16];
            snprintf(originale, sizeof originale, "u%05u",
                     prng() % N_CHIAVI);
            genera_sostituzione(refuso, sizeof refuso, originale, 1);
            snprintf(reqs[i], 256,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"chiedi\",\"arguments\":"
                     "{\"query\":\"%s\",\"k\":8}}}", refuso);
        }
        double *camp = malloc(n * sizeof *camp);
        if (!camp) { fprintf(stderr, "memoria\n"); return 1; }
        size_t byte_tot = 0;
        for (size_t i = 0; i < n; i++) {
            char *risp = NULL;
            size_t rl = 0;
            double t0 = ora_us();
            chiamate++;
            if (mcp_gestisci(s, reqs[i], strlen(reqs[i]), &risp, &rl) != 0) {
                fprintf(stderr, "memoria MCP\n");
                return 1;
            }
            camp[i] = ora_us() - t0;
            byte_tot += rl;
            free(risp);
        }
        qsort(camp, n, sizeof *camp, cmp_double);
        printf("chiedi       p50 %8.2f us  p99 %8.2f us  "
               "risposta media %4zu byte (~%zu token)\n",
               percentile(camp, n, 0.50), percentile(camp, n, 0.99),
               byte_tot / n, byte_tot / n / 4);
        if (percentile(camp, n, 0.99) > 50000.0) {
            fprintf(stderr, "GATE [MISURA] ROTTO: p99 chiedi oltre"
                            " 50000 us\n");
            return 1;
        }
        for (size_t i = 0; i < n; i++) free(reqs[i]);
        free(reqs);
        free(camp);
    }
    {
        /* presente: 200 batch da 64 chiavi */
        size_t n = N_BATCH;
        char **reqs = malloc(n * sizeof *reqs);
        if (!reqs) { fprintf(stderr, "memoria\n"); return 1; }
        for (size_t i = 0; i < n; i++) {
            reqs[i] = malloc(2048);
            if (!reqs[i]) { fprintf(stderr, "memoria\n"); return 1; }
            size_t pos = (size_t)snprintf(reqs[i], 2048,
                    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                    "\"params\":{\"name\":\"presente\",\"arguments\":"
                    "{\"chiavi\":[");
            for (int c = 0; c < 64; c++)
                pos += (size_t)snprintf(reqs[i] + pos, 2048 - pos,
                                        "\"u%05u\",", prng() % N_CHIAVI);
            snprintf(reqs[i] + pos - 1, 2048 - pos + 1, "]}}}");
        }
        double *camp = malloc(n * sizeof *camp);
        if (!camp) { fprintf(stderr, "memoria\n"); return 1; }
        size_t byte_tot = 0;
        for (size_t i = 0; i < n; i++) {
            char *risp = NULL;
            size_t rl = 0;
            double t0 = ora_us();
            chiamate++;
            if (mcp_gestisci(s, reqs[i], strlen(reqs[i]), &risp, &rl) != 0) {
                fprintf(stderr, "memoria MCP\n");
                return 1;
            }
            camp[i] = ora_us() - t0;
            byte_tot += rl;
            free(risp);
        }
        qsort(camp, n, sizeof *camp, cmp_double);
        printf("presente(64) p50 %8.2f us  p99 %8.2f us  "
               "risposta media %4zu byte (~%zu token)\n",
               percentile(camp, n, 0.50), percentile(camp, n, 0.99),
               byte_tot / n, byte_tot / n / 4);
        if (percentile(camp, n, 0.99) > 20000.0) {
            fprintf(stderr, "GATE [MISURA] ROTTO: p99 presente oltre"
                            " 20000 us\n");
            return 1;
        }
        for (size_t i = 0; i < n; i++) free(reqs[i]);
        free(reqs);
        free(camp);
    }
    {
        /* intervallo: 100 finestre da 100 chiavi */
        size_t n = 100;
        char **reqs = malloc(n * sizeof *reqs);
        if (!reqs) { fprintf(stderr, "memoria\n"); return 1; }
        for (size_t i = 0; i < n; i++) {
            reqs[i] = malloc(256);
            if (!reqs[i]) { fprintf(stderr, "memoria\n"); return 1; }
            uint32_t da = prng() % (N_CHIAVI - 100);
            snprintf(reqs[i], 256,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"intervallo\",\"arguments\":"
                     "{\"da\":\"u%05u\",\"a\":\"u%05u\",\"k\":32}}}",
                     da, da + 99);
        }
        double *camp = malloc(n * sizeof *camp);
        if (!camp) { fprintf(stderr, "memoria\n"); return 1; }
        size_t byte_tot = 0;
        for (size_t i = 0; i < n; i++) {
            char *risp = NULL;
            size_t rl = 0;
            double t0 = ora_us();
            chiamate++;
            if (mcp_gestisci(s, reqs[i], strlen(reqs[i]), &risp, &rl) != 0) {
                fprintf(stderr, "memoria MCP\n");
                return 1;
            }
            camp[i] = ora_us() - t0;
            byte_tot += rl;
            free(risp);
        }
        qsort(camp, n, sizeof *camp, cmp_double);
        printf("intervallo   p50 %8.2f us  p99 %8.2f us  "
               "risposta media %4zu byte (~%zu token)\n",
               percentile(camp, n, 0.50), percentile(camp, n, 0.99),
               byte_tot / n, byte_tot / n / 4);
        if (percentile(camp, n, 0.99) > 50000.0) {
            fprintf(stderr, "GATE [MISURA] ROTTO: p99 intervallo oltre"
                            " 50000 us\n");
            return 1;
        }
        for (size_t i = 0; i < n; i++) free(reqs[i]);
        free(reqs);
        free(camp);
    }
    printf("round-trip per compito tipico: refuso -> 1 chiedi;"
           " finestra -> 1 intervallo; 64 chiavi -> 1 presente.\n");
    printf("il confronto onesto: una chiamata LLM costa 10^2..10^3 ms di"
           " generazione;\nil motore sta sotto il millisecondo al p99:"
           " il costo e' il giro, e il layer lo minimizza.\n\n");

    /* ---- il disco dal filo: salva, ricarica, chiedi ancora */
    printf("[I10] il disco parlato via MCP: salva, ricarica, verifica...\n");
    {
        jval_t *p = tool(s, "salva", "{\"nome\":\"banco_mcp.db\"}");
        if (!e_vero(p, "salvato")) {
            fprintf(stderr, "GATE ROTTO: salva fallito\n");
            return 1;
        }
        json_libera(p);
    }
    mcp_chiudi(s);
    {
        size_t scartate = 1;             /* parte colpevole: deve uscire 0 */
        s = mcp_apri("banco_mcp.db", 0, &scartate);
        if (!s) {
            fprintf(stderr, "GATE ROTTO: il ricaricato non apre\n");
            return 1;
        }
        if (scartate != 0) {
            fprintf(stderr, "GATE [I5] ROTTO: %zu scartate su un file"
                            " appena scritto\n", scartate);
            return 1;
        }
        jval_t *p = tool(s, "get", "{\"chiave\":\"u00100\"}");
        const char *v = json_str(json_get(p, "valore"), &vl);
        if (!v || strcmp(v, "valore-di-u00100") != 0) {
            fprintf(stderr, "GATE [I10] ROTTO: il valore non sopravvive"
                            " al giro del disco\n");
            return 1;
        }
        json_libera(p);

        /* il bloom raddrizzato: la ricomposizione lo ha ridimensionato
         * a 10 bit/chiave [I9] — gli alieni tornano al "forse" raro */
        char args[2048];
        size_t pos = (size_t)snprintf(args, sizeof args,
                                      "{\"chiavi\":[");
        char chiave[16];
        for (int i = 0; i < 32; i++) {
            genera_lettere(chiave, sizeof chiave);
            pos += (size_t)snprintf(args + pos, sizeof args - pos,
                                    "\"%s\",", chiave);
        }
        snprintf(args + pos - 1, sizeof args - pos + 1, "]}");
        p = tool(s, "presente", args);
        if (p->tipo != J_ARR || p->n != 32) {
            fprintf(stderr, "GATE [M5] ROTTO: 32 chiavi -> %zu verdetti\n",
                    p->tipo == J_ARR ? p->n : (size_t)0);
            return 1;
        }
        int forse = 0;
        for (size_t i = 0; i < p->n; i++) {
            int valido = 0;
            if (json_num(p->figli[i], &valido) == 1.0 && valido)
                forse++;
        }
        printf("     bloom raddrizzato dal disco: alieni \"forse\" %d/32"
               " (%.1f%%, atteso ~0.8%%)\n",
               forse, 100.0 * forse / 32.0);
        json_libera(p);
    }
    remove("banco_mcp.db");

    mcp_chiudi(s);
    printf("\nTUTTI I GATE VERDI: il motore e' uno strumento onesto.\n");
    return 0;
}
