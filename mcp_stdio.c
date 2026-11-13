/*
 * mcp_stdio.c — il guscio sottile del layer MCP [M9].
 *
 * Il canale: JSON-RPC 2.0, un oggetto per riga, stdin -> stdout.
 * Tutto il resto (i pensieri del motore, i caricamenti, gli errori
 * del guscio) va su STDERR: stdout e' il filo del protocollo, un
 * byte di diagnosi li' dentro e' un client avvelenato.
 *
 * Uso: mcp_server [file.db [tick_iniziale]]
 *   senza argomenti: motore vuoto, orologio a 0 [I8]
 *   con file.db:     caricato col validatore PDB1 [I10], scartate
 *                    dichiarate su stderr [I5]
 *
 * I LIMITI (dichiarati in testa): una riga oltre 4 MiB e' rifiutata
 * con uscita 1 (il filo e' per richieste, non per caricare romanzi);
 * il \r di chiusura e' tollerato e scartato (i client Windows esistono).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mcp_server.h"

#define RIGA_MAX ((size_t)1 << 22)   /* 4 MiB: il filo non e' un romanzo */

/* 1 = riga letta, 0 = EOF pulito, -1 = riga oltre il tetto o memoria */
static int leggi_riga(char **buf, size_t *cap, size_t *len)
{
    *len = 0;
    for (;;) {
        if (*len + 2 > *cap) {
            if (*cap >= RIGA_MAX)
                return -1;
            *cap *= 2;
            char *nuovo = realloc(*buf, *cap);
            if (!nuovo)
                return -1;
            *buf = nuovo;
        }
        if (!fgets(*buf + *len, (int)(*cap - *len), stdin)) {
            if (*len == 0)
                return 0;               /* EOF: nessuna riga parziale  */
            break;                       /* ultima riga senza newline  */
        }
        size_t letti = strlen(*buf + *len);
        *len += letti;
        if (letti > 0 && (*buf)[*len - 1] == '\n') {
            (*len)--;                    /* il newline non e' JSON     */
            (*buf)[*len] = '\0';
            break;
        }
    }
    if (*len > 0 && (*buf)[*len - 1] == '\r')
        (*buf)[--(*len)] = '\0';         /* client Windows: tollerato  */
    return 1;
}

int main(int argc, char **argv)
{
    uint32_t adesso = 0;
    if (argc >= 3) {
        unsigned long t = strtoul(argv[2], NULL, 10);
        if (t > 4294967295ul) {
            fprintf(stderr, "mcp_stdio: tick oltre 32 bit\n");
            return 1;
        }
        adesso = (uint32_t)t;
    }

    size_t scartate = 0;
    mcp_t *s = mcp_apri(argc >= 2 ? argv[1] : NULL, adesso, &scartate);
    if (!s) {
        fprintf(stderr, "mcp_stdio: memoria, o file non leggibile\n");
        return 1;
    }
    if (argc >= 2 && scartate > 0)
        fprintf(stderr, "[mcp_stdio] %zu entry scartate al caricamento "
                        "[I5]\n", scartate);

    size_t cap = 1 << 16;
    char *riga = malloc(cap);
    if (!riga) {
        fprintf(stderr, "mcp_stdio: memoria\n");
        return 1;
    }

    for (;;) {
        size_t len = 0;
        int r = leggi_riga(&riga, &cap, &len);
        if (r < 0) {
            fprintf(stderr, "mcp_stdio: riga oltre %zu byte o memoria\n",
                    RIGA_MAX);
            return 1;
        }
        if (r == 0)
            break;                       /* EOF: il server tace ed esce */
        if (len == 0)
            continue;                    /* riga vuota: rumore, ignora   */

        char *risposta = NULL;
        size_t risposta_len = 0;
        if (mcp_gestisci(s, riga, len, &risposta, &risposta_len) != 0) {
            fprintf(stderr, "mcp_stdio: memoria esaurita\n");
            return 1;
        }
        if (risposta) {
            fwrite(risposta, 1, risposta_len, stdout);
            fputc('\n', stdout);
            fflush(stdout);              /* il client non aspetta un buffer */
            free(risposta);
        }
    }

    free(riga);
    mcp_chiudi(s);
    return 0;
}
