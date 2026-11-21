/*
 * gateway.c — il custode: la lavagna a un solo padrone. CAPITOLO 14.
 *
 * L'implementazione del contratto [L1]..[L6] di gateway.h. Il nucleo
 * non ha Un'opinione: le righe intere vanno a mcp_gestisci (che non
 * cambia di una riga [L3]), le risposte tornano per la stessa porta.
 * Qui vive solo la MECANICA DELLA PORTA: socket portabili (WinSock
 * su Windows, BSD su POSIX — una fonte sola: il banco e i clienti
 * passano da queste funzioni), righe a pezzi che aspettano di essere
 * intere nel buffer del loro cliente [L4], risposte che aspettano
 * il loro cliente nella loro coda di uscita.
 *
 * POST MORTEM DELLA PUTTANATA EVITATA (scritto perche' quasi fatta):
 * il primo abbozzo leggeva col recv BLOCCANTE dal cliente che
 * arrivava prima: un cliente muto congelava il custode e la lavagna
 * intera. No: la lettura e' NON bloccante e gira a eventi — la
 * lentezza di uno resta nel suo buffer [L4], e' il contratto.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

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

/* il bootstrap dei socket (init/fini) e il TCP_NODELAY vivono nel
 * FILE DEL CLIENTE (gateway_cliente.c): il custode li prende da li'.
 * Il motivo [P10]: l'hook prefetch cliente della lavagna linka il
 * cliente, NON il custode — altrimenti si trascinerebbe
 * mcp_gestisci e il server intero dentro un processo che gira a
 * ogni turno. */

/* il socket non blocca: leggere quando non c'e' niente e' riprovare
 * dopo, non addormentarsi con la lavagna in mano [L4] */
static int sblocca(SOCKET fd)
{
#if defined(_WIN32)
    u_long uno = 1;
    return ioctlsocket(fd, FIONBIO, &uno) == 0 ? 0 : -1;
#else
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0)
        return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

static int e_di_nuovo_piu_tardi(void)
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* NAGLE: la cura vive nel file del cliente (gateway_niente_nagle):
 * stessa pillola per il socket accettato e per chi parte. */

/* --------------------------------------------------------- i clienti */

struct gateway_conn {
    SOCKET   fd;                       /* INVALID = slot libero     */
    char    *in;                       /* la riga in costruzione    */
    size_t   in_cap, in_len;
    char    *out;                      /* la coda di uscita [L4]    */
    size_t   out_cap, out_off, out_len;
};

struct gateway {
    mcp_t             *mo;
    SOCKET             ascolto;
    struct gateway_conn conn[GATEWAY_CLIENTI_MAX];
    size_t             clienti_tot;
};

static void conn_a_vuoto(struct gateway_conn *c)
{
    if (c->fd != INVALID_SOCKET)
        closesocket(c->fd);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = INVALID_SOCKET;
}

/* ------------------------------------------------------------ il giro */

static int coda_prendi(struct gateway_conn *c, const char *piu, size_t n)
{
    if (c->out_len + n > c->out_cap) {
        size_t nuova = c->out_cap ? c->out_cap * 2 : 4096;
        while (nuova < c->out_len + n)
            nuova *= 2;
        char *buf = realloc(c->out, nuova);
        if (!buf)
            return -1;
        c->out = buf;
        c->out_cap = nuova;
    }
    memcpy(c->out + c->out_len, piu, n);
    c->out_len += n;
    return 0;
}

/* una riga intera del cliente -> il filo del cap. 11 [L3] */
static size_t serva_riga(gateway_t *g, struct gateway_conn *c,
                         const char *riga, size_t len)
{
    size_t rl = 0;
    char *risposta = NULL;
    if (mcp_gestisci(g->mo, riga, len, &risposta, &rl) != 0) {
        fprintf(stderr, "[gateway] memoria del filo: richiesta persa"
                        " (dichiarato, non taciuto)\n");
        return 0;
    }
    if (risposta) {
        /* il filo parla A RIGHE: la risposta porta il suo newline, o
         * il cliente non la riconosce mai come oggetto finito [M1] */
        if (coda_prendi(c, risposta, rl) == 0 &&
            coda_prendi(c, "\n", 1) == 0)
            ;
        else
            fprintf(stderr, "[gateway] coda di uscita esausta:"
                            " risposta persa\n");
        free(risposta);
        return 1;
    }
    return 0;                           /* notifica: la risposta e'
                                           il silenzio [M1]         */
}

size_t gateway_gira(gateway_t *g, int timeout_ms)
{
    if (!g)
        return 0;
    fd_set leggibili, scrivibili;
    FD_ZERO(&leggibili);
    FD_ZERO(&scrivibili);
    SOCKET massimo = g->ascolto;
    FD_SET(g->ascolto, &leggibili);
    for (int i = 0; i < GATEWAY_CLIENTI_MAX; i++) {
        struct gateway_conn *c = &g->conn[i];
        if (c->fd == INVALID_SOCKET)
            continue;
        FD_SET(c->fd, &leggibili);      /* c'e' sempre da leggere    */
        if (c->out_off < c->out_len)
            FD_SET(c->fd, &scrivibili); /* la coda vuole andare      */
        if (c->fd > massimo)
            massimo = c->fd;
    }
    struct timeval tv;
    struct timeval *attesa = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        attesa = &tv;
    }
    int rselect = select((int)massimo + 1, &leggibili, &scrivibili, NULL,
                         attesa);
    if (rselect == SOCKET_ERROR)
        return 0;                       /* un giro così; si torna [L4] */

    size_t servite = 0;

    /* la porta: un nuovo cliente prende uno slot, il diciassettesimo
     * si richiude da solo (dichiarato in gateway.h) */
    if (FD_ISSET(g->ascolto, &leggibili)) {
        SOCKET nuovo = accept(g->ascolto, NULL, NULL);
        if (nuovo != INVALID_SOCKET) {
            int buco = -1;
            for (int i = 0; i < GATEWAY_CLIENTI_MAX && buco < 0; i++)
                if (g->conn[i].fd == INVALID_SOCKET)
                    buco = i;
            if (buco < 0 || sblocca(nuovo) != 0) {
                closesocket(nuovo);
            } else {
                gateway_niente_nagle((int)nuovo);
                memset(&g->conn[buco], 0, sizeof g->conn[buco]);
                g->conn[buco].fd = nuovo;
                g->conn[buco].in_cap = 4096;
                g->conn[buco].in = malloc(g->conn[buco].in_cap);
                if (!g->conn[buco].in)
                    conn_a_vuoto(&g->conn[buco]);
                else
                    g->clienti_tot++;
            }
        }
    }

    for (int i = 0; i < GATEWAY_CLIENTI_MAX; i++) {
        struct gateway_conn *c = &g->conn[i];
        if (c->fd == INVALID_SOCKET)
            continue;
        if (FD_ISSET(c->fd, &leggibili)) {
            for (;;) {
                if (c->in_len + 1 >= c->in_cap) {
                    if (c->in_cap >= GATEWAY_RIGA_MAX) {
                        /* il filo non e' un romanzo [M1]: il cliente
                         * che non finisce una riga se ne va */
                        fprintf(stderr, "[gateway] riga oltre %zu byte:"
                                        " cliente espulso\n",
                                GATEWAY_RIGA_MAX);
                        conn_a_vuoto(c);
                        break;
                    }
                    char *buf = realloc(c->in, c->in_cap * 2);
                    if (!buf) {
                        conn_a_vuoto(c);
                        break;
                    }
                    c->in = buf;
                    c->in_cap *= 2;
                }
                int letti = (int)recv(c->fd, c->in + c->in_len,
                                       (int)(c->in_cap - c->in_len - 1), 0);
                if (letti == SOCKET_ERROR) {
                    if (e_di_nuovo_piu_tardi())
                        break;          /* per ora e' tutto [L4]      */
                    conn_a_vuoto(c);      /* rotto: via lui, non tutti */
                    break;
                }
                if (letti == 0) {       /* il cliente ha salutato    */
                    conn_a_vuoto(c);
                    break;
                }
                c->in_len += (size_t)letti;
                c->in[c->in_len] = '\0';
                /* le righe intere che c'erano gia' dentro: servite e
                 * compattate — la coda del cliente e' SUA [L4] */
                size_t inizio = 0;
                for (size_t p = 0; p < c->in_len; p++) {
                    if (c->in[p] != '\n')
                        continue;
                    if (p > inizio && c->in[p - 1] == '\r')
                        c->in[p - 1] = '\0';
                    else
                        c->in[p] = '\0';
                    const char *riga = c->in + inizio;
                    if (*riga)         /* la riga vuota e' rumore    */
                        servite += serva_riga(g, c, riga, strlen(riga));
                    inizio = p + 1;
                }
                if (inizio > 0) {
                    memmove(c->in, c->in + inizio, c->in_len - inizio + 1);
                    c->in_len -= inizio;
                }
            }
        }
        if (c->fd == INVALID_SOCKET)
            continue;                   /* espulso mentre leggevamo  */
        if (FD_ISSET(c->fd, &scrivibili) || c->out_off < c->out_len) {
            for (;;) {
                size_t resto = c->out_len - c->out_off;
                if (resto == 0)
                    break;
                int scritti = (int)send(c->fd, c->out + c->out_off,
                                        (int)resto, 0);
                if (scritti == SOCKET_ERROR) {
                    if (e_di_nuovo_piu_tardi())
                        break;          /* il cliente non legge: la
                                           coda aspetta LUI [L4]     */
                    conn_a_vuoto(c);
                    break;
                }
                c->out_off += (size_t)scritti;
                if (c->out_off == c->out_len)
                    c->out_off = c->out_len = 0;   /* coda vuota, via */
            }
        }
    }
    return servite;
}

size_t gateway_clienti(const gateway_t *g)
{
    if (!g)
        return 0;
    size_t n = 0;
    for (int i = 0; i < GATEWAY_CLIENTI_MAX; i++)
        if (g->conn[i].fd != INVALID_SOCKET)
            n++;
    return n;
}

size_t gateway_clienti_tot(const gateway_t *g)
{
    return g ? g->clienti_tot : 0;
}

/* ------------------------------------------------------------ la porta */

gateway_t *gateway_apri(mcp_t *s, uint32_t *porta)
{
    if (!s)
        return NULL;
    if (gateway_porta_init() != 0)
        return NULL;
    gateway_t *g = calloc(1, sizeof *g);
    if (!g) {
        gateway_porta_fini();
        return NULL;
    }
    g->mo = s;
    for (int i = 0; i < GATEWAY_CLIENTI_MAX; i++)
        g->conn[i].fd = INVALID_SOCKET;

    g->ascolto = socket(AF_INET, SOCK_STREAM, 0);
    if (g->ascolto == INVALID_SOCKET) {
        free(g);
        gateway_porta_fini();
        return NULL;
    }
    struct sockaddr_in mega;
    memset(&mega, 0, sizeof mega);
    mega.sin_family = AF_INET;
    mega.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* porta stretta [L6] */
    mega.sin_port = 0;                               /* l'OS sceglie      */
    if (bind(g->ascolto, (struct sockaddr *)&mega, sizeof mega) != 0 ||
        listen(g->ascolto, 8) != 0 || sblocca(g->ascolto) != 0) {
        closesocket(g->ascolto);
        free(g);
        gateway_porta_fini();
        return NULL;
    }
    socklen_t l = sizeof mega;
    if (getsockname(g->ascolto, (struct sockaddr *)&mega, &l) != 0) {
        closesocket(g->ascolto);
        free(g);
        gateway_porta_fini();
        return NULL;
    }
    if (porta)
        *porta = (uint32_t)ntohs(mega.sin_port);
    return g;
}

void gateway_chiudi(gateway_t *g)
{
    if (!g)
        return;
    for (int i = 0; i < GATEWAY_CLIENTI_MAX; i++)
        if (g->conn[i].fd != INVALID_SOCKET)
            conn_a_vuoto(&g->conn[i]);
    closesocket(g->ascolto);
    free(g);
    gateway_porta_fini();
}

