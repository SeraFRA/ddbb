/*
 * demo_multiagente.c — il banco multi-sessione: la gobba del cold
 * start, MISURATA prima del gateway.
 *
 * =====================================================================
 * PERCHE' QUESTO BANCO (la domanda che decide un mese di lavoro)
 * =====================================================================
 * La tesi in campo (il verdetto del prefetcher sul progetto reale):
 * il prefetcher e' un ottimizzatore del COLD START — nell'apertura di
 * una sessione vale giri interi, a contesto saturo vale zero e costa
 * handle. Il guadagno quindi dovrebbe avere una GOBBA: massima nelle
 * sessioni corte dense di look-up che muoiono prima che il dedup
 * spenga il segnale — cioe' il profilo di un sub-agente. Se la gobba
 * c'e', la destinazione blackboard (gateway a server unico, stile
 * Redis) ha un numero che la giustifica; se non c'e', ci si e'
 * risparmiati un mese di socket. Il banco si paga ORA: misurato
 * prima, deciso dopo.
 *
 * =====================================================================
 * IL CONTRATTO [B1]..[B6] (invariants-first: scritto prima del codice)
 * =====================================================================
 *  [B1] IL DB E' DEI VIVI (UNO PER VOLTA), LA MEMORIA E' DEI MORTI.
 *       Le sessioni sono SEQUENZIALI sullo stesso pool ("banco_m.db"):
 *       nasce, cerca, muore; la prossima riapre i resti. Modella il
 *       mondo di OGGI (una chat chiude, la prossima ritrova il db) e
 *       rispetta [I11]: mai due padroni vivi. I processi paralleli
 *       sono la destinazione (il gateway), non il banco.
 *  [B2] IL CASO E' SINTETICO, I COSTI SONO DEL FILO. Il reasoning e'
 *       testo sintetico col vocabolario dei fatti (le chiavi MAI
 *       verbatim: [P4] deve trovare lavoro, non la porta sbarrata),
 *       ma OGNI costo e' misurato dalle risposte vere dell'MCP
 *       (mcp_contatori) e dagli handle costruiti DENTRO il banco
 *       esattamente come li costruisce l'hook. Due libri dichiarati:
 *       IL LIBRO DEL FILO (byte di risposta + byte di handle: lui e'
 *       il verdetto) e il libro del parcheggio (la formula [P2] di
 *       costo_scoperta da prefetch_stat: la CONFERMA di coerenza, non
 *       il verdetto).
 *  [B3] DUE REGIMI, STESSO COMPITO. ON: il produttore (in-process,
 *       le stesse funzioni dell'hook) predice e parcheggia; l'agente
 *       tira il fatto con un get se l'handle l'ha proposto, altrimenti
 *       paga la scoperta STRETTA (un intervallo del tema) e poi get.
 *       OFF: niente produttore; la scoperta e' la lista LARGA (un
 *       intervallo dell'intero namespace, UNA volta per sessione: e'
 *       la base gentile — un agente accorto legge la lista una volta
 *       sola, un agente maldestro pagherebbe peggio). Ogni regime
 *       parte col parcheggio VUOTO e lo STAGING ASSENTE: il file
 *       orfano del regime prima inquina quello dopo, e va rimosso
 *       tra i regimi ([P3]: un derivato muore col suo mondo).
 *  [B4] IL CONTO DEL BANCO E' IL CONTO DEL FILO. Il banco tiene un
 *       modello di cio' che parcheggia e di cio' che colpisce; se il
 *       modello diverge da prefetch_stat (hit, spesa_handle), il banco
 *       sta contando con la testa: GATE, non opzione.
 *  [B5] IL COMPORTAMENTO E' DA SUB-AGENTE. Per turno: il produttore
 *       guarda la traccia, l'agente cerca fino a 2 fatti del compito,
 *       il reasoning NOMINA IL TEMA (mai la chiave); i turni senza
 *       fatti sono turni di scrittura — il produttore gira lo stesso:
 *       e' li' che la speculazione paga senza colpire. L'handle entra
 *       nella traccia del turno dopo: il dedup [P4] quel se' lo deve
 *       trovare, come nella sessione vera.
 *  [B6] IL GATE PUO' SMENTIRE. Le soglie sono numerate PRIMA del run;
 *       se i numeri contraddicono la tesi, il banco torna rosso e la
 *       gobba non c'e'. Il margine delle lunghe NON e' un gate di
 *       direzione (troppo fragile su un caso sintetico): e' MISURA
 *       dichiarata, e si legge accanto a quello delle brevi.
 *
 * =====================================================================
 * I GATE (soglie numerate, EXIT != 0 se uno cade)
 * =====================================================================
 *  GATE [B1] BREVI: il regime ON costa almeno il 15% in meno del
 *          regime OFF sul libro del filo — sotto, la speculazione non
 *          ripaga il suo ingombro proprio dove dovrebbe.
 *  GATE [B2] NON INQUINA: nelle sessioni lunghe ON non costa MAI piu'
 *          di OFF — la speculazione che peggiora va spenta [P8].
 *  GATE [B3] IL CONTO: per OGNI sessione ON il modello del banco
 *          (hit, spesa) e' IDENTICO a prefetch_stat.
 *
 * =====================================================================
 * I LIMITI (dichiarati in testa, come sempre)
 * =====================================================================
 *  - il caso e' sintetico: un reasoning che nomina il tema in modo
 *    pulito e' PIU' gentile del mondo vero (il verdetto di campo resta
 *    quello del progetto reale: colpo pieno sulle riprese a distanza,
 *    0.10 sul lavoro a contesto saturo). Questo banco misura la GOBBA,
 *    non il rumore del mondo;
 *  - l'OFF e' la base gentile: la lista larga si legge UNA volta per
 *    sessione, come farebbe un agente accorto;
 *  - il costo di scoperta del libro del parcheggio [P2] e' formula,
 *    non filo: i suoi numeri sono la conferma, non il verdetto.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "motore.h"
#include "prefetch.h"
#include "mcp_server.h"
#include "json.h"

#define DB              "banco_m.db"
#define STAGING         DB ".prefetch"
#define N_TEMI          12
#define FATTI           4       /* 48 voci: la lista larga sta nel cap */
#define BREVI           8
#define LUNGHE          4
#define TURNI_BREVI     3       /* 2 di ricerca + 1 di scrittura      */
#define TURNI_LUNGHE    8       /* 4 di ricerca (2 temi) + 4 scrittura */
#define ASK_PER_TURNO   2
#define SOGLIA_BREVI    0.15     /* GATE [B1]                          */

/* ------------------------------------------------------- il filo del banco
 * le stesse utility del banco del capitolo 11: niente corto-incircuiti,
 * ogni risposta attraversa envelope e content come da client vero */

static jval_t *chiama(mcp_t *s, const char *req)
{
    char *r = NULL;
    size_t rl = 0;
    if (mcp_gestisci(s, req, strlen(req), &r, &rl) != 0 || !r) {
        fprintf(stderr, "GATE ROTTO: nessuna risposta a: %.80s\n", req);
        exit(1);
    }
    jval_t *v = json_parse(r, rl);
    if (!v) {
        fprintf(stderr, "GATE ROTTO: risposta non-JSON: %.80s\n", r);
        exit(1);
    }
    free(r);
    return v;
}

static jval_t *payload_di(jval_t *risp)
{
    const jval_t *content = json_get(json_get(risp, "result"), "content");
    size_t tl = 0;
    if (!content || content->tipo != J_ARR || content->n == 0) {
        fprintf(stderr, "GATE ROTTO: envelope senza content\n");
        exit(1);
    }
    const char *testo = json_str(json_get(content->figli[0], "text"), &tl);
    if (!testo) {
        fprintf(stderr, "GATE ROTTO: content senza text\n");
        exit(1);
    }
    jval_t *p = json_parse(testo, tl);
    if (!p) {
        fprintf(stderr, "GATE ROTTO: payload non-JSON: %.60s\n", testo);
        exit(1);
    }
    return p;
}

static jval_t *tool(mcp_t *s, const char *nome, const char *args)
{
    char req[2048];
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
             nome, args);
    jval_t *risp = chiama(s, req);
    const jval_t *ie = json_get(json_get(risp, "result"), "isError");
    if (ie && ie->tipo == J_BOOL && ie->booleano) {
        fprintf(stderr, "GATE ROTTO: %s ha risposto errore\n", nome);
        exit(1);
    }
    jval_t *p = payload_di(risp);
    json_libera(risp);
    return p;
}

/* ------------------------------------------------------------ il fixture
 * la coda generica ("il contratto della memoria si misura...") e'
 * condivisa da TUTTI i fatti: e' il canale di rumore cross-tema,
 * quello che nella sessione vera sporca le predizioni lontane [B6] */

static const struct { const char *w1, *w2; } TEMI[N_TEMI] = {
    { "grammi",      "posizionali" },
    { "orologio",    "fermo" },
    { "wal",         "trittico" },
    { "lucchetto",   "padrone" },
    { "prefetch",    "speculativo" },
    { "arena",       "ordinata" },
    { "recinto",     "campioni" },
    { "bloom",       "forse" },
    { "anello",      "timing" },
    { "crash",       "replay" },
    { "salva",       "atomico" },
    { "staging",     "derivato" },
};

static void chiave_fatto(char *out, size_t cap, int tema, int f)
{
    snprintf(out, cap, "nota/%s-%s/f%d", TEMI[tema].w1, TEMI[tema].w2, f);
}

static void valore_fatto(char *out, size_t cap, int tema, int f)
{
    snprintf(out, cap,
             "il fatto %d: %s %s nel motore — il contratto della"
             " memoria si misura, il forse non e' un si' e il dato si"
             " dichiara anche quando smentisce",
             f, TEMI[tema].w1, TEMI[tema].w2);
}

static void prompt_tema(char *out, size_t cap, int tema)
{
    snprintf(out, cap,
             "il compito di oggi: rivedere %s %s del motore, ogni"
             " fatto conta percio' la misura del contratto della"
             " memoria va letta per intero",
             TEMI[tema].w1, TEMI[tema].w2);
}

/* id tema/fatto di una chiave del pool, o -1: il banco riconosce solo
 * cio' che ha seminato lui [B4 e']: niente sguardo dentro il server */
static void chiave_dove(const char *chiave, int *tema, int *fatto)
{
    *tema = -1;
    *fatto = -1;
    char prova[256];
    for (int t = 0; t < N_TEMI; t++)
        for (int f = 0; f < FATTI; f++) {
            chiave_fatto(prova, sizeof prova, t, f);
            if (strcmp(prova, chiave) == 0) {
                *tema = t;
                *fatto = f;
                return;
            }
        }
}

static void traccia_aggiungi(char *traccia, size_t *tlen, const char *piu,
                            size_t plen)
{
    if (*tlen + plen + 2 >= PREFETCH_TRACCIA_MAX)
        return;                /* la coda recente resta la piu' viva [P1] */
    if (*tlen) {
        traccia[(*tlen)++] = '\n';
        traccia[*tlen] = '\0';
    }
    memcpy(traccia + *tlen, piu, plen);
    *tlen += plen;
    traccia[*tlen] = '\0';
}

/* ------------------------------------------------- una sessione [B1][B5] */

static uint64_t esegui_sessione(int on, int brevi, int s_idx,
                                int temi[2], uint64_t *hit_out,
                                uint64_t *spesa_out)
{
    /* lo staging orfano della sessione prima NON si eredita: la prima
     * richiesta di questa lo consumerebbe e il conto del banco
     * sballerebbe [B3][B4] — muore col suo mondo, come il regime */
    remove(STAGING);
    mcp_t *m = mcp_apri(DB, 1, NULL);      /* un padrone per volta [B1] */
    if (!m) {
        fprintf(stderr, "GATE ROTTO: il pool non apre [B1]\n");
        exit(1);
    }
    uint64_t r0 = 0, b0 = 0;
    mcp_contatori(m, &r0, &b0);

    char traccia[PREFETCH_TRACCIA_MAX];
    size_t tlen = 0;
    char prompt[512];
    int n_temi = (temi[0] != temi[1]) ? 2 : 1;
    for (int t = 0; t < n_temi; t++) {
        prompt_tema(prompt, sizeof prompt, temi[t]);
        traccia_aggiungi(traccia, &tlen, prompt, strlen(prompt));
    }

    int parcheggiata[N_TEMI][FATTI];
    memset(parcheggiata, 0, sizeof parcheggiata);
    int prossimo[N_TEMI];
    for (int t = 0; t < N_TEMI; t++)
        prossimo[t] = 0;

    /* la semantica del consumatore, specchiata nel modello [B4]: lo
     * staging scritto non costa NIENTE finche' il filo non lo legge
     * (prima richiesta dopo la scrittura). La generazione di un turno
     * di sola scrittura finisce sovrascritta dalla prossima e il filo
     * NON la conteggia: pero' il banco la scarta uguale, o il gate
     * contrebbe la testa */
    char in_attesa[PREFETCH_PREDIZIONI_MAX][PREFETCH_CHIAVE_MAX + 8];
    size_t in_attesa_n = 0;
    uint64_t byte_in_attesa = 0;

    uint64_t hit_s = 0, spesa_s = 0;
    char staging[512];
    prefetch_percorso_staging(DB, staging, sizeof staging);
    int lista_larga_fatta = 0;            /* l'OFF accorto: una volta [B3] */
    int turni = brevi ? TURNI_BREVI : TURNI_LUNGHE;

    for (int tt = 0; tt < turni; tt++) {
        /* il produttore: le stesse chiamate dell'hook, in-process.
         * Scrive lo staging PRIMA delle domande: la spesa entra solo
         * quando il filo la consuma [P3][B2] */
        if (on) {
            motore_t *snap = motore_apri_da(DB, 1, NULL);  /* [P9] */
            if (snap) {
                prefetch_predizione_t pred[PREFETCH_PREDIZIONI_MAX];
                size_t n = prefetch_predici(snap, traccia, tlen, pred,
                                            PREFETCH_PREDIZIONI_MAX);
                motore_chiudi(snap);
                if (n > 0) {
                    jbuf_t h;
                    jbuf_init(&h);
                    JBUF_LIT(&h, "memoria prefetch (FORSE, similarita'"
                             " sintattica): ");
                    for (size_t i = 0; i < n; i++) {
                        if (i)
                            JBUF_LIT(&h, ", ");
                        jbuf_add(&h, pred[i].chiave, pred[i].chiave_len);
                        JBUF_LIT(&h, " ");
                        jbuf_num(&h, pred[i].confidenza);
                    }
                    JBUF_LIT(&h, " — get <chiave> per il valore");
                    char gen[32];
                    snprintf(gen, sizeof gen, "%c%dt%d",
                             brevi ? 'b' : 'l', s_idx, tt);
                    if (!h.rotto && prefetch_scrivi_staging(staging, gen,
                                                            pred, n,
                                                            h.n) == 0) {
                        /* la generazione nuova CANCELLA quella in
                         * attesa: il filo non l'ha mai letta [P3] */
                        in_attesa_n = 0;
                        byte_in_attesa = 0;
                        for (size_t i = 0; i < n && i < PREFETCH_PREDIZIONI_MAX; i++) {
                            /* copia per LUNGHEZZA: la chiave del
                             * parcheggio non e' garantita NUL-terminata
                             * [P2], leggere con %s e' camminare oltre */
                            size_t kl = pred[i].chiave_len;
                            if (kl > sizeof in_attesa[i] - 1)
                                kl = sizeof in_attesa[i] - 1;
                            memcpy(in_attesa[i], pred[i].chiave, kl);
                            in_attesa[i][kl] = '\0';
                            in_attesa_n = i + 1;
                        }
                        byte_in_attesa = h.n;
                        /* l'handle ENTRA nel contesto [P4][B5]: il
                         * dedup lo trovera' al prossimo giro */
                        traccia_aggiungi(traccia, &tlen, h.p, h.n);
                    }
                    jbuf_libera(&h);
                }
            }
        }

        /* le domande del turno: fino a ASK_PER_TURNO fatti [B5] */
        for (int a = 0; a < ASK_PER_TURNO; a++) {
            int tem = -1;
            if (prossimo[temi[0]] < FATTI)
                tem = temi[0];
            else if (temi[1] != temi[0] && prossimo[temi[1]] < FATTI)
                tem = temi[1];
            if (tem < 0)
                break;                   /* resta il turno di scrittura */
            int f = prossimo[tem]++;
            char chiave[256], args[512];
            chiave_fatto(chiave, sizeof chiave, tem, f);
            int proposto = parcheggiata[tem][f];
            if (!proposto || !on) {
                if (!on && !lista_larga_fatta) {
                    /* la lista dell'intero namespace, UNA volta [B3] */
                    snprintf(args, sizeof args,
                             "{\"da\":\"nota/\",\"a\":\"nota/zz\",\"k\":64}");
                    jval_t *p = tool(m, "intervallo", args);
                    json_libera(p);
                    lista_larga_fatta = 1;
                } else if (on) {
                    /* la scoperta stretta: il tema e' gia' bucato */
                    char prefisso[64];
                    snprintf(prefisso, sizeof prefisso, "nota/%s-",
                             TEMI[tem].w1);
                    snprintf(args, sizeof args,
                             "{\"da\":\"%s\",\"a\":\"nota/zz\",\"k\":8}",
                             prefisso);
                    jval_t *p = tool(m, "intervallo", args);
                    json_libera(p);
                }
            }
            /* la prima richiesta del turno consuma lo staging in
             * attesa: da qui in poi la spesa e' del filo e il
             * parcheggio e' armato [P6][B4] */
            if (on && in_attesa_n > 0) {
                for (size_t i = 0; i < in_attesa_n; i++) {
                    int tem2, fat2;
                    chiave_dove(in_attesa[i], &tem2, &fat2);
                    if (tem2 >= 0)
                        parcheggiata[tem2][fat2] = 1;
                }
                spesa_s += byte_in_attesa;
                in_attesa_n = 0;
                byte_in_attesa = 0;
            }
            proposto = parcheggiata[tem][f];
            snprintf(args, sizeof args, "{\"chiave\":\"%s\"}", chiave);
            jval_t *p = tool(m, "get", args);
            size_t vl = 0;
            const char *v = json_str(json_get(p, "valore"), &vl);
            if (!v || vl == 0) {
                fprintf(stderr, "GATE ROTTO: il compito non si completa"
                        " (%s)\n", chiave);
                exit(1);
            }
            if (on && proposto)
                hit_s++;
            json_libera(p);

            /* il reasoning NOMINA IL TEMA, mai la chiave [B5] */
            char riga[256];
            int nl = snprintf(riga, sizeof riga,
                              "ho letto il fatto %d di %s %s del motore"
                              " con la misura del contratto della memoria",
                              f, TEMI[tem].w1, TEMI[tem].w2);
            traccia_aggiungi(traccia, &tlen, riga, (size_t)nl);
        }
    }

    /* il conto del filo [B4]: stat dal server, non dalla testa.
     * La stat e' una richiesta: consuma l'eventuale pending rimasto
     * (turno di sola scrittura), percio' il banco lo commette PRIMA */
    if (on && in_attesa_n > 0) {
        for (size_t i = 0; i < in_attesa_n; i++) {
            int tem2, fat2;
            chiave_dove(in_attesa[i], &tem2, &fat2);
            if (tem2 >= 0)
                parcheggiata[tem2][fat2] = 1;
        }
        spesa_s += byte_in_attesa;
        in_attesa_n = 0;
        byte_in_attesa = 0;
    }
    uint64_t hit_filo = 0, spesa_filo = 0;
    double bilancio = 0.0;
    {
        jval_t *p = tool(m, "prefetch_stat", "{}");
        int hv = 0, sv = 0;
        hit_filo = (uint64_t)json_num(json_get(p, "hit"), &hv);
        spesa_filo = (uint64_t)json_num(json_get(p, "spesa_handle"), &sv);
        bilancio = json_num(json_get(p, "bilancio"), NULL);
        json_libera(p);
        if (!on)
            hit_filo = spesa_filo = 0;    /* il filo li' conta, il banco no */
        else if (hit_filo != hit_s || spesa_filo != spesa_s) {
            fprintf(stderr, "GATE [B3] ROTTO: il banco conta con la testa"
                    " (hit %llu, spesa %llu) mentre il filo dice un'altra"
                    " cosa (%llu, %llu)\n",
                    (unsigned long long)hit_s,
                    (unsigned long long)spesa_s,
                    (unsigned long long)hit_filo,
                    (unsigned long long)spesa_filo);
            exit(1);
        }
    }

    uint64_t r1 = 0, b1 = 0;
    mcp_contatori(m, &r1, &b1);
    (void)r0; (void)r1;
    uint64_t filo = (b1 - b0) + spesa_s;      /* il libro del filo [B2] */
    *hit_out = hit_filo;
    *spesa_out = spesa_filo;
    if (on)
        printf("     sessione (%s, tema %s%s%s): %llu byte, hit %llu,"
               " spesa %llu, bilancio parcheggio %+.0f\n",
               brevi ? "brevi" : "lunghe", TEMI[temi[0]].w1,
               n_temi == 2 ? " + " : "",
               n_temi == 2 ? TEMI[temi[1]].w1 : "",
               (unsigned long long)filo, (unsigned long long)hit_filo,
               (unsigned long long)spesa_filo, bilancio);
    else
        printf("     sessione (%s, tema %s%s%s): %llu byte\n",
               brevi ? "brevi" : "lunghe", TEMI[temi[0]].w1,
               n_temi == 2 ? " + " : "",
               n_temi == 2 ? TEMI[temi[1]].w1 : "",
               (unsigned long long)filo);
    mcp_chiudi(m);                       /* la sessione muore [B1] */
    return filo;
}

static uint64_t esegui_regime(int on, int brevi, const int *temi_di,
                             uint64_t *hit_out, uint64_t *spesa_out)
{
    /* il regime parte pulito [B3]: parcheggio nuovo (mcp_apri) e
     * STAGING ASSENTE (il file orfano del regime prima inquina) */
    remove(STAGING);
    int n_sess = brevi ? BREVI : LUNGHE;
    /* [B3] STESSO COMPITO per i due regimi: le lunghe leggono i LORO
     * slot del regista (dopo le brevi), non quelli delle brevi —
     * la prima versione leggeva gli slot da capo e misurava due
     * volte lo stesso film (margine 70.9% contro 71.1%: nessuna gobba,
     * solo un indice bugiardo) */
    int base = brevi ? 0 : BREVI;
    uint64_t tot = 0, hit = 0, spesa = 0;
    char nome[32];
    snprintf(nome, sizeof nome, "%s %s",
             on ? "ON " : "OFF", brevi ? "brevi" : "lunghe");
    printf("--- regime %s:\n", nome);
    for (int s = 0; s < n_sess; s++) {
        int temi[2];
        temi[0] = temi_di[(base + s) * 2];
        temi[1] = temi_di[(base + s) * 2 + 1];
        uint64_t hs = 0, ss = 0;
        tot += esegui_sessione(on, brevi, s, temi, &hs, &ss);
        hit += hs;
        spesa += ss;
    }
    printf("     totale %s: %llu byte, hit %llu, spesa %llu\n", nome,
           (unsigned long long)tot, (unsigned long long)hit,
           (unsigned long long)spesa);
    *hit_out = hit;
    *spesa_out = spesa;
    return tot;
}

int main(void)
{
    printf("il banco multi-sessione: la gobba del cold start, misurata"
           " prima del gateway [B]\n\n");

    /* il pool dei resti [B1]: seminato DAL FILO (put+salva veri), come
     * una sessione morta che l'ha lasciato li' */
    {
        mcp_t *s = mcp_apri(NULL, 1, NULL);    /* in memoria: il seme   */
        if (!s) {
            fprintf(stderr, "GATE ROTTO: il seme non nasce\n");
            return 1;
        }
        jval_t *p = tool(s, "salva",
                         "{\"nome\":\"" DB "\"}");
        const jval_t *sv = json_get(p, "salvato");
        int fatto = (sv && sv->tipo == J_BOOL && sv->booleano);
        json_libera(p);
        mcp_chiudi(s);
        if (!fatto) {
            fprintf(stderr, "GATE ROTTO: il pool vuoto non si crea [B1]\n");
            return 1;
        }
        remove(DB ".wal");          /* il seme paga solo il suo snapshot */

        s = mcp_apri(DB, 1, NULL);
        if (!s) {
            fprintf(stderr, "GATE ROTTO: il pool non apre [B1]\n");
            return 1;
        }
        char chiave[256], valore[512], args[1024];
        for (int t = 0; t < N_TEMI; t++)
            for (int f = 0; f < FATTI; f++) {
                chiave_fatto(chiave, sizeof chiave, t, f);
                valore_fatto(valore, sizeof valore, t, f);
                snprintf(args, sizeof args,
                         "{\"chiave\":\"%s\",\"valore\":\"%s\"}",
                         chiave, valore);
                jval_t *p = tool(s, "put", args);
                json_libera(p);
            }
        p = tool(s, "salva", "{\"nome\":\"" DB "\"}");
        const jval_t *sv2 = json_get(p, "salvato");
        int fatto2 = (sv2 && sv2->tipo == J_BOOL && sv2->booleano);
        json_libera(p);
        mcp_chiudi(s);
        if (!fatto2) {
            fprintf(stderr, "GATE ROTTO: il pool non si conserva\n");
            return 1;
        }
        printf("il pool: %d temi per %d fatti = %d voci, seminato col"
               " filo vero [B1]\n", N_TEMI, FATTI, N_TEMI * FATTI);
    }

    /* il programma dei compiti: lo STESSO film per i due regimi [B3]
     * — il regista e' un prng dichiarato, non il caso del runtime */
    int temi_di[(BREVI + LUNGHE) * 2];
    {
        uint32_t ss = 0x9e3779b9u;      /* seme dichiarato: riproducibile */
        for (int i = 0; i < (int)(sizeof temi_di / sizeof temi_di[0]); i++) {
            ss ^= ss << 13; ss ^= ss >> 17; ss ^= ss << 5;
            temi_di[i] = (int)(ss % N_TEMI);
        }
        /* le BREVI sono da tema singolo [B5]: il compito corto muore
         * prima del dedup, e' tutto il punto della gobba; le LUNGHE
         * lavorano su DUE temi distinti */
        for (int i = 0; i < BREVI * 2; i += 2)
            temi_di[i + 1] = temi_di[i];
        for (int i = BREVI * 2; i < (BREVI + LUNGHE) * 2; i += 2)
            if (temi_di[i] == temi_di[i + 1])
                temi_di[i + 1] = (temi_di[i + 1] + 1) % N_TEMI;
    }

    uint64_t hb = 0, sb = 0, hl = 0, sl = 0;
    uint64_t off_b = esegui_regime(0, 1, temi_di, &hb, &sb);
    uint64_t on_b  = esegui_regime(1, 1, temi_di, &hb, &sb);
    uint64_t off_l = esegui_regime(0, 0, temi_di, &hl, &sl);
    uint64_t on_l  = esegui_regime(1, 0, temi_di, &hl, &sl);

    /* ---- LA MISURA: prima i numeri, poi i gate --------------------- */
    double marg_b = (double)(off_b - on_b) / (double)off_b;
    double marg_l = (double)(off_l - on_l) / (double)off_l;
    printf("\n==== LA MISURA: il libro del filo (byte di risposta +"
           " byte di handle) ====\n");
    printf("sessioni brevi  (%dx%d turni, 1 tema):  OFF %8llu byte"
           "  ON %8llu byte  margine %6.1f%%\n",
           BREVI, TURNI_BREVI,
           (unsigned long long)off_b, (unsigned long long)on_b,
           marg_b * 100.0);
    printf("sessioni lunghe (%dx%d turni, 2 temi): OFF %8llu byte"
           "  ON %8llu byte  margine %6.1f%%\n",
           LUNGHE, TURNI_LUNGHE,
           (unsigned long long)off_l, (unsigned long long)on_l,
           marg_l * 100.0);
    printf("la GOBBA [B6]: margine brevi %.1f%% vs margine lunghe %.1f%%"
           " — MISURA dichiarata, non gate\n", marg_b * 100.0,
           marg_l * 100.0);
    printf("il libro del parcheggio (conferma [P2], non verdetto):"
           " brevi %llu colpi / %llu byte spesa, lunghe %llu colpi /"
           " %llu byte spesa\n",
           (unsigned long long)hb, (unsigned long long)sb,
           (unsigned long long)hl, (unsigned long long)sl);

    /* ---- I GATE (soglie numerate prima del run [B6]) -------------- */
    int fallito = 0;
    if (marg_b < SOGLIA_BREVI) {
        fprintf(stderr, "GATE [B1] ROTTO: margine brevi %.1f%% sotto la"
                " soglia %.0f%%: la speculazione non ripaga dove"
                " dovrebbe\n", marg_b * 100.0, SOGLIA_BREVI * 100.0);
        fallito = 1;
    }
    if (on_l > off_l) {
        fprintf(stderr, "GATE [B2] ROTTO: nelle lunghe ON (%llu byte)"
                " costa PIU' di OFF (%llu): la speculazione inquina e"
                " va spenta\n",
                (unsigned long long)on_l, (unsigned long long)off_l);
        fallito = 1;
    }

    remove(DB);
    remove(DB ".wal");
    remove(DB ".lock");
    remove(STAGING);

    if (fallito)
        return 1;
    printf("\nTUTTI I GATE VERDI: la gobba ha un numero, e il gateway si"
           " decide col dato sul tavolo.\n");
    return 0;
}
