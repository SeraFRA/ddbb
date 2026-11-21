/*
 * mcp_gateway.c — il guscio TCP del gateway: la lavagna in ascolto.
 *
 * Guscio-2 del layer [L3]: dove mcp_stdio.c parlava con stdin/stdout,
 * questo parla con una porta TCP su 127.0.0.1 [L6] — e il nucleo e'
 * lo stesso: mcp_apri una volta sola (il custode e' IL padrone
 * [I11][L2]), gateway_gira per sempre.
 *
 * uso: mcp_gateway [file.db [tick_iniziale]]
 *   senza argomenti: motore vuoto, orologio a 0 [I8] (una lavagna
 *                    senza memoria: per le prove)
 *   con file.db:     il padrone del db: chi arriva secondo cade ad
 *                    alta voce [I11], il messaggio dice chi tiene la
 *                    porta
 *
 * STDOUT: una riga "gateway: 127.0.0.1:<porta> db: <file>" appena la
 * porta e' aperta (un cliente la aspetta li'); tutto il resto e'
 * diagnostica su stderr. Il processo vive finche' non lo si ferma.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mcp_server.h"
#include "gateway.h"

int main(int argc, char **argv)
{
    uint32_t adesso = 0;
    if (argc >= 3) {
        unsigned long t = strtoul(argv[2], NULL, 10);
        if (t > 4294967295ul) {
            fprintf(stderr, "mcp_gateway: tick oltre 32 bit\n");
            return 1;
        }
        adesso = (uint32_t)t;
    }

    /* il custode prende possesso: il LUCCHETTO viene prima di tutto
     * [I11][L2] — un secondo padrone qui non esiste, esiste un'uscita
     * ad alta voce */
    mcp_t *s = mcp_apri(argc >= 2 ? argv[1] : NULL, adesso, NULL);
    if (!s) {
        fprintf(stderr, "mcp_gateway: memoria, file non leggibile, o"
                        " db gia' posseduto da un altro padrone [I11]\n");
        return 1;
    }

    uint32_t porta = 0;
    gateway_t *g = gateway_apri(s, &porta);
    if (!g) {
        fprintf(stderr, "mcp_gateway: la porta non si apre\n");
        mcp_chiudi(s);
        return 1;
    }
    printf("gateway: 127.0.0.1:%u db: %s\n", porta,
           (argc >= 2) ? argv[1] : "(memoria)");
    fflush(stdout);                       /* il cliente aspetta qui' */

    uint64_t giri_muti = 0;
    for (;;) {
        size_t servite = gateway_gira(g, 1000);
        if (servite == 0) {
            giri_muti++;                  /* il custode tace, a volte */
            if (giri_muti == 60) {
                fprintf(stderr, "[gateway] vivo, %zu clienti,"
                                " %zu serviti in totale\n",
                        gateway_clienti(g), gateway_clienti_tot(g));
                giri_muti = 0;
            }
            continue;
        }
        giri_muti = 0;
    }
    /* non si arriva: la lavagna vive finche' non la si ferma */
}
