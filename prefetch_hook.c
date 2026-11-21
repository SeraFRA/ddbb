/*
 * prefetch_hook.c — il guscio dell'hook pre_agent. CAPITOLO 12.
 *
 * Il produttore del segnale [P1]: Vibe lo lancia a ogni turno,
 * PRIMA che il modello legga il messaggio, con un payload JSON su
 * stdin (prompt, transcript_path, turn_id). Lui guarda i pensieri
 * (il journal), predice, parcheggia le predizioni nel file di
 * staging [P3] e inietta gli handle nel contesto dell'agente via
 * additional_context [P5].
 *
 * DUE MODI, UN CUORE [P10]:
 *   uso: prefetch_hook [db]              produttore SNAPSHOT [P9]:
 *        le chiavi dallo snapshot PDB1 (motore_apri_da: l'ultimo
 *        salva, orologio fermo come la memoria);
 *   uso: prefetch_hook [db] lavagna     produttore CLIENTE della
 *        lavagna (cap. 14): le chiavi si CHIEDONO AL CUSTODE via
 *        filo [L3] — un intervallo, il batch piu' il totale — e
 *        quello che si vede e' l'ARENA VIVA, non lo snapshot: [P9]
 *        si rafforza. La porta si legge dal file-porta <db>.porta
 *        scritto dal guscio del gateway (derivato: gitignored).
 *
 * FAIL-OPEN SEMPRE: un prefetcher rotto non deve MAI fermare il
 * turno. Ogni errore e' silenzio: exit 0, stdout vuoto. Il costo di
 * un hook muto e' zero predizioni, non un turno bloccato.
 *
 * I LIMITI DEL CLIENTE (dichiarati in testa, come sempre):
 *  - l'intervallo del filo porta UN batch (k=64): una lavagna con
 *    piu' voci resta non predicibile dall'hook cliente — una
 *    memoria di lavoro ci sta dentro, un magazzino no;
 *  - la risposta del filo oltre FILO_RISPOSTA_MAX byte e' silenzio
 *    (cap = il filo stesso del cap. 11, non un mega-batch);
 *  - la stale-port e' dichiara in gateway.h: custode morto = porta
 *    morta = silenzio.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "motore.h"
#include "prefetch.h"
#include "gateway.h"
#include "json.h"

#define PAYLOAD_MAX  (1024u * 1024u)
#define FILO_RISPOSTA_MAX (256u * 1024u)
#define FILO_BATCH_MAX 64             /* il cap del batch [M2], [P10] */

/* legge tutto stdin: il payload e' piccolo, il filo e' un pipe */
static char *leggi_stdin(size_t *len)
{
    char *buf = malloc(PAYLOAD_MAX);
    if (!buf)
        return NULL;
    size_t n = 0;
    while (n < PAYLOAD_MAX) {
        size_t r = fread(buf + n, 1, PAYLOAD_MAX - n, stdin);
        if (r == 0)
            break;
        n += r;
    }
    *len = n;
    return buf;
}

/* il produttore CLIENTE della lavagna [P10]: l'elenco delle chiavi
 * lo consegna il custode via filo — arena viva, due parse (la busta
 * JSON-RPC, poi il text interno). Fallimento d'ogni tipo = 0: il
 * silenzio e' onesto anche contro il TCP. */
static size_t predici_via_filo(const char *db, const char *traccia,
                               size_t tlen, prefetch_predizione_t pred[],
                               size_t max_pred)
{
    /* la porta del custode: file-porta derivato, letto cittadino */
    char pporta[1024];
    gateway_percorso_porta(db, pporta, sizeof pporta);
    FILE *fp = fopen(pporta, "r");
    if (!fp)
        return 0;                       /* niente lavagna: silenzio  */
    char rigap[32];
    int letta = (fgets(rigap, sizeof rigap, fp) != NULL);
    fclose(fp);
    if (!letta)
        return 0;
    uint32_t porta = (uint32_t)strtoul(rigap, NULL, 10);
    if (porta == 0)
        return 0;

    gateway_cliente_t cl;
    if (gateway_cliente_apri(&cl, porta) != 0)
        return 0;                       /* custode morto: silenzio  */

    /* l'elenco delle chiavi: un intervallo, il batch piu' il totale
     * [M2][P10]. k = 64: il cap del batch del filo, limite dichiarato.
     * Il range '' .. \\xff*4 crudo: il filo e' trasparente ai byte
     * (json.h: ingresso controllo nudo rifiutato, il resto passa). */
    jbuf_t req;
    if (jbuf_init(&req) != 0) {
        gateway_cliente_chiudi(&cl);
        return 0;
    }
    JBUF_LIT(&req, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                   "\"tools/call\",\"params\":{\"name\":\"intervallo\","
                   "\"arguments\":{\"da\":\"\",\"a\":\"");
    jbuf_testo(&req, "\xff\xff\xff\xff", 4);
    JBUF_LIT(&req, "\",\"k\":64}}}\n");
    if (req.rotto) {
        jbuf_libera(&req);
        gateway_cliente_chiudi(&cl);
        return 0;
    }
    int scritto = gateway_cliente_scrivi(&cl, req.p, req.n);
    jbuf_libera(&req);
    if (scritto != 0) {
        gateway_cliente_chiudi(&cl);
        return 0;
    }

    char *risp = malloc(FILO_RISPOSTA_MAX);
    if (!risp) {
        gateway_cliente_chiudi(&cl);
        return 0;
    }
    int n_let = gateway_cliente_leggi(&cl, risp, FILO_RISPOSTA_MAX);
    gateway_cliente_chiudi(&cl);
    if (n_let != 1) {                   /* riga intera o silenzio    */
        free(risp);
        return 0;
    }
    /* POST MORTEM: leggi torna 1 (riga intera), NON la lunghezza —
     * la prima versione glielo faceva credere e il parse riceveva
     * un byte di busta: fail-open rendeva il bug INVISIBILE (solo
     * silenzio). Un cliente del filo misura la sua merce: strlen. */
    size_t risp_n = strlen(risp);

    /* parse #1: la busta JSON-RPC; il text interno e' decodificato */
    jval_t *busta = json_parse(risp, risp_n);
    free(risp);
    if (!busta)
        return 0;
    const jval_t *risultato = json_get(busta, "result");
    const jval_t *contenuto = risultato ? json_get(risultato, "content")
                                        : NULL;
    if (!contenuto || contenuto->tipo != J_ARR || contenuto->n == 0) {
        json_libera(busta);
        return 0;
    }
    size_t t_n = 0;
    const char *testo = json_str(json_get(contenuto->figli[0], "text"),
                                 &t_n);
    if (!testo || t_n == 0) {
        json_libera(busta);
        return 0;
    }

    /* parse #2: il payload del tool (totali, trovate[]) */
    jval_t *dentro = json_parse(testo, t_n);
    json_libera(busta);
    if (!dentro)
        return 0;
    const jval_t *trovate = json_get(dentro, "trovate");
    if (!trovate || trovate->tipo != J_ARR || trovate->n == 0) {
        json_libera(dentro);
        return 0;
    }

    /* le chiavi dal filo: puntano ai buffer di `dentro`, vivi fino
     * alla predizione — il cuore le COPIA [P2]. FILO_BATCH_MAX e'
     * il cap del batch dell'intervallo del filo (64, [M2]): il
     * limite dichiarato del cliente. */
    motore_risposta_t chiavi[FILO_BATCH_MAX];
    size_t n_chiavi = 0;
    for (size_t i = 0; i < trovate->n &&
                        n_chiavi < FILO_BATCH_MAX; i++) {
        size_t kl = 0, vl = 0;
        const char *k = json_str(json_get(trovate->figli[i], "chiave"),
                                 &kl);
        const char *v = json_str(json_get(trovate->figli[i], "valore"),
                                 &vl);
        if (!k || !v || kl == 0 || kl >= PREFETCH_CHIAVE_MAX)
            continue;
        chiavi[n_chiavi].chiave = (const uint8_t *)k;
        chiavi[n_chiavi].chiave_len = (uint32_t)kl;
        chiavi[n_chiavi].valore = (const uint8_t *)v;
        chiavi[n_chiavi].valore_len = (uint32_t)vl;
        chiavi[n_chiavi].confidenza = 1.0;
        n_chiavi++;
    }
    size_t n = prefetch_predici_da_chiavi(chiavi, n_chiavi, traccia,
                                         tlen, pred, max_pred);
    json_libera(dentro);
    return n;
}

int main(int argc, char **argv)
{
    size_t plen = 0;
    char *payload = leggi_stdin(&plen);
    if (!payload)
        return 0;                       /* fail-open: silenzio       */
    jval_t *root = json_parse(payload, plen);
    free(payload);
    if (!root)
        return 0;

    size_t prompt_len = 0, tr_path_len = 0, gen_len = 0;
    const char *prompt  = json_str(json_get(root, "prompt"), &prompt_len);
    const char *tr_path  = json_str(json_get(root, "transcript_path"),
                                   &tr_path_len);
    const char *gen     = json_str(json_get(root, "turn_id"), &gen_len);
    if (!gen || gen_len == 0 || gen_len >= PREFETCH_GEN_MAX) {
        json_libera(root);
        return 0;                       /* senza generazione non c'e'
                                           contratto [P3]            */
    }

    /* la traccia: il ragionamento recente dal journal, poi il
     * prompt nuovo in CODA (la finestra piu' viva [P1]) */
    char traccia[PREFETCH_TRACCIA_MAX];
    size_t tlen = tr_path
        ? prefetch_traccia_da_journal(tr_path, traccia, sizeof traccia)
        : 0;
    if (prompt && tlen + prompt_len + 2 < sizeof traccia) {
        if (tlen)
            traccia[tlen++] = '\n';
        memcpy(traccia + tlen, prompt, prompt_len);
        tlen += prompt_len;
        traccia[tlen] = '\0';
    }

    const char *db = (argc > 1) ? argv[1] : "memoria.db";
    int lavagna = (argc > 2 && strcmp(argv[2], "lavagna") == 0);

    prefetch_predizione_t pred[PREFETCH_PREDIZIONI_MAX];
    size_t n = 0;
    if (lavagna) {
        /* il cliente della lavagna [P10]: fila col custode */
        n = predici_via_filo(db, traccia, tlen, pred,
                             PREFETCH_PREDIZIONI_MAX);
    } else {
        /* il produttore snapshot [P9]: l'ultimo salva */
        motore_t *m = motore_apri_da(db, 1, NULL);
        if (m) {
            n = prefetch_predici(m, traccia, tlen, pred,
                                 PREFETCH_PREDIZIONI_MAX);
            motore_chiudi(m);
        }
    }
    if (n == 0) {
        json_libera(root);
        return 0;                       /* il silenzio e' onesto [P5] */
    }

    /* l'handle one-liner [P5]: chiavi e confidenza, MAI i valori */
    jbuf_t h;
    if (jbuf_init(&h) != 0) {
        json_libera(root);
        return 0;
    }
    JBUF_LIT(&h, "memoria prefetch (FORSE, similarita' sintattica): ");
    for (size_t i = 0; i < n; i++) {
        if (i)
            JBUF_LIT(&h, ", ");
        jbuf_add(&h, pred[i].chiave, pred[i].chiave_len);
        JBUF_LIT(&h, " ");
        jbuf_num(&h, pred[i].confidenza);
    }
    JBUF_LIT(&h, " — get <chiave> per il valore");
    if (h.rotto) {
        jbuf_libera(&h);
        json_libera(root);
        return 0;
    }

    char staging[1024];
    prefetch_percorso_staging(db, staging, sizeof staging);
    if (prefetch_scrivi_staging(staging, gen, pred, n, h.n) != 0) {
        jbuf_libera(&h);
        json_libera(root);
        return 0;                       /* niente parcheggio: non si
                                           promette ciò che non si
                                           tira [P3] */
    }

    /* il contratto di Vibe: questo e' l'UNICO stdout */
    jbuf_t out;
    if (jbuf_init(&out) == 0) {
        JBUF_LIT(&out, "{\"hook_specific_output\":{\"additional_context\":\"");
        jbuf_testo(&out, h.p, h.n);
        JBUF_LIT(&out, "\"}}");
        if (!out.rotto && out.n > 0)
            fwrite(out.p, 1, out.n, stdout);
        jbuf_libera(&out);
    }
    jbuf_libera(&h);
    json_libera(root);
    return 0;
}
