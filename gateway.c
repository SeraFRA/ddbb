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

/* ------------------------------------------------- il lato piattaforma */

static int porte_aperte = 0;

static int porta_init(void)
{
#if defined(_WIN32)
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
        return -1;
#endif
    porte_aperte++;
    return 0;
}

static void porta_fini(void)
{
    if (porte_aperte > 0 && --porte_aperte == 0) {
#if defined(_WIN32)
        (void)WSACleanup();
#endif
    }
}

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

/* NAGLE E' IL NEMICO DEL FILO A RIGHE: il newline da un byte resta
 * nello stomaco del kernel (loopback Linux: delayed-ACK fino a 40ms)
 * e la risposta che dipende da lui muore li'. Il protocollo e'
 * request-response: si disattiva, come fa Redis. (Il post mortem e'
 * nel commit: sui giras a tempo-zero il banco si bloccava, su
 * Windows il timing benigno lo nascondeva.) */
static void niente_nagle(SOCKET fd)
{
    int uno = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     (const char *)&uno, sizeof uno);
}

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
                niente_nagle(nuovo);
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
    if (porta_init() != 0)
        return NULL;
    gateway_t *g = calloc(1, sizeof *g);
    if (!g) {
        porta_fini();
        return NULL;
    }
    g->mo = s;
    for (int i = 0; i < GATEWAY_CLIENTI_MAX; i++)
        g->conn[i].fd = INVALID_SOCKET;

    g->ascolto = socket(AF_INET, SOCK_STREAM, 0);
    if (g->ascolto == INVALID_SOCKET) {
        free(g);
        porta_fini();
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
        porta_fini();
        return NULL;
    }
    socklen_t l = sizeof mega;
    if (getsockname(g->ascolto, (struct sockaddr *)&mega, &l) != 0) {
        closesocket(g->ascolto);
        free(g);
        porta_fini();
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
    porta_fini();
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
    if (porta_init() != 0)
        return -1;
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        porta_fini();
        return -1;
    }
    struct sockaddr_in mega;
    memset(&mega, 0, sizeof mega);
    mega.sin_family = AF_INET;
    mega.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    mega.sin_port = htons((unsigned short)porta);
    if (connect(fd, (struct sockaddr *)&mega, sizeof mega) != 0) {
        closesocket(fd);
        porta_fini();
        return -1;
    }
    niente_nagle(fd);                 /* il cliente non trattiene il
                                         proprio newline [Nagle post mortem] */
    c->fd = (int)fd;
    c->cap = 8192;
    c->buf = malloc(c->cap);
    if (!c->buf) {
        closesocket(fd);
        porta_fini();
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
    porta_fini();
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
            if (l + 1 > cap)
                return -1;              /* la riga oltre il cap       */
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
