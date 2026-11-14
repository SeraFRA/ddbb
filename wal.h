/*
 * wal.h — il write-ahead log (capitolo 13): la memoria che sopravvive
 * al crash fra due snapshot.
 *
 * LA METAORA: il quaderno di bordo. La nave puo' affondare a ogni
 * ondata (il processo muore quando vuole), ma il diario si scrive
 * riga per riga e ogni riga e' INCHIOSTRATA prima di passare alla
 * successiva: chi rilegge il diario ricostruisce tutto il viaggio
 * fino all'ultima riga leggibile. Nel mondo reale si chiama
 * write-ahead log (Bitcask, Postgres, SQLite): le intenzioni si
 * appendono in ordine PRIMA di toccare lo stato, e il replay le
 * ri-applica in ordine dopo il crash.
 *
 * IL DOLORE CHE CURA (misurato nel quaderno del capitolo 13): ogni
 * salva riscrive il file intero (64 ms a 50k chiavi) e fra due salva
 * tutto vive solo in arena: un crash perde tutto dall'ultimo snapshot.
 * Il WAL vive in mezzo: append O(1) per put, replay solo alla
 * riapertura, taglio a ogni salva.
 *
 * IL CONTRATTO (scritto prima del codice, come sempre qui):
 *  [W1] IL LOG VIENE PRIMA. E' un write-AHEAD log: chi compone il
 *       layer scrive il record PRIMA di toccare il motore. Un crash
 *       fra i due ri-applica al replay (il put e' idempotente); il
 *       contrario perderebbe la put che il motore ha gia' accettato.
 *  [W2] IL TRITTICO DELLA DURABILITA': fwrite + fflush + fsync per
 *       record. fsync senza fflush sincronizza un kernel che non ha
 *       ancora i byte (vivi nel buffer del processo): durabilita'
 *       bugiarda. Il costo e' misurato da misura_fsync: ~216 us per
 *       record su NVMe Windows — la decisione (2) del quaderno,
 *       chiusa coi numeri: al ritmo dell'agente (poche put per giro,
 *       il giro costa secondi di LLM) e' invisibile, e il group
 *       commit resta fuori perche' il suo batch non si riempie MAI
 *       a quel ritmo (degenererebbe in "fsync solo a salva").
 *  [W3] IL PREFISSO E' SICURO (il contratto del capitolo 4, [I5]):
 *       CRC-32 per record; un record rotto (lunghezza bugiarda o
 *       CRC) ferma il replay LI'. Si perde solo la coda non ancora
 *       inchiostrata — mai il segmento. La coda non valida si
 *       conta, si dichiara e si TRONCA: un record nuovo non finisce
 *       mai dietro una coda che il replay non leggerebbe mai.
 *  [W4] IL TICK STA NEL RECORD. Il replay deve ricostruire il nato
 *       di ogni entry (le TTL sono un contratto [I8]): il tick
 *       registrato avanza l'orologio del motore (monotono per
 *       costruzione) PRIMA della put. Limite dichiarato: se il
 *       chiamante carica lo snapshot a un tick SOPRA i record del
 *       WAL, le TTL replayate si allungano (nato troppo tardo) —
 *       il tick dentro il file PDB1 e' formato v2, e sta in
 *       obitorio.
 *  [W5] SI TAGLIA DOPO LO SNAPSHOT, MAI PRIMA. Il taglio azzera il
 *       log a ogni salva: il WAL vive solo fra due snapshot. Un
 *       crash fra snapshot e taglio ri-applica l'ultimo batch
 *       (idempotente a orologio fermo); il contrario perderebbe il
 *       batch con uno snapshot vecchio. Il DEL di una chiave
 *       assente al replay e' legale: puo' essere scaduta per conto
 *       suo.
 *  [W6] UNO SCRITTORE. Single-thread per contratto, come il motore:
 *       la concorrenza sta in obitorio (un estremo per volta).
 *
 * IL FORMATO DEL RECORD (21 byte di header, poi i byte):
 *   0   u32 klen
 *   4   u32 vlen        (0 per il DEL: il tombstone e' dedicato)
 *   8   u8  op          (1 = PUT, 2 = DEL)
 *   9   u32 ttl
 *   13  u32 tick        (l'orologio del chiamante, [W4])
 *   17  u32 crc32       (su byte 0..16 + chiave + valore)
 *   21  chiave (klen) + valore (vlen)
 *
 * OBITUARIO DEL CAPITOLO:
 *  - GROUP COMMIT: ammortizza 30x ma solo a batch pieni (misura_fsync
 *    D); al ritmo dell'agente il batch da 32 non si riempie in una
 *    sessione intera — degenerebbe in "fsync solo a salva", cioe' in
 *    non-WAL. Escluso col numero, non col gusto.
 *  - CHECKSUM PER SEGMENTO: un segmento rotto perderebbe tutto il
 *    segmento — incoerente col capitolo 4 (CRC per entry).
 *  - WAL COMPLESSO (LZ4, framing a blocchi): il replay deve essere
 *    deterministico e leggibile a meta' file; la compressione
 *    comprerebbe byte che non sono il collo (il collo e' il fsync).
 *  - fdatasync: un log append-only cambia la dimensione del file a
 *    ogni record, e la dimensione E' metadata — non comprerebbe
 *    niente. fsync e' la risposta onesta per un log che cresce.
 */
#ifndef WAL_H
#define WAL_H

#include <stddef.h>
#include <stdint.h>

#include "motore.h"

enum {
    WAL_OK          =  0,
    WAL_ERR_IO      = -1,
    WAL_ERR_MEMORIA = -2,
    WAL_ERR_FORMATO = -3    /* chiave o valore oltre i 4 GB di header */
};

/* le intenzioni */
enum {
    WAL_OP_PUT = 1,
    WAL_OP_DEL = 2
};

typedef struct wal wal_t;

/* apre (o crea) il segmento in append; NULL = errore di I/O o memoria */
wal_t *wal_apri(const char *percorso);
void   wal_chiudi(wal_t *w);

/* il trittico [W2]: fwrite + fflush + fsync, il record e' inchiostrato
 * quando tornano. Il tick e' del chiamante [W4] */
int wal_put(wal_t *w, const char *chiave, size_t klen,
            const char *valore, size_t vlen, uint32_t ttl, uint32_t tick);
int wal_del(wal_t *w, const char *chiave, size_t klen, uint32_t tick);

/* il taglio [W5]: azzero il segmento DOPO lo snapshot. Durabile pure
 * lui: un taglio che evapora e' una bugia */
int wal_taglia(wal_t *w);

/* il replay: applica i record al motore in ordine di scrittura, dal
 * primo byte. Ritorna i record applicati (put accettate + del, anche
 * su chiave assente [W5]); (size_t)-1 = errore di I/O o memoria.
 * *scartati riceve i byte di coda non valida (troncati via [W3]).
 * I record che il motore rifiuta (per esempio TTL oltre la finestra)
 * si consumano e non si contano: la stessa validazione del vivo,
 * la stessa risposta al morto */
size_t wal_riavvolgi(wal_t *w, motore_t *m, size_t *scartati);

/* il bilancio: byte totali del segmento e record validi. La fonte e'
 * il file, sempre — nessun contatore in memoria da far divergere */
void wal_bilancio(wal_t *w, size_t *byte, size_t *record);

#endif /* WAL_H */
