/*
 * prefetch_hook.c — il guscio dell'hook pre_agent. CAPITOLO 12.
 *
 * Il produttore del segnale [P1]: Vibe lo lancia a ogni turno,
 * PRIMA che il modello legga il messaggio, con un payload JSON su
 * stdin (prompt, transcript_path, turn_id). Lui guarda i pensieri
 * (il journal), predice col motore, parcheggia le predizioni nel
 * file di staging [P3] e inietta gli handle nel contesto
 * dell'agente via additional_context [P5].
 *
 * FAIL-OPEN SEMPRE: un prefetcher rotto non deve MAI fermare il
 * turno. Ogni errore e' silenzio: exit 0, stdout vuoto. Il costo
 * di un hook muto e' zero predizioni, non un turno bloccato.
 *
 * uso: prefetch_hook [db]     (default: memoria.db, relativo alla
 * cwd della sessione; lo staging e' derivato: <db>.prefetch)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "motore.h"
#include "prefetch.h"
#include "json.h"

#define PAYLOAD_MAX  (1024u * 1024u)

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
    motore_t *m = motore_apri_da(db, 1, NULL);   /* l'ultimo salva
                                                    [P9], orologio
                                                    fermo come la
                                                    memoria */
    if (!m) {
        json_libera(root);
        return 0;                       /* niente db: niente predizioni */
    }

    prefetch_predizione_t pred[PREFETCH_PREDIZIONI_MAX];
    size_t n = prefetch_predici(m, traccia, tlen, pred,
                                PREFETCH_PREDIZIONI_MAX);
    motore_chiudi(m);
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
