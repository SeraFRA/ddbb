/*
 * gateway.h — il gateway: la lavagna a un solo custode. CAPITOLO 14.
 *
 * LA METAFORE: il palazzo con un solo archivista. N agenti entrano
 * dalla porta (socket), ognuno col suo sportello; dietro il bancone
 * c'e' UNA arena viva, UN solo WAL, UN solo lucchetto. Cio' che
 * l'agente A scrive, l'agente B lo legge senza che nessuno abbia
 * salvato: la lavagna vive al contrario dello snapshot, che e' la
 * fotografia dei morti.
 *
 * LA STIRPE: Linda (1985) e Redis (2010) — un processo possiede i
 * dati, N clienti parlano al processo. Il prefetcher diventa il
 * meccanismo di CONTROLLO della lavagna (lo scheduler di Hearsay-II):
 * le predizioni parcheggiate da una traccia possono colpire con la
 * get di UN ALTRO agente — il passamano speculative.
 *
 * =====================================================================
 * IL CONTRATTO [L1]..[L6] (invariants-first: scritto prima del codice)
 * =====================================================================
 *  [L1] LA LAVAGNA E' L'ARENA VIVA. Un solo motore in un solo
 *       processo: lo stato che un cliente scrive e' visibile al
 *       prossimo get di UN ALTRO cliente, senza salva in mezzo.
 *       Consequences dichiarate: il WAL e' quello del padrone (le
 *       put di TUTTI si inchiostrano nel segmento unico [W1]); il
 *       salva di un cliente taglia il segmento PER TUTTI [W5] — la
 *       convenzione della memoria restaCLETA una: "salva dopo le
 *       scritture pesanti", di chiunque siano.
 *  [L2] UN SOLO CUSTODE [I11]. Il gateway E' il padrone: mcp_apri
 *       prende il lucchetto sul db, un secondo gateway (o server)
 *       sullo stesso file cade ad alta voce. N clienti != N padroni:
 *       i clienti non toccano il file, parlano al custode.
 *  [L3] IL FILO E' QUELLO DEL CAPITOLO 11. JSON-RPC 2.0, un oggetto
 *       per riga, stessi metodi, stesse risposte: mcp_gestisci NON
 *       cambia di una riga. Un cliente MCP esistente si connette e
 *       funziona.
 *  [L4] NESSUNO PARLA NEL TEMPO DEGLI ALTRI. Il custode gira a
 *       eventi (select): la riga di un cliente arriva a pezzi? resta
 *       nel SUO buffer finche' non e' intera; la risposta che il
 *       cliente non ha ancora letto? resta nella SUA coda di
 *       uscita. Un cliente lento consuma la sua coda, non il tempo
 *       degli altri. (I limiti veri — zero thread, il costo di ogni
 *       gira e' il piu' lento dei suoi pezzi — stanno in obitorio.)
 *  [L5] L'OROLOGIO E' IL GIRO DEL CUSTODE [I8]. Le richieste di
 *       TUTTI i clienti alimentano lo stesso contatore: e' l'orologio
 *       del parcheggio [P6] e la base del TTL contato in richieste.
 *       Conseguenza onesta misurata dal banco: il TTL di un cliente
 *       e' accorciato dal traffico degli altri — la lavagna e' un
 *       bene comune anche nel consumo.
 *  [L6] LA PORTA E' STRETTA. Bind su 127.0.0.1, porta scelta
 *      dall'OS, mai una porta in ascolto sul mondo: la lavagna di
 *       lavoro e' del processo e dei suoi figli, non di internet.
 *
 * =====================================================================
 * I LIMITI (dichiarati in testa, come sempre)
 * =====================================================================
 *  - zero thread: il giro serve TUTTI i pezzi pronti, poi ricomincia;
 *    una mcp_gestisci lenta (una risposta enorme) ritarda le altre:
 *    il costo e' dichiarato dal banco, la cura (un thread per
 *    cliente) sta in obitorio — e' la stessa rinuncia [W6];
 *  - il cliente che sparisce senza chiudere: il socket resta fino a
 *    errore di lettura (TCP decide lui); zero keepalive in v1;
 *  - il protocollo non ha AUTENTICAZIONE: chi arriva sulla porta
 *    parla col motore — per questo la porta e' solo su 127.0.0.1 [L6].
 */
#ifndef GATEWAY_H
#define GATEWAY_H

#include <stddef.h>
#include <stdint.h>

#include "mcp_server.h"

#define GATEWAY_CLIENTI_MAX 16
#define GATEWAY_RIGA_MAX   ((size_t)1 << 22)   /* il filo [M1]: 4 MiB  */

typedef struct gateway gateway_t;

/* apre la porta SOPRA un server MCP gia' padrone del suo db [L2]:
 * l'ownership del mcp_t resta al chiamante (il gateway non lo chiude).
 * Ritorna NULL se la porta non si apre. *porta riceve la porta TCP
 * scelta dall'OS [L6]. */
gateway_t *gateway_apri(mcp_t *s, uint32_t *porta);

/* chiude la porta e libera i buffer dei clienti (NON il mcp_t) */
void gateway_chiudi(gateway_t *g);

/* UN GIRO del custode [L4]: attende fino a timeout_ms, accetta i
 * nuovi, consuma le righe intere di ogni cliente (mcp_gestisci),
 * smaltisce le code di uscita. Ritorna quante richieste ha servito
 * in questo giro (0 = silenzio — anche il custode tace, a volte). */
size_t gateway_gira(gateway_t *g, int timeout_ms);

/* la MISURA: clienti collegati ora, clienti serviti in totale */
size_t gateway_clienti(const gateway_t *g);
size_t gateway_clienti_tot(const gateway_t *g);

/* --------------------------------------- il lato cliente (del banco e di
 * chi vuole provare la porta senza un client MCP): le stesse socket
 * portabili e la lettura A BLOCCHI (una recv per risposta, non una
 * per byte: la misura della porta e' del kernel, non della lentezza
 * di chi legge). BLOCCANTE sulla riga: chi chiede, aspetta. */
typedef struct {
    int    fd;                 /* -1 = non collegato                  */
    char  *buf;                /* il blocco ricevuto, non ancora letto */
    size_t cap, len, off;
} gateway_cliente_t;

int  gateway_cliente_apri(gateway_cliente_t *c, uint32_t porta);
void gateway_cliente_chiudi(gateway_cliente_t *c);
int  gateway_cliente_scrivi(gateway_cliente_t *c, const char *buf,
                            size_t len);
/* 1 = riga intera in "riga" (senza newline), 0 = porta chiusa,
 * -1 = errore o riga oltre cap */
int  gateway_cliente_leggi(gateway_cliente_t *c, char *riga, size_t cap);

#endif /* GATEWAY_H */
