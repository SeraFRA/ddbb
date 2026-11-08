/*
 * motore.h — il motore unico del progetto: dieci file, un sistema.
 *
 * Il contratto completo (invarianti [I1]..[I10], obituario dei port
 * esclusi, decisioni) vive nell'header di motore.c: questa e' solo
 * la_interfaccia, cioe' le promesse che il resto del mondo puo'
 * pretendere. Handle OPACO: chi chiama non vede l'arena, le voci, le
 * firme, l'anello o il recinto — e per questo il motore puo' cambiare
 * geometria interna (vedi la compattazione che ha reso l'arena
 * ordinata) senza che il chiamante se ne accorga.
 *
 * LA PROMESSA CENTRALE (in stile "risposta viva"):
 *   motore_get     -> il valore corrente o NULL (mai bugie)
 *   motore_presente-> 1 "probabilmente" (bloom) / 0 "certamente no"
 *   motore_chiedi  -> la chiave E il valore, con la confidenza dichiarata
 *   motore_intervallo-> tutte le chiavi vive in [da, a] in ordine,
 *                      il lavoro che nessun indice a hash sa fare
 *
 * L'OROLOGIO E' DEL CHIAMANTE [I8]: niente orologio da muro dentro il
 * motore, mai. avanza() muove il tempo (e le scadenze), apri_da() lo
 * fissa alla nascita del ricaricato. Semantica TTL inclusiva: la entry
 * nata a t con ttl T e' morta quando adesso >= t + T.
 */
#ifndef MOTORE_H
#define MOTORE_H

#include <stddef.h>
#include <stdint.h>

enum {
    MOTORE_OK          =  0,
    MOTORE_ERR_MEMORIA = -1,
    MOTORE_ERR_PIENO   = -2,   /* arena al tetto di sicurezza        */
    MOTORE_ERR_MANCA   = -3,   /* chiave assente alla delete         */
    MOTORE_ERR_IO      = -4,
    MOTORE_ERR_FORMATO = -5,
    MOTORE_ERR_TTL     = -6    /* ttl oltre la finestra dell'anello  */
};

/* i bytes puntati vivono nell'arena del motore: validi fino alla
 * prossima put/compattazione (le realloc della verita' girano) */
typedef struct {
    const uint8_t *chiave;
    uint32_t       chiave_len;
    const uint8_t *valore;
    uint32_t       valore_len;
    double         confidenza;   /* 1.0 esatta, altrimenti Jaccard   */
} motore_risposta_t;

typedef struct motore motore_t;

motore_t *motore_apri(void);
void      motore_chiudi(motore_t *m);

int             motore_put(motore_t *m, const char *chiave, size_t klen,
                          const char *valore, size_t vlen, uint32_t ttl);
const uint8_t  *motore_get(motore_t *m, const char *chiave, size_t klen,
                           uint32_t *valore_len);
int             motore_del(motore_t *m, const char *chiave, size_t klen);

/* il tempo del motore [I8]: avanza il tick ASSOLUTO (non un delta) e
 * fa scadere in batch (timing wheel: O(1) ammortizzato per scadenza) */
void     motore_avanza(motore_t *m, uint32_t adesso);
uint32_t motore_adesso(const motore_t *m);

/* 1 = probabilmente c'e' (bloom: mai falso negativo sulle vive);
 * 0 = certamente non c'e' dall'ultima ricomposizione [I9] */
int motore_presente(motore_t *m, const char *chiave, size_t klen);

/* compatta: butta la spazzatura, ORDINA l'arena per chiave (il merge
 * che impara la geometria), riallena ogni derivato */
int motore_compatta(motore_t *m);

/* snapshot compattato, formato del decimo file (PDB1): i due mondi
 * si leggono a vicenda [I10] */
int       motore_salva(motore_t *m, const char *nome);
motore_t *motore_apri_da(const char *nome, uint32_t adesso,
                         size_t *scartate);

/* la domanda probabile: esatta -> 1 risposta a confidenza 1.0;
 * miss -> top-k semantico, ognuno col SUO valore [I6] */
size_t motore_chiedi(motore_t *m, const char *query, size_t qlen,
                     size_t k, motore_risposta_t risposte[],
                     size_t *n_candidati);

/* tutte le vive in [da, a]: parte ordinata in ordine di chiave, poi
 * le giovani in ordine di nascita; riempie risposte fino a k_max e
 * ritorna il numero TOTALE di corrispondenze (conta anche oltre il
 * cap). *passi_seek (se non NULL) riceve la spesa del recinto [I7] */
size_t motore_intervallo(motore_t *m, const char *da, size_t da_len,
                         const char *a, size_t a_len, size_t k_max,
                         motore_risposta_t risposte[], size_t *totali,
                         uint32_t *passi_seek);

/* l'invariante [I7] resa API: conta le vive camminando l'arena e le
 * confronta col contatore interno. Divergenza = contratto rotto */
size_t motore_audit(const motore_t *m, size_t *dichiarate);

void motore_statistiche(const motore_t *m, const char *etichetta);

#endif /* MOTORE_H */
