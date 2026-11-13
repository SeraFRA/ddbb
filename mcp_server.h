/*
 * mcp_server.h — il layer MCP: il motore come strumento (capitolo 11).
 *
 * L'interfaccia pubblica e' volutamente piccola: un server (handle
 * opaco, come il motore), un modo di aprirlo (vuoto o da file PDB1),
 * e UNA funzione che del filo e' l'unica fonte di verita':
 *
 *   mcp_gestisci(richiesta) -> risposta
 *
 * Il guscio stdio (mcp_stdio.c) e' un adattatore sottile; il banco di
 * prova (demo_mcp.c) chiama il nucleo in process: i gate non parlano
 * con pipe. La risposta e' JSON-RPC 2.0 senza newline; *risposta e'
 * NULL quando la richiesta era una notifica (nessuna risposta e' la
 * risposta giusta). Il buffer va liberato con free().
 */
#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct mcp mcp_t;

/* percorso_db NULL = server vuoto; altrimenti formato PDB1 [I10].
 * adesso fissa l'orologio alla nascita [I8]. */
mcp_t *mcp_apri(const char *percorso_db, uint32_t adesso, size_t *scartate);
void   mcp_chiudi(mcp_t *s);

/* una richiesta JSON (NUL-terminata, len = byte utili) -> una
 * risposta JSON (malloc, NUL-terminata) o NULL se notifica.
 * ritorna 0, o -1 su memoria esaurita (nessuna risposta). */
int mcp_gestisci(mcp_t *s, const char *richiesta, size_t len,
                 char **risposta, size_t *risposta_len);

/* i contatori della MISURA: quante richieste viste, quanti byte di
 * payload spediti all'agente (cioe' i token che l'LLM paga). */
void mcp_contatori(const mcp_t *s, uint64_t *richieste,
                   uint64_t *byte_payload);

#endif /* MCP_SERVER_H */
