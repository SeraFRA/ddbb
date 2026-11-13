/*
 * json.h — il parser/serializzatore JSON minimale del layer MCP.
 *
 * Perche' esiste come file suo: e' l'UNICA fonte di verita' sul filo
 * (regola del repo: un derivato, una funzione di derivazione sola) —
 * la usa il server per leggere le richieste e il banco di prova per
 * verificare le risposte: se il banco usasse un secondo parser,
 * verificherebbe se stesso, non il server.
 *
 * I LIMITI (dichiarati in testa, non scoperti in produzione):
 *  - profondita' massima di annidamento: 64 (oltre: rifiuto, non stack
 *    overflow: un input ostile e' un caso del filo);
 *  - i numeri viaggiano come double: fino a 2^53 l'intero e' esatto,
 *    oltre si perde — i tick e le lunghezze di questo progetto stanno
 *    ben dentro;
 *  - le stringhe in ingresso con escape \\uXXXX diventano UTF-8 (le
 *    surrogate spaiate diventano U+FFFD: dichiarato, non nascosto);
 *  - in uscita i byte >= 0x80 passano dritti: si ASSUME che chiave e
 *    valore siano gia' UTF-8 (il motore non lo garantisce: il filo
 *    MCP e' per payload di testo, non per binari).
 */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtipo_t;

typedef struct jval jval_t;
struct jval {
    jtipo_t  tipo;
    int      booleano;   /* J_BOOL                                  */
    double   num;        /* J_NUM                                   */
    char    *str;        /* J_STR: malloc, NUL-terminata            */
    size_t   str_len;
    jval_t **figli;      /* J_ARR / J_OBJ: elementi o valori        */
    char   **chiavi;     /* J_OBJ: malloc, NUL-terminate            */
    size_t   n;          /* elementi / membri                       */
    size_t   cap;        /* capacita' interna dei vettori          */
};

/* parse: NULL = errore. Il buffer non serve NUL-terminato: non si
 * legge mai oltre len (i numeri sono copiati prima di strtod). */
jval_t *json_parse(const char *s, size_t len);
void    json_libera(jval_t *v);

/* accesso: NULL / 0 se il campo manca o il tipo non e' quello chiesto */
const jval_t *json_get(const jval_t *oggetto, const char *chiave);
const char   *json_str(const jval_t *v, size_t *len);
double        json_num(const jval_t *v, int *valido);

/* serializzazione: un buffer che cresce. "rotto" e' la morte atomica
 * del buffer: la prima realloc fallita lo marca, e da li' ogni
 * jbuf_* e' un no-op — chi risponde controlla UNA volta, in fondo. */
typedef struct {
    char  *p;
    size_t n, cap;
    int    rotto;
} jbuf_t;

int  jbuf_init(jbuf_t *b);
void jbuf_libera(jbuf_t *b);
int  jbuf_add(jbuf_t *b, const char *s, size_t len);
int  jbuf_testo(jbuf_t *b, const char *s, size_t len);  /* con escape */
int  jbuf_num(jbuf_t *b, double v);   /* intero se finito, %.17g altrimenti */
int  json_scrivi(jbuf_t *b, const jval_t *v);            /* ricorsivo      */

/* la lunghezza di un letterale la conta il compilatore, non la mano:
 * contate a mano in questo repo hanno gia' prodotto un byte fantasma
 * dentro una risposta (post mortem del capitolo 11). */
#define JBUF_LIT(b, lit) jbuf_add((b), (lit), sizeof(lit) - 1)

#endif /* JSON_H */
