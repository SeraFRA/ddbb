/*
 * prefetch.c — il prefetcher speculativo. CAPITOLO 12.
 *
 * Il contratto [P1]..[P9] vive in prefetch.h: qui la meccanica che lo
 * mantiene. Due meta' e un filo: il produttore (traccia -> predizioni
 * -> file di staging) e il consumatore (file -> parcheggio -> colpi ->
 * MISURA). Il filo tra i due e' il JSON dello staging [P3], e json.c
 * resta l'UNICA fonte di verita' sul filo (regola del repo).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>

#include "prefetch.h"
#include "json.h"

/* il tetto del journal letto in un colpo [limiti in testa al .h] */
#define JOURNAL_MAX_BYTE   (16u * 1024u * 1024u)
#define STAGING_MAX_BYTE   (64u * 1024u)
#define CANDIDATI_MAX      128

/* memmem portabile (MinGW non la dichiara): la traccia e' corta,
 * la ricerca e' nuda e onesta */
static int memmem_traccia(const char *pagliaio, size_t n,
                          const char *ago, size_t m)
{
    if (m == 0 || m > n)
        return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (pagliaio[i] == ago[0] &&
            memcmp(pagliaio + i, ago, m) == 0)
            return 1;
    return 0;
}

/* ---------------------------------------------------------- il produttore */

/* un token e' una run di caratteri da chiave: le chiavi della memoria
 * (in-corso/capitolo-12) devono restare INTERE, o la finestra non le
 * vede mai [P1] */
static int char_token(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '/' || c == '.';
}

/* i bigrammi NON posizionali del layer [P1]: casefold ASCII, il
 * bigramma e' la coppia di caratteri, l'insieme e' ordinato e unico.
 * (POST MORTEM in prefetch.h: i grammi del motore sono posizionali,
 * chiedi e' un fuzzy-prefix — la similarita' per tema vive qui.) */
#define GRAMMI_SET_MAX 256

static double jaccard_u16(const uint16_t *a, size_t na,
                          const uint16_t *b, size_t nb)
{
    size_t i = 0, j = 0, inter = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) i++;
        else if (a[i] > b[j]) j++;
        else { inter++; i++; j++; }
    }
    size_t uni = na + nb - inter;
    return uni ? (double)inter / (double)uni : 0.0;
}

/* i bigrammi di un testo in un insieme ordinato e unico; il testo
 * oltre 64 byte si guarda dall'inizio (limite dichiarato) */
static size_t inserisci_grammi(uint16_t *set, size_t n, size_t cap,
                               const char *testo, size_t len)
{
    if (len > 64)
        len = 64;
    for (size_t i = 0; i + 1 < len && n < cap; i++) {
        uint16_t a = (uint8_t)testo[i];
        uint16_t b = (uint8_t)testo[i + 1];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        uint16_t g = (uint16_t)((a << 8) | b);
        size_t j = n;
        while (j > 0 && set[j - 1] > g) {
            set[j] = set[j - 1];
            j--;
        }
        if (j > 0 && set[j - 1] == g)
            continue;                   /* unico: il doppio non conta */
        set[j] = g;
        n++;
    }
    return n;
}

static size_t set_bigrammi(uint16_t *out, size_t cap,
                           const char *testo, size_t len)
{
    return inserisci_grammi(out, 0, cap, testo, len);
}

size_t prefetch_predici(motore_t *m, const char *traccia, size_t tlen,
                        prefetch_predizione_t pred[], size_t max_pred)
{
    if (!m || !traccia || !pred || max_pred == 0)
        return 0;

    /* i token della traccia: inizio e fine, una passata sola */
    size_t cap_tok = 256;
    size_t *tin = malloc(cap_tok * sizeof *tin);
    size_t *tfi = malloc(cap_tok * sizeof *tfi);
    if (!tin || !tfi) {
        free(tin);
        free(tfi);
        return 0;
    }
    size_t n_tok = 0, i = 0;
    while (i < tlen) {
        if (!char_token((unsigned char)traccia[i])) {
            i++;
            continue;
        }
        size_t inizio = i;
        while (i < tlen && char_token((unsigned char)traccia[i]))
            i++;
        if (n_tok == cap_tok) {
            cap_tok *= 2;
            size_t *a = realloc(tin, cap_tok * sizeof *tin);
            size_t *b = realloc(tfi, cap_tok * sizeof *tfi);
            if (!a || !b) {
                free(a ? a : tin);
                free(b ? b : tfi);
                return 0;
            }
            tin = a;
            tfi = b;
        }
        tin[n_tok] = inizio;
        tfi[n_tok] = i;
        n_tok++;
    }
    if (n_tok == 0) {
        free(tin);
        free(tfi);
        return 0;
    }

    /* le finestre: W token consecutive, la CODA della traccia e' la
     * piu' viva [P1]. Pochi token = una finestra sola, comunque. */
    const size_t W = PREFETCH_FINESTRA_PAROLE;
    size_t n_finestre = (n_tok >= W) ? n_tok - W + 1 : 1;
    size_t prima = 0;
    if (n_finestre > PREFETCH_FINESTRE_MAX)
        prima = n_finestre - PREFETCH_FINESTRE_MAX;
    n_finestre -= prima;

    /* i bigrammi NON posizionali di ogni finestra, precomputati:
     * il costo della similarita' si paga una volta, non per chiave */
    uint16_t *wset = malloc(PREFETCH_FINESTRE_MAX * GRAMMI_SET_MAX
                            * sizeof *wset);
    size_t   *wn   = malloc(PREFETCH_FINESTRE_MAX * sizeof *wn);
    if (!wset || !wn) {
        free(wset);
        free(wn);
        free(tin);
        free(tfi);
        return 0;
    }
    for (size_t f = 0; f < n_finestre; f++) {
        uint16_t *set = wset + f * GRAMMI_SET_MAX;
        size_t n_set = 0;
        size_t da = prima + f;
        size_t a = (n_tok >= W) ? da + W : n_tok;
        for (size_t t = da; t < a; t++)
            n_set = inserisci_grammi(set, n_set, GRAMMI_SET_MAX,
                                     traccia + tin[t], tfi[t] - tin[t]);
        wn[f] = n_set;
    }

    /* le chiavi vive: il scan completo via intervallo, il lavoro che
     * l'indice ordinato sa fare [P1]. Due passate: prima il totale,
     * poi il batch — l'arena e' privata dell'hook, non si muove. */
    size_t totali = 0;
    motore_intervallo(m, "", 0, "\xff\xff\xff\xff", 4, 0, NULL,
                      &totali, NULL);
    if (totali == 0) {
        free(wset);
        free(wn);
        free(tin);
        free(tfi);
        return 0;
    }
    if (totali > PREFETCH_CHIAVI_MAX)
        totali = PREFETCH_CHIAVI_MAX;   /* il tetto dichiarato       */
    motore_risposta_t *chiavi = malloc(totali * sizeof *chiavi);
    if (!chiavi) {
        free(wset);
        free(wn);
        free(tin);
        free(tfi);
        return 0;
    }
    size_t n_chiavi = motore_intervallo(m, "", 0, "\xff\xff\xff\xff",
                                        4, totali, chiavi, NULL, NULL);

    /* i candidati aggregati: chiave copiata, mai un puntatore
     * all'arena [P2] — le risposte del motore muoiono al giro dopo. */
    prefetch_predizione_t cand[CANDIDATI_MAX];
    size_t n_cand = 0;
    uint16_t kset[GRAMMI_SET_MAX];

    for (size_t c = 0; c < n_chiavi; c++) {
        size_t kl = chiavi[c].chiave_len;
        const char *k = (const char *)chiavi[c].chiave;
        if (kl == 0 || kl >= PREFETCH_CHIAVE_MAX)
            continue;                   /* limite dichiarato          */
        /* [P4]: gia' in vista nella traccia = dedup, il contesto e'
         * la risorsa cara */
        if (memmem_traccia(traccia, tlen, k, kl))
            continue;
        size_t kn = set_bigrammi(kset, GRAMMI_SET_MAX, k, kl);
        if (kn == 0)
            continue;
        double migliore = 0.0;
        for (size_t f = 0; f < n_finestre; f++) {
            double j = jaccard_u16(kset, kn, wset + f * GRAMMI_SET_MAX,
                                   wn[f]);
            if (j > migliore)
                migliore = j;
        }
        if (migliore < PREFETCH_SOGLIA)
            continue;                   /* rumore, non "forse" [P5]   */
        if (n_cand < CANDIDATI_MAX) {
            memcpy(cand[n_cand].chiave, k, kl);
            cand[n_cand].chiave[kl] = '\0';
            cand[n_cand].chiave_len = kl;
            cand[n_cand].confidenza = migliore;
            /* il costo di scoperta [P2]: i byte della risposta che
             * l'agente avrebbe dovuto leggere per trovarsi qui */
            cand[n_cand].costo_scoperta = 43 + (uint64_t)kl +
                                          (uint64_t)chiavi[c].valore_len;
            n_cand++;
        } else {
            /* pieno: la predizione piu' debole cede il posto */
            size_t debole = 0;
            for (size_t w = 1; w < n_cand; w++)
                if (cand[w].confidenza < cand[debole].confidenza)
                    debole = w;
            if (migliore <= cand[debole].confidenza)
                continue;
            memcpy(cand[debole].chiave, k, kl);
            cand[debole].chiave[kl] = '\0';
            cand[debole].chiave_len = kl;
            cand[debole].confidenza = migliore;
            cand[debole].costo_scoperta = 43 + (uint64_t)kl +
                                          (uint64_t)chiavi[c].valore_len;
        }
    }
    free(chiavi);
    free(wset);
    free(wn);
    free(tin);
    free(tfi);

    /* ordina per confidenza discendente e taglia: le handle sono
     * poche e one-liner [P5] */
    for (size_t a = 1; a < n_cand; a++) {
        prefetch_predizione_t tmp = cand[a];
        size_t b = a;
        while (b > 0 && cand[b - 1].confidenza < tmp.confidenza) {
            cand[b] = cand[b - 1];
            b--;
        }
        cand[b] = tmp;
    }
    if (n_cand > max_pred)
        n_cand = max_pred;
    memcpy(pred, cand, n_cand * sizeof *pred);
    return n_cand;
}

int prefetch_scrivi_staging(const char *percorso, const char *gen,
                            const prefetch_predizione_t pred[],
                            size_t n, uint64_t handle_byte)
{
    if (!percorso || !gen || (!pred && n > 0))
        return -1;

    jbuf_t b;
    if (jbuf_init(&b) != 0)
        return -1;
    JBUF_LIT(&b, "{\"gen\":\"");
    jbuf_testo(&b, gen, strlen(gen));
    JBUF_LIT(&b, "\",\"handle\":");
    jbuf_num(&b, (double)handle_byte);
    JBUF_LIT(&b, ",\"voci\":[");
    for (size_t i = 0; i < n; i++) {
        if (i)
            JBUF_LIT(&b, ",");
        JBUF_LIT(&b, "{\"k\":\"");
        jbuf_testo(&b, pred[i].chiave, pred[i].chiave_len);
        JBUF_LIT(&b, "\",\"c\":");
        jbuf_num(&b, pred[i].confidenza);
        JBUF_LIT(&b, ",\"b\":");
        jbuf_num(&b, (double)pred[i].costo_scoperta);
        JBUF_LIT(&b, "}");
    }
    JBUF_LIT(&b, "]}");
    if (b.rotto) {
        jbuf_libera(&b);
        return -1;
    }

    /* atomico [P3]: tmp, poi il rename. Il remove prima del rename
     * lascia un istante senza file: il consumatore vede "niente di
     * nuovo", che in quell'istante e' la verita'. */
    char tmp[1024];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", percorso) >= (int)sizeof tmp) {
        jbuf_libera(&b);
        return -1;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        jbuf_libera(&b);
        return -1;
    }
    int ok = fwrite(b.p, 1, b.n, f) == b.n;
    ok = (fclose(f) == 0) && ok;
    jbuf_libera(&b);
    if (!ok)
        return -1;
    remove(percorso);
    if (rename(tmp, percorso) != 0)
        return -1;
    return 0;
}

void prefetch_percorso_staging(const char *db, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (!db) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%s.prefetch", db);
}

/* -------------------------------------------- la traccia dal journal [P1] */

/* l'ultimo file .jsonl del journal: i nomi sono numeri a zeri */
static char *ultimo_journal(const char *dir_sessione)
{
    static char percorso[1024];
    size_t dl = strlen(dir_sessione);
    if (dl == 0)
        return NULL;
    if (snprintf(percorso, sizeof percorso, "%s%sjournal",
                 dir_sessione,
                 dir_sessione[dl - 1] == '/' ? "" : "/")
        >= (int)sizeof percorso)
        return NULL;
    DIR *d = opendir(percorso);
    if (!d)
        return NULL;
    char migliore[280] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t ln = strlen(e->d_name);
        if (ln > 6 && ln < sizeof migliore &&
            strcmp(e->d_name + ln - 6, ".jsonl") == 0 &&
            strcmp(e->d_name, migliore) > 0)
            snprintf(migliore, sizeof migliore, "%s", e->d_name);
    }
    closedir(d);
    if (migliore[0] == '\0')
        return NULL;
    static char completo[1280];
    if (snprintf(completo, sizeof completo, "%s/%s",
                 percorso, migliore) >= (int)sizeof completo)
        return NULL;
    return completo;
}

/* raccoglie i testi dell'assistente da UN record completion */
/* accumula un testo COPIANDOLO, con dedup per testo identico: la
 * memoria e' del chiamante, non dell'albero JSON che sta per morire */
static void accumula_testo(const char *tx, size_t tl,
                            char *testi[], size_t lunghezze[],
                            size_t *n_testi, size_t max_testi)
{
    for (size_t i = 0; i < *n_testi; i++)
        if (lunghezze[i] == tl &&
            memcmp(testi[i], tx, tl) == 0)
            return;                     /* gia' raccolto: un retry non
                                           si conta due volte        */
    if (*n_testi >= max_testi)
        return;
    char *copia = malloc(tl + 1);
    if (!copia)
        return;
    memcpy(copia, tx, tl);
    copia[tl] = '\0';
    testi[*n_testi] = copia;
    lunghezze[*n_testi] = tl;
    (*n_testi)++;
}

/* raccoglie i testi dell'assistente da UN record completion. I testi
 * sono COPIATI (malloc): l'albero JSON muore col record, la traccia
 * no [P2, la stessa regola delle chiavi]. Il dedup e' qui: lo stesso
 * pensiero rispediato da un retry non si conta due volte. */
static void raccogli_testi(const jval_t *root,
                           char *testi[], size_t lunghezze[],
                           size_t *n_testi, size_t max_testi)
{
    const jval_t *msgs = json_get(root, "payload");
    msgs = json_get(msgs, "request");
    msgs = json_get(msgs, "model_input");
    msgs = json_get(msgs, "messages");
    msgs = json_get(msgs, "messages");
    if (!msgs || msgs->tipo != J_ARR)
        return;
    for (size_t i = 0; i < msgs->n && *n_testi < max_testi; i++) {
        const jval_t *msg = msgs->figli[i];
        const char *role = json_str(json_get(msg, "role"), NULL);
        if (!role || strcmp(role, "assistant") != 0)
            continue;
        const jval_t *cont = json_get(msg, "content");
        if (!cont || cont->tipo != J_ARR)
            continue;
        for (size_t c = 0; c < cont->n && *n_testi < max_testi; c++) {
            const jval_t *item = cont->figli[c];
            const char *tipo = json_str(json_get(item, "type"), NULL);
            if (tipo && strcmp(tipo, "reasoning") == 0) {
                /* il ragionamento e' annidato: content[].text */
                const jval_t *interno = json_get(item, "content");
                if (!interno || interno->tipo != J_ARR)
                    continue;
                for (size_t t = 0;
                     t < interno->n && *n_testi < max_testi; t++) {
                    size_t tl = 0;
                    const char *tx = json_str(
                        json_get(interno->figli[t], "text"), &tl);
                    if (tx)
                        accumula_testo(tx, tl, testi, lunghezze,
                                       n_testi, max_testi);
                }
            } else if (tipo && strcmp(tipo, "text") == 0) {
                size_t tl = 0;
                const char *tx = json_str(json_get(item, "text"), &tl);
                if (tx)
                    accumula_testo(tx, tl, testi, lunghezze,
                                   n_testi, max_testi);
            }
        }
    }
}

size_t prefetch_traccia_da_journal(const char *dir_sessione,
                                   char *traccia, size_t cap)
{
    if (!dir_sessione || !traccia || cap == 0)
        return 0;
    traccia[0] = '\0';

    const char *percorso = ultimo_journal(dir_sessione);
    if (!percorso)
        return 0;

    FILE *f = fopen(percorso, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long dim = ftell(f);
    if (dim <= 0) {
        fclose(f);
        return 0;
    }
    size_t leggi = (size_t)dim;
    long salta = 0;
    if (leggi > JOURNAL_MAX_BYTE) {
        salta = dim - (long)JOURNAL_MAX_BYTE;
        leggi = JOURNAL_MAX_BYTE;
    }
    if (fseek(f, salta, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    char *buf = malloc(leggi + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, leggi, f) != leggi) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[leggi] = '\0';
    if (salta > 0) {
        /* si parte dal confine di riga: la prima riga parziale e'
         * spazzatura, non un record */
        char *nl = memchr(buf, '\n', leggi);
        if (!nl) {
            free(buf);
            return 0;
        }
        leggi -= (size_t)(nl - buf) + 1;
        memmove(buf, nl + 1, leggi);
        buf[leggi] = '\0';
    }

    /* i confini di riga, dalla fine verso l'inizio: la coda e' il
     * pensiero piu' recente [P1] */
    size_t *righe = malloc((leggi / 16 + 2) * sizeof *righe);
    if (!righe) {
        free(buf);
        return 0;
    }
    size_t n_righe = 0;
    righe[n_righe++] = 0;
    for (size_t i = 0; i < leggi; i++)
        if (buf[i] == '\n') {
            buf[i] = '\0';
            righe[n_righe++] = i + 1;
        }

    /* i testi piu' recenti prima, con dedup: lo stesso pensiero puo'
     * viaggiare in piu' record (i retry lo rispediscono) */
    char *testi[64];
    size_t lunghezze[64];
    size_t n_testi = 0;

    for (size_t r = n_righe; r-- > 0 && n_testi < 64;) {
        const char *riga = buf + righe[r];
        if (*riga == '\0')
            continue;
        if (!strstr(riga, "\"kind\":\"completion\""))
            continue;                   /* pre-filtro: non si parse-a
                                           tutto il journal */
        jval_t *root = json_parse(riga, strlen(riga));
        if (!root)
            continue;
        raccogli_testi(root, testi, lunghezze, &n_testi, 64);
        json_libera(root);
    }
    free(righe);

    /* assembla dal piu' VECCHIO al piu' NUOVO: la coda della traccia
     * e' il pensiero vivo, e le finestre guardano la coda [P1] */
    size_t scritti = 0;
    for (size_t t = n_testi; t-- > 0;) {
        if (scritti >= cap)
            break;
        size_t spazio = cap - scritti;
        size_t prendi = lunghezze[t] < spazio ? lunghezze[t] : spazio;
        memcpy(traccia + scritti, testi[t], prendi);
        scritti += prendi;
        if (scritti < cap)
            traccia[scritti++] = '\n';
    }
    traccia[scritti < cap ? scritti : cap - 1] = '\0';
    for (size_t t = 0; t < n_testi; t++)
        free(testi[t]);                 /* le copie muoiono qui, la
                                           traccia e' assemblata     */
    free(buf);
    return scritti;
}

/* ---------------------------------------------------------- il consumatore */

struct pf_voce {
    char     chiave[PREFETCH_CHIAVE_MAX];
    size_t   chiave_len;
    double   confidenza;
    uint64_t costo_scoperta;
    uint64_t nata;          /* il giro del chiamante [P6]            */
    int      colpita;       /* il colpo conta UNA volta [P7]         */
};

struct prefetch_parcheggio {
    struct pf_voce *voci;
    size_t   capacita;
    size_t   n;
    char     ultima_gen[PREFETCH_GEN_MAX];
    uint64_t spesa_handle;
    uint64_t risparmio;
    uint64_t parcheggiate;
    uint64_t hit;
    uint64_t morte;
    uint32_t ttl;
};

prefetch_parcheggio_t *prefetch_parcheggio_apri(size_t capacita,
                                                uint32_t ttl)
{
    if (capacita == 0)
        capacita = PREFETCH_PARCHEGGIO_MAX;
    prefetch_parcheggio_t *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->voci = calloc(capacita, sizeof *p->voci);
    if (!p->voci) {
        free(p);
        return NULL;
    }
    p->capacita = capacita;
    p->ttl = ttl ? ttl : PREFETCH_TTL_RICHIESTE;
    return p;
}

void prefetch_parcheggio_chiudi(prefetch_parcheggio_t *p)
{
    if (!p)
        return;
    free(p->voci);
    free(p);
}

/* la morte in batch [P6]: chi risponde paga un controllo solo */
static void spazza_morti(prefetch_parcheggio_t *p, uint64_t giro)
{
    size_t vive = 0;
    for (size_t i = 0; i < p->n; i++) {
        if (giro - p->voci[i].nata < p->ttl) {
            p->voci[vive++] = p->voci[i];
        } else if (!p->voci[i].colpita) {
            p->morte++;      /* speculazione inquinante, contata [P7] */
        }
    }
    p->n = vive;
}

size_t prefetch_consuma(prefetch_parcheggio_t *p, const char *percorso,
                        uint64_t giro)
{
    if (!p || !percorso)
        return 0;
    spazza_morti(p, giro);

    FILE *f = fopen(percorso, "rb");
    if (!f)
        return 0;
    char *buf = malloc(STAGING_MAX_BYTE);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t letti = fread(buf, 1, STAGING_MAX_BYTE, f);
    fclose(f);
    if (letti == 0) {
        free(buf);
        return 0;
    }
    jval_t *root = json_parse(buf, letti);
    free(buf);
    if (!root)
        return 0;                       /* strappato = silenzio [P3]  */

    size_t gen_len = 0;
    const char *gen = json_str(json_get(root, "gen"), &gen_len);
    if (!gen || gen_len >= PREFETCH_GEN_MAX ||
        strcmp(gen, p->ultima_gen) == 0) {
        json_libera(root);
        return 0;                       /* una generazione, un colpo
                                           solo [P3] */
    }
    int valido = 0;
    uint64_t handle = (uint64_t)json_num(json_get(root, "handle"),
                                         &valido);
    if (!valido)
        handle = 0;

    const jval_t *voci = json_get(root, "voci");
    size_t parcheggiate = 0;
    if (voci && voci->tipo == J_ARR) {
        for (size_t i = 0; i < voci->n; i++) {
            size_t kl = 0;
            const char *k = json_str(json_get(voci->figli[i], "k"), &kl);
            if (!k || kl == 0 || kl >= PREFETCH_CHIAVE_MAX)
                continue;
            int cv = 0, bv = 0;
            double c = json_num(json_get(voci->figli[i], "c"), &cv);
            uint64_t b = (uint64_t)json_num(json_get(voci->figli[i], "b"),
                                            &bv);
            if (!cv)
                c = 0;
            if (!bv)
                b = 0;

            /* gia' parcheggiata: la ri-conferma rinfresca la nascita
             * (il pensiero la indica DI NUOVO) e tiene il meglio */
            size_t v = 0;
            while (v < p->n &&
                   (p->voci[v].chiave_len != kl ||
                    memcmp(p->voci[v].chiave, k, kl) != 0))
                v++;
            if (v < p->n) {
                if (c > p->voci[v].confidenza) {
                    p->voci[v].confidenza     = c;
                    p->voci[v].costo_scoperta = b;
                }
                p->voci[v].nata = giro;
                continue;
            }
            if (p->n < p->capacita) {
                v = p->n++;
            } else {
                /* pieno: la speculazione peggiore cede il posto */
                size_t peggiore = 0;
                for (size_t w = 1; w < p->n; w++)
                    if (p->voci[w].confidenza <
                        p->voci[peggiore].confidenza)
                        peggiore = w;
                if (c <= p->voci[peggiore].confidenza)
                    continue;
                v = peggiore;
            }
            memcpy(p->voci[v].chiave, k, kl);
            p->voci[v].chiave[kl] = '\0';
            p->voci[v].chiave_len     = kl;
            p->voci[v].confidenza     = c;
            p->voci[v].costo_scoperta = b;
            p->voci[v].nata           = giro;
            p->voci[v].colpita        = 0;
            parcheggiate++;
        }
    }
    p->parcheggiate += parcheggiate;
    p->spesa_handle += handle;
    snprintf(p->ultima_gen, sizeof p->ultima_gen, "%s", gen);
    json_libera(root);
    return parcheggiate;
}

int prefetch_tocca(prefetch_parcheggio_t *p, const char *chiave,
                   size_t klen, uint64_t giro)
{
    if (!p || !chiave)
        return 0;
    for (size_t i = 0; i < p->n; i++) {
        if (p->voci[i].chiave_len != klen ||
            memcmp(p->voci[i].chiave, chiave, klen) != 0)
            continue;
        if (giro - p->voci[i].nata >= p->ttl)
            return 0;                   /* morta: il colpo non c'e'   */
        if (!p->voci[i].colpita) {
            p->voci[i].colpita = 1;
            p->hit++;
            p->risparmio += p->voci[i].costo_scoperta;
        }
        return 1;
    }
    return 0;
}

void prefetch_stat(const prefetch_parcheggio_t *p, uint64_t giro,
                   uint64_t *parcheggiate, uint64_t *vive,
                   uint64_t *hit, uint64_t *morte,
                   uint64_t *spesa_handle, uint64_t *risparmio)
{
    if (!p) {
        if (parcheggiate) *parcheggiate = 0;
        if (vive)         *vive = 0;
        if (hit)          *hit = 0;
        if (morte)        *morte = 0;
        if (spesa_handle) *spesa_handle = 0;
        if (risparmio)    *risparmio = 0;
        return;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < p->n; i++)
        if (giro - p->voci[i].nata < p->ttl)
            v++;
    if (parcheggiate) *parcheggiate = p->parcheggiate;
    if (vive)         *vive = v;
    if (hit)          *hit = p->hit;
    if (morte)        *morte = p->morte;
    if (spesa_handle) *spesa_handle = p->spesa_handle;
    if (risparmio)    *risparmio = p->risparmio;
}
