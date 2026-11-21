/*
 * demo_gateway.c — il banco della lavagna: piu' agenti, un'arena sola.
 *
 * Non e' una demo: ogni regola del contratto [L1]..[L6] ha il suo
 * gate con soglia, e un gate che salta ferma tutto con EXIT!=0. Il
 * banco apre IL GATEWAY in process (mcp_apri + gateway_apri) e parla
 * con socket VERI dal lato cliente: la lavagna attraversa il filo
 * intero, kernel compreso.
 *
 *   GATE [L1] LA LAVAGNA E' VIVA: il cliente A scrive una voce, il
 *              cliente B — un ALTRO socket, senza salva in mezzo —
 *              la legge. E' tutto il capitolo: la condivisione non e'
 *              una promessa del disco, e' l'arena che respira.
 *   GATE [L3] IL FILO E' QUELLO DEL CAP 11: la risposta attraversa
 *              l'envelope MCP-JSON e dentro c'e' il payload atteso,
 *              confidenza compresa [M3].
 *   GATE [L4] NESSUNO PARLA NEL TEMPO DEGLI ALTRI: tre clienti a
 *              righe SPEZZATE gli uni contro gli altri, ognuno
 *              riceve la SUA risposta; e i byte del cliente che non
 *              legge aspettano LUI (nel kernel o, oltre il buffer
 *              del kernel, nella coda del custode: i due mondi sono
 *              lo stesso contratto, il secondo non si provoca da
 *              banco perche' dipende dai buffer del sistema, e una
 *              soglia che dipende dal sysctl non e' una soglia).
 *   GATE [W1] IL WAL E' DEL CUSTODE: due clienti scrivono, il
 *              custode muore senza salva, il replay li ridara'
 *              tutti e due — le put di TUTTI si inchiostrano nello
 *              stesso segmento.
 *   GATE [L2][I11] IL CUSTODE E' UNO: finche' il gateway vive, il
 *              secondo che apre il db cade ad alta voce.
 *   GATE [MISURA] il prezzo della porta: il filo in processo contro
 *              il filo attraverso il kernel, p50 e p99 numerati.
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
#include "gateway.h"
#include "json.h"

#define DB "banco_g.db"

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

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* ------------------------------------------------------- il filo cliente */

/* una richiesta al custode: il '\n' del filo lo porta QUA' ([M1] un
 * oggetto per riga) E il girevole gira FINCHE' la risposta esce o la
 * pazienza finisce (200 x 2 ms): il kernel non e' sincrono con il
 * banco, e i giras a tempo-zero sono ciechi ai byte in ritardo di
 * Nagle (il post mortem del capitolo: le sentinelle cercavano un
 * newline che dormiva nello stomaco del mittente). */
static jval_t *chiedi_al_custode(gateway_t *g, gateway_cliente_t *c,
                                 const char *req)
{
    if (gateway_cliente_scrivi(c, req, strlen(req)) != 0 ||
        gateway_cliente_scrivi(c, "\n", 1) != 0)
        return NULL;
    for (int attesa = 0; attesa < 200; attesa++)
        if (gateway_gira(g, 2) > 0)
            break;
    char riga[16384];
    if (gateway_cliente_leggi(c, riga, sizeof riga) != 1)
        return NULL;
    return json_parse(riga, strlen(riga));
}

/* scrive e gira SENZA leggere (il cliente che non legge): la stessa
 * pazienza, perche' anche chi non legge aspettera' la sua coda */
static void di_al_custode(gateway_t *g, gateway_cliente_t *c,
                          const char *req)
{
    gateway_cliente_scrivi(c, req, strlen(req));
    gateway_cliente_scrivi(c, "\n", 1);
    for (int attesa = 0; attesa < 200; attesa++)
        if (gateway_gira(g, 2) > 0)
            break;
}

/* il payload dentro l'envelope MCP [L3] — il filo del cap. 11 */
static jval_t *payload_di(jval_t *risp)
{
    const jval_t *content = json_get(json_get(risp, "result"), "content");
    if (!content || content->tipo != J_ARR || content->n == 0)
        return NULL;
    size_t tl = 0;
    const char *testo = json_str(json_get(content->figli[0], "text"), &tl);
    if (!testo)
        return NULL;
    return json_parse(testo, tl);
}

static const char GET_LAVAGNA[] =
    "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"tools/call\","
    "\"params\":{\"name\":\"get\",\"arguments\":"
    "{\"chiave\":\"lavagna/viva\"}}}";

int main(void)
{
    printf("==== CAPITOLO 14: IL GATEWAY — la lavagna a un solo"
           " custode ====\n\n");

    /* il terreno: una sessione morta pianta una voce e salva, cosi'
     * il db esiste in forma PDB1 [I10] */
    {
        mcp_t *seme = mcp_apri(NULL, 1, NULL);
        if (!seme) {
            fprintf(stderr, "memoria\n");
            return 1;
        }
        char req[512];
        char *r = NULL;
        size_t rl = 0;
        snprintf(req, sizeof req, "{\"jsonrpc\":\"2.0\",\"id\":1,"
                 "\"method\":\"tools/call\",\"params\":{\"name\":"
                 "\"put\",\"arguments\":{\"chiave\":\"lavagna/seme\","
                 "\"valore\":\"la prima voce che nessuno ha ancora\"}}}");
        if (mcp_gestisci(seme, req, strlen(req), &r, &rl) != 0 || !r) {
            fprintf(stderr, "GATE ROTTO: il seme non scrive\n");
            return 1;
        }
        free(r);
        snprintf(req, sizeof req, "{\"jsonrpc\":\"2.0\",\"id\":2,"
                 "\"method\":\"tools/call\",\"params\":{\"name\":"
                 "\"salva\",\"arguments\":{\"nome\":\"" DB "\"}}}");
        r = NULL;
        if (mcp_gestisci(seme, req, strlen(req), &r, &rl) != 0 || !r) {
            fprintf(stderr, "GATE ROTTO: il seme non salva\n");
            return 1;
        }
        free(r);
        mcp_chiudi(seme);
    }

    mcp_t *mo = mcp_apri(DB, 1, NULL);       /* IL custode [L2][I11]   */
    if (!mo) {
        fprintf(stderr, "GATE ROTTO: il custode non nasce\n");
        return 1;
    }
    uint32_t porta = 0;
    gateway_t *g = gateway_apri(mo, &porta);
    if (!g) {
        fprintf(stderr, "GATE ROTTO: la porta non si apre\n");
        mcp_chiudi(mo);
        return 1;
    }
    printf("la porta: 127.0.0.1:%u — un solo custode, un motore solo"
           " [L2][L6]\n", porta);

    gateway_cliente_t A, B, C;
    if (gateway_cliente_apri(&A, porta) != 0 ||
        gateway_cliente_apri(&B, porta) != 0 ||
        gateway_cliente_apri(&C, porta) != 0) {
        fprintf(stderr, "GATE ROTTO: i clienti non entrano\n");
        return 1;
    }
    /* bussare non e' entrare: chi accoglie e' il GIRO del custode
     * (l'accettazione vive li', non nella connect) */
    for (int i = 0; i < 5; i++)
        gateway_gira(g, 2);
    if (gateway_clienti(g) != 3) {
        fprintf(stderr, "GATE ROTTO: tre dentro, %zu contati\n",
                gateway_clienti(g));
        return 1;
    }

    /* GATE [L1]: la lavagna e' viva — A scrive, B (un altro socket,
     * senza salva) legge. E' il cuore del capitolo. */
    printf("[L1] la lavagna e' viva: A scrive, B legge senza salva...\n");
    {
        jval_t *ra = chiedi_al_custode(g, &A,
                 "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"put\",\"arguments\":"
                 "{\"chiave\":\"lavagna/viva\","
                 "\"valore\":\"scritta-da-A\"}}}");
        if (!ra) {
            fprintf(stderr, "GATE [L1] ROTTO: la put di A non risponde\n");
            return 1;
        }
        json_libera(ra);
        jval_t *rb = chiedi_al_custode(g, &B, GET_LAVAGNA);
        if (!rb) {
            fprintf(stderr, "GATE [L1] ROTTO: la get di B non risponde\n");
            return 1;
        }
        jval_t *p = payload_di(rb);
        size_t vl = 0;
        const char *v = p ? json_str(json_get(p, "valore"), &vl) : NULL;
        if (!v || strcmp(v, "scritta-da-A") != 0) {
            fprintf(stderr, "GATE [L1] ROTTO: B non vede cio' che A ha"
                    " scritto (valore: %s)\n", v ? v : "null");
            return 1;
        }
        double conf = json_num(json_get(p, "confidenza"), NULL);
        if (conf != 1.0) {
            fprintf(stderr, "GATE [L3] ROTTO: confidenza %.3f != 1.0"
                    " [M3]\n", conf);
            return 1;
        }
        printf("     B ha letto \"%s\" dall'arena di A: due socket,"
               " un'arena sola\n", v);
        json_libera(p);
        json_libera(rb);
    }

    /* GATE [L4]: tre clienti, righe spezzate e mescolate, e un
     * cliente con risposte non lette che non ferma nessuno */
    printf("[L4] nessuno parla nel tempo degli altri: righe spezzate"
           " e mescolate...\n");
    {
        /* le due PING di B non le legge nessuno: aspettano LUI */
        di_al_custode(g, &B,
                 "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"ping\"}");
        di_al_custode(g, &B,
                 "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"ping\"}");
        /* la riga di A arriva a META': il custode non risponde a un
         * pezzo di riga, aspetta l'intera [M1] */
        const char *riga_a =
            "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"ping\"}\n";
        size_t intera = strlen(riga_a);
        size_t spezzo = intera - 8;
        gateway_cliente_scrivi(&A, riga_a, spezzo);
        gateway_gira(g, 2);               /* il giro vede il moncone  */
        /* arriva la coda della riga di A e la riga intera di C */
        gateway_cliente_scrivi(&A, riga_a + spezzo, intera - spezzo);
        gateway_gira(g, 2);
        gateway_gira(g, 2);

        char riga[1024];
        if (gateway_cliente_leggi(&A, riga, sizeof riga) != 1) {
            fprintf(stderr, "GATE [L4] ROTTO: A resta senza risposta"
                    " (la riga spezzata si e' persa)\n");
            return 1;
        }
        jval_t *ja = json_parse(riga, strlen(riga));
        if (!ja || json_num(json_get(ja, "id"), NULL) != 20.0) {
            fprintf(stderr, "GATE [L3] ROTTO: la risposta di A non e'"
                    " la sua (id atteso 20)\n");
            return 1;
        }
        json_libera(ja);
        /* A e la prima risposta di B navigano insieme: la riga di C
         * non seguiva, e deve essere la PROSSIMA cosa che C vede */
        jval_t *jc = chiedi_al_custode(g, &C,
                 "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"ping\"}");
        if (!jc || json_num(json_get(jc, "id"), NULL) != 23.0) {
            fprintf(stderr, "GATE [L4] ROTTO: C non riceve la sua"
                    " risposta mentre B non legge le sue\n");
            json_libera(jc);
            return 1;
        }
        json_libera(jc);
        /* la coda di B e' ancora tutta sua: due risposte nell'ordine */
        for (int i = 0; i < 2; i++) {
            if (gateway_cliente_leggi(&B, riga, sizeof riga) != 1) {
                fprintf(stderr, "GATE [L4] ROTTO: la coda di B si e'"
                        " persa (%d)\n", i);
                return 1;
            }
            jval_t *jb = json_parse(riga, strlen(riga));
            double id = json_num(json_get(jb, "id"), NULL);
            json_libera(jb);
            if (id != (i == 0 ? 21.0 : 22.0)) {
                fprintf(stderr, "GATE [L3] ROTTO: la coda di B ha"
                        " l'ordine sbagliato (%.0f)\n", id);
                return 1;
            }
        }
        printf("     servita la riga di A spezzata in due, C mentre B"
               " non leggeva, e la coda di B nell'ordine giusto\n");
    }

    /* GATE [W1]: le put di DUE clienti nello stesso segmento, il
     * custode muore senza salva, il replay le ridara' tutte e due */
    printf("[W1] il WAL e' del custode: due clienti scrivono, il"
           " custode muore senza salva...\n");
    {
        jval_t *r = chiedi_al_custode(g, &A,
                 "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"put\",\"arguments\":"
                 "{\"chiave\":\"lavagna/di-a\",\"valore\":\"viva\"}}}");
        if (!r)
            return 1;
        json_libera(r);
        jval_t *rb = chiedi_al_custode(g, &B,
                 "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"put\",\"arguments\":"
                 "{\"chiave\":\"lavagna/di-b\",\"valore\":\"viva\"}}}");
        if (!rb)
            return 1;
        json_libera(rb);

        gateway_cliente_chiudi(&A);
        gateway_cliente_chiudi(&B);
        gateway_cliente_chiudi(&C);
        gateway_chiudi(g);         /* il custode muore SENZA salva    */
        mcp_chiudi(mo);

        mcp_t *dopo = mcp_apri(DB, 1, NULL);     /* il replay [W3]     */
        if (!dopo) {
            fprintf(stderr, "GATE [W1] ROTTO: il ricaricato non apre\n");
            return 1;
        }
        static const char *attese[2] = {
            "lavagna/di-a", "lavagna/di-b"
        };
        for (int i = 0; i < 2; i++) {
            char req[512];
            snprintf(req, sizeof req, "{\"jsonrpc\":\"2.0\",\"id\":40,"
                     "\"method\":\"tools/call\",\"params\":{\"name\":"
                     "\"get\",\"arguments\":{\"chiave\":\"%s\"}}}",
                     attese[i]);
            char *ris = NULL;
            size_t rl = 0;
            if (mcp_gestisci(dopo, req, strlen(req), &ris, &rl) != 0 ||
                    !ris) {
                fprintf(stderr, "memoria\n");
                return 1;
            }
            if (!strstr(ris, "viva")) {
                fprintf(stderr, "GATE [W1] ROTTO: %s non ha"
                        " sopravvissuto al crash del custode\n", attese[i]);
                return 1;
            }
            free(ris);
        }
        printf("     le voci di A e B vive dopo il crash: un WAL solo,"
               " tutte le penne [L1][W1]\n");

        /* GATE [L2][I11]: questo E' il custode ora; il secondo che
         * apre lo stesso db deve cadere ad alta voce */
        gateway_t *g2 = gateway_apri(dopo, &porta);
        if (!g2) {
            fprintf(stderr, "memoria\n");
            return 1;
        }
        mcp_t *intruso = mcp_apri(DB, 1, NULL);
        if (intruso) {
            fprintf(stderr, "GATE [L2] ROTTO: un secondo custode ha"
                    " aperto lo stesso db\n");
            mcp_chiudi(intruso);
            return 1;
        }
        printf("     [L2] il secondo padrone e' caduto ad alta voce,"
               " come da contratto [I11]\n");

        /* ---- GATE [MISURA]: il prezzo della porta ---- */
        printf("[MISURA] il prezzo della porta: il filo in processo"
               " contro il filo attraverso il kernel...\n");
        gateway_cliente_t M1, M2;
        if (gateway_cliente_apri(&M1, porta) != 0 ||
            gateway_cliente_apri(&M2, porta) != 0) {
            fprintf(stderr, "GATE ROTTO: i clienti della misura non"
                    " entrano\n");
            return 1;
        }
        const size_t n_mis = 400;
        double *attraverso = malloc(n_mis * sizeof *attraverso);
        if (!attraverso) {
            fprintf(stderr, "memoria\n");
            return 1;
        }
        char *ris = NULL;
        size_t rl = 0;
        for (size_t i = 0; i < n_mis; i++) {
            double t0 = ora_us();
            jval_t *rm = chiedi_al_custode(g2, &M1, GET_LAVAGNA);
            attraverso[i] = ora_us() - t0;
            if (!rm) {
                fprintf(stderr, "GATE [MISURA] ROTTO: la misura non"
                        " risponde\n");
                return 1;
            }
            json_libera(rm);
            /* il JOINT: tra una misura e l'altra, l'altro cliente
             * bussa — il custode NON leva la mano dal girevole */
            di_al_custode(g2, &M2,
                          "{\"jsonrpc\":\"2.0\",\"id\":60,\"method\":"
                          "\"ping\"}");
        }
        double dentro_tot = 0.0;
        for (size_t i = 0; i < n_mis; i++) {
            double t0 = ora_us();
            if (mcp_gestisci(dopo, GET_LAVAGNA, strlen(GET_LAVAGNA),
                             &ris, &rl) != 0 || !ris) {
                fprintf(stderr, "memoria\n");
                return 1;
            }
            dentro_tot += ora_us() - t0;
            free(ris);
            ris = NULL;
        }
        double dentro_us = dentro_tot / (double)n_mis;
        /* il resto della coda di M2 (i ping del JOINT) muore con la
         * connessione: sono byte buttati per contratto — se il
         * cliente non li ha letti, erano suoi */
        qsort(attraverso, n_mis, sizeof *attraverso, cmp_double);
        double p50 = attraverso[(n_mis - 1) / 2];
        double p99 = attraverso[n_mis - 2];
        printf("     get in processo: %7.2f us; attraverso la porta:"
                " p50 %7.2f us, p99 %7.2f us (il kernel sta nel"
                " mezzo, e il girevole lo serve senza sdegno)\n",
               dentro_us, p50, p99);
        if (p99 > 20000.0) {
            fprintf(stderr, "GATE [MISURA] ROTTO: p99 attraverso la"
                    " porta oltre 20000 us\n");
            return 1;
        }
        if (p50 < dentro_us) {
            fprintf(stderr, "GATE [MISURA] ROTTO: la porta e' PIU'"
                    " veloce del filo in-processo: il banco misura"
                    " una bugia (%.2f < %.2f)\n", p50, dentro_us);
            return 1;
        }
        gateway_cliente_chiudi(&M1);
        gateway_cliente_chiudi(&M2);
        gateway_chiudi(g2);
        mcp_chiudi(dopo);
        free(attraverso);
    }

    remove(DB);
    remove(DB ".wal");
    remove(DB ".lock");
    printf("\nTUTTI I GATE VERDI: la lavagna ha una porta, un custode,"
           " e N clienti che si parlano addosso.\n");
    return 0;
}
