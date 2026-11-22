/*
 * gateway_cliente.c — il cliente della lavagna. CAPITOLO 14, lato filo.
 *
 * Qui vive cio' che serve per PARLARE al custode senza esserlo: il
 * bootstrap dei socket portabili (WinSock su Windows, BSD su POSIX —
 * una fonte sola: il custode e i clienti passano da qui), la cura del
 * Nagle, il file-porta DERIVATO dal db (<db>.porta) e le quattro
 * funzioni del cliente (apri, chiudi, scrivi, leggi — bloccante
 * sulla riga: chi chiede, aspetta [L3]).
 *
 * PERCHE' UN FILE SUO [P10]: il produttore cliente della lavagna
 * (l'hook prefetch in modo "lavagna") linka QUESTO e non il custode —
 * altrimenti si trascinerebbe mcp_gestisci e il server intero dentro
 * un processo che gira a OGNI turno. Il cliente e' piccolo per
 * contratto: e' l'economia del capitolo 11 fatta file.
 *
 * POST MORTEM (il motivo del file): il primo tentativo linkava
 * gateway.c intero nell'hook — e collect2 riusciva solo a dire
 * "ld returned 1" SENZA stampare l'undefined reference a
 * mcp_gestisci: il banco inseguiva un fantasma di linker per ore.
 * Con ld chiamato direttamente la verita' e' uscita in una riga.
 * La cura non e' linkare piu' roba nell'hook: e' che il cliente non
 * porti con se' il custode.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "gateway.h"

#if defined(_WIN32)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wpedantic"
# include <winsock2.h>
# include <ws2tcpip.h>
# pragma GCC diagnostic pop
#else
# include <sys/types.h>
# include <sys/socket.h>
# include <sys/select.h>              /* select, fd_set: POSIX li sta qui */
# include <netinet/in.h>
# include <netinet/tcp.h>             /* TCP_NODELAY: la cura del Nagle */
# include <arpa/inet.h>
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
typedef int SOCKET;
# define INVALID_SOCKET (-1)
# define SOCKET_ERROR   (-1)
# define closesocket(fd) close((fd))
#endif

/* ------------------------------------------------- il lato piattaforma */

static int porte_aperte = 0;

int gateway_porta_init(void)
{
#if defined(_WIN32)
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
        return -1;
#endif
    porte_aperte++;
    return 0;
}

void gateway_porta_fini(void)
{
    if (porte_aperte > 0 && --porte_aperte == 0) {
#if defined(_WIN32)
        (void)WSACleanup();
#endif
    }
}

/* NAGLE E' IL NEMICO DEL FILO A RIGHE: il newline da un byte resta
 * nello stomaco del kernel (loopback Linux: delayed-ACK fino a 40ms)
 * e la risposta che dipende da lui muore li'. Il protocollo e'
 * request-response: si disattiva, come fa Redis. (Il post mortem e'
 * nel commit del capitolo 14: sui giras a tempo-zero il banco si
 * bloccava, su Windows il timing benigno lo nascondeva.) */
void gateway_niente_nagle(int fd)
{
    int uno = 1;
    (void)setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY,
                     (const char *)&uno, sizeof uno);
}

/* --------------------------------------- il file-porta, DERIVATO dal db
 * (un derivato, una funzione di derivazione sola): il guscio lo
 * scrive appena la porta e' aperta, i clienti del filo lo leggono.
 * Stale-port dichiarata: se il custode muore il file resta — chi
 * legge una porta morta fallisce e TACE (fail-open del cliente). */
void gateway_percorso_porta(const char *db, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (!db || !*db) {
        out[0] = '\0';                  /* senza db non c'e' derivato */
        return;
    }
    snprintf(out, cap, "%s.porta", db);
}

/* --------------------------------------- il lato cliente (del banco e di
 * chi vuole provare la porta senza un client MCP): lettura a BLOCCHI,
 * una recv per risposta — il prezzo della porta e' del kernel, non
 * della lentezza di chi legge */

int gateway_cliente_apri(gateway_cliente_t *c, uint32_t porta)
{
    if (!c)
        return -1;
    memset(c, 0, sizeof *c);
    c->fd = -1;
    if (gateway_porta_init() != 0)
        return -1;
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        gateway_porta_fini();
        return -1;
    }
    struct sockaddr_in mega;
    memset(&mega, 0, sizeof mega);
    mega.sin_family = AF_INET;
    mega.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    mega.sin_port = htons((unsigned short)porta);
    if (connect(fd, (struct sockaddr *)&mega, sizeof mega) != 0) {
        closesocket(fd);
        gateway_porta_fini();
        return -1;
    }
    gateway_niente_nagle((int)fd);   /* il cliente non trattiene il
                                        proprio newline [Nagle post mortem] */
    c->fd = (int)fd;
    c->cap = 8192;
    c->buf = malloc(c->cap);
    if (!c->buf) {
        closesocket(fd);
        gateway_porta_fini();
        c->fd = -1;
        return -1;
    }
    return 0;
}

void gateway_cliente_chiudi(gateway_cliente_t *c)
{
    if (!c || c->fd < 0)
        return;
    closesocket((SOCKET)c->fd);
    free(c->buf);
    c->buf = NULL;
    c->fd = -1;
    gateway_porta_fini();
}

int gateway_cliente_scrivi(gateway_cliente_t *c, const char *buf,
                           size_t len)
{
    if (!c || c->fd < 0)
        return -1;
    size_t scritti = 0;
    while (scritti < len) {
        int n = (int)send((SOCKET)c->fd, buf + scritti,
                          (int)(len - scritti), 0);
        if (n == SOCKET_ERROR)
            return -1;
        scritti += (size_t)n;
    }
    return 0;
}

/* la riga gia' nel blocco ricevuto? si. Altrimenti UNA recv per
 * blocco: la syscall e' per il blocco, non per il byte */
int gateway_cliente_leggi(gateway_cliente_t *c, char *riga, size_t cap)
{
    if (!c || !riga || cap == 0)
        return -1;
    for (;;) {
        /* c'e' una riga intera nel blocco gia' letto? */
        for (size_t p = c->off; p < c->len; p++) {
            if (c->buf[p] != '\n')
                continue;
            size_t l = p - c->off;
            if (l + 1 > cap) {
                /* POST MORTEM (gialli della review, 22-11): il -1
                 * tornava SENZA avanzare il cursore, e la stessa riga
                 * gigante restava li' per sempre: errore permanente.
                 * La riga oltre il cap si BUTTA (il cursore scavalca
                 * il suo newline) e lo si dichiara una volta sola;
                 * il filo riprende dalla riga dopo.               */
                c->off = p + 1;
                if (c->off == c->len)
                    c->off = c->len = 0;
                return -1;
            }
            if (l > 0 && c->buf[p - 1] == '\r')
                l--;
            memcpy(riga, c->buf + c->off, l);
            riga[l] = '\0';
            c->off = p + 1;
            if (c->off == c->len)
                c->off = c->len = 0;    /* blocco consumato intero    */
            return 1;
        }
        int letti = (int)recv((SOCKET)c->fd, c->buf + c->len,
                              (int)(c->cap - c->len - 1), 0);
        if (letti == SOCKET_ERROR || letti == 0)
            return (letti == 0) ? 0 : -1;
        if (c->len + (size_t)letti + 2 > c->cap) {
            char *nuovo = realloc(c->buf, c->cap * 2);
            if (!nuovo)
                return -1;
            c->buf = nuovo;
            c->cap *= 2;
        }
        c->len += (size_t)letti;
        c->buf[c->len] = '\0';
    }
}
