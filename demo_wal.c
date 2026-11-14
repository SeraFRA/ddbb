/*
 * demo_wal.c — il banco di prova del capitolo 13: il write-ahead log.
 *
 * Il banco racconta la malattia e la cura, con i gate al posto delle
 * promesse: ogni invariante del contratto [W1]..[W6] di wal.h ha la
 * sua prova qui, e una prova fallita ferma tutto (EXIT != 0).
 *
 * Le fasi:
 *   0) il controfatto: il crash senza WAL, numerato (il dolore)
 *   1) la cura: 200 put + 12 del, crash, replay: tutto torna [W1][W3]
 *   2) l'ordine e' la storia: sovrascritture e tombstone [W5]
 *   3) il tick nel record: la TTL rivive col nato GIUSTO [W4]
 *   4) il prefisso sicuro: la coda marcia si tronca, i record nuovi
 *      non finiscono dietro una coda mai leggibile [W3]
 *   5) il taglio dopo lo snapshot: il WAL vive fra due salva [W5]
 *   6) il prezzo: il trittico contro la put nuda, p50 [W2]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "wal.h"

#define DB      "banco_wal.db"
#define SEGMENTO "banco_wal.db.wal"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>                     /* unlink su Linux (lezione cap. 12) */
#endif

static double ora_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = { { 0, 0 } };
    LARGE_INTEGER c;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
#endif
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double p50(double *v, size_t n)
{
    qsort(v, n, sizeof *v, cmp_double);
    return v[n / 2];
}

/* la composizione del layer, l'ordine del contratto [W1]: il log
 * viene PRIMA del motore — un crash fra i due ri-applica, il
 * contrario perderebbe la put accettata */
static int put_con_wal(motore_t *m, wal_t *w, const char *chiave,
                       const char *valore, uint32_t ttl)
{
    size_t kl = strlen(chiave), vl = strlen(valore);
    if (wal_put(w, chiave, kl, valore, vl, ttl, motore_adesso(m)) != WAL_OK)
        return WAL_ERR_IO;               /* niente inchiostro, niente put */
    return motore_put(m, chiave, kl, valore, vl, ttl);
}

static int del_con_wal(motore_t *m, wal_t *w, const char *chiave)
{
    size_t kl = strlen(chiave);
    if (wal_del(w, chiave, kl, motore_adesso(m)) != WAL_OK)
        return WAL_ERR_IO;
    return motore_del(m, chiave, kl);   /* MANCA al replay: legale [W5] */
}

static int vive_esatte(motore_t *m, const char *chiave, const char *atteso)
{
    uint32_t vl = 0;
    const uint8_t *v = motore_get(m, chiave, strlen(chiave), &vl);
    if (!v)
        return atteso == NULL;
    if (atteso == NULL)
        return 0;
    return vl == strlen(atteso) && memcmp(v, atteso, vl) == 0;
}

static void ripulisci(void)
{
    remove(DB);
    remove(SEGMENTO);
}

int main(void)
{
    int gate = 0;
    ripulisci();

    printf("== demo_wal: il capitolo 13 — il write-ahead log ==\n");
    printf("il segmento vive accanto allo snapshot: %s + %s\n\n",
           DB, SEGMENTO);

    /* --------------------------------------------- FASE 0: il controfatto */
    printf("-- fase 0: il dolore, numerato (crash senza WAL) --\n");
    {
        motore_t *m = motore_apri();
        motore_avanza(m, 1);
        for (int i = 0; i < 50; i++) {
            char k[8], v[16];
            snprintf(k, sizeof k, "b%02d", i);
            snprintf(v, sizeof v, "base-%02d", i);
            motore_put(m, k, strlen(k), v, strlen(v), 0);
        }
        motore_salva(m, DB);             /* l'ultimo snapshot: 50 vive */
        for (int i = 0; i < 10; i++) {   /* 10 put SOLO in arena */
            char k[8], v[16];
            snprintf(k, sizeof k, "x%02d", i);
            snprintf(v, sizeof v, "dopo-%02d", i);
            motore_put(m, k, strlen(k), v, strlen(v), 0);
        }
        motore_chiudi(m);                /* il crash: niente salva */

        m = motore_apri_da(DB, 1, NULL);
        size_t dichiarate = 0;
        size_t vive = motore_audit(m, &dichiarate);
        printf("senza WAL il ricaricato ha %zu vive: le 10 put dopo lo "
               "snapshot sono perse [il dolore del quaderno]\n", vive);
        if (vive == 50 && vive_esatte(m, "x00", NULL))
            printf("dichiarato: il motore del capitolo 10 non promette "
                   "cio' che non ha\n");
        else {
            printf("GATE ROTTO (fase 0): il controfatto non torna: "
                   "%zu vive\n", vive);
            gate = 1;
        }
        motore_chiudi(m);
    }

    /* ------------------------------------------------- FASE 1: la cura */
    printf("\n-- fase 1: la cura — 200 put + 12 del, crash, replay --\n");
    {
        ripulisci();
        motore_t *m = motore_apri();
        motore_avanza(m, 1);
        for (int i = 0; i < 50; i++) {
            char k[8], v[16];
            snprintf(k, sizeof k, "b%02d", i);
            snprintf(v, sizeof v, "base-%02d", i);
            motore_put(m, k, strlen(k), v, strlen(v), 0);
        }
        motore_salva(m, DB);

        wal_t *w = wal_apri(SEGMENTO);
        if (!w) {
            printf("GATE ROTTO (fase 1): il segmento non apre\n");
            gate = 1;
            motore_chiudi(m);
        } else {
            size_t byte_attesi = 0;
            for (int i = 0; i < 200; i++) {          /* le 200 col WAL */
                char k[8], v[16];
                snprintf(k, sizeof k, "w%03d", i);
                snprintf(v, sizeof v, "valore-%03d", i);
                if (put_con_wal(m, w, k, v, 0) != MOTORE_OK) {
                    printf("GATE ROTTO (fase 1): put %d rifiutata\n", i);
                    gate = 1;
                    break;
                }
                byte_attesi += 21 + strlen(k) + strlen(v);
            }
            for (int i = 0; i < 10; i++) {           /* 10 del sulla base */
                char k[8];
                snprintf(k, sizeof k, "b%02d", i);
                del_con_wal(m, w, k);
                byte_attesi += 21 + strlen(k);
            }
            del_con_wal(m, w, "w007");               /* su una delle 200 */
            byte_attesi += 21 + strlen("w007");
            del_con_wal(m, w, "mai-esistita");       /* su nulla: legale */
            byte_attesi += 21 + strlen("mai-esistita");

            /* il crash: si chiude TUTTO senza salva */
            wal_chiudi(w);
            motore_chiudi(m);

            /* la rinascita: snapshot + replay */
            m = motore_apri_da(DB, 1, NULL);
            w = wal_apri(SEGMENTO);
            size_t scartati = 0;
            size_t applicati = wal_riavvolgi(w, m, &scartati);
            size_t dichiarate = 0;
            size_t vive = motore_audit(m, &dichiarate);
            size_t byte = 0, record = 0;
            wal_bilancio(w, &byte, &record);

            printf("replay: %zu applicati, %zu scartati, %zu vive "
                   "(dichiarate %zu)\n", applicati, scartati, vive, dichiarate);
            printf("bilancio: %zu byte in %zu record validi\n", byte, record);

            if (applicati != 212) {
                printf("GATE [G1] ROTTO: attesi 212 applicati, sono %zu\n",
                       applicati);
                gate = 1;
            } else
                printf("[G1] VERDE: 212 record applicati in ordine\n");

            int esatte = (vive == 239 && dichiarate == 239);
            for (int i = 0; i < 200 && esatte; i++) {
                char k[8], v[16];
                snprintf(k, sizeof k, "w%03d", i);
                snprintf(v, sizeof v, "valore-%03d", i);
                if (i == 7)
                    esatte = vive_esatte(m, k, NULL);   /* cancellata */
                else
                    esatte = vive_esatte(m, k, v);
            }
            for (int i = 0; i < 50 && esatte; i++) {
                char k[8], v[16];
                snprintf(k, sizeof k, "b%02d", i);
                snprintf(v, sizeof v, "base-%02d", i);
                esatte = i < 10 ? vive_esatte(m, k, NULL)
                               : vive_esatte(m, k, v);
            }
            esatte = esatte && vive_esatte(m, "mai-esistita", NULL);
            if (esatte)
                printf("[G2] VERDE: 239 vive, 199 valori esatti, "
                       "12 assenti al posto giusto\n");
            else {
                printf("GATE [G2] ROTTO: il replay non ricostruisce "
                       "lo stato atteso\n");
                gate = 1;
            }

            if (scartati == 0 && byte == byte_attesi && record == 212) {
                printf("[G3] VERDE: %zu byte, il formato e' un contratto "
                       "(21 + chiave + valore per record)\n", byte);
            } else {
                printf("GATE [G3] ROTTO: attesi %zu byte/212 record, "
                       "sono %zu byte/%zu record, %zu scartati\n",
                       byte_attesi, byte, record, scartati);
                gate = 1;
            }
            wal_chiudi(w);
            motore_chiudi(m);
        }
    }

    /* ------------------------------------ FASE 2: l'ordine e' la storia */
    printf("\n-- fase 2: l'ordine e' la storia (sovrascritture, tombstone) --\n");
    {
        ripulisci();
        motore_t *m = motore_apri();
        motore_avanza(m, 1);
        motore_salva(m, DB);
        wal_t *w = wal_apri(SEGMENTO);
        put_con_wal(m, w, "storia", "prima", 0);
        put_con_wal(m, w, "storia", "seconda", 0);
        del_con_wal(m, w, "storia");
        put_con_wal(m, w, "storia", "terza", 0);
        put_con_wal(m, w, "ombra", "effimera", 0);
        del_con_wal(m, w, "ombra");
        wal_chiudi(w);                   /* crash */
        motore_chiudi(m);

        m = motore_apri_da(DB, 1, NULL);
        w = wal_apri(SEGMENTO);
        size_t applicati = wal_riavvolgi(w, m, NULL);
        int ok = vive_esatte(m, "storia", "terza")
              && vive_esatte(m, "ombra", NULL);
        printf("replay: %zu applicati — ", applicati);
        if (ok && applicati == 6)
            printf("[G4] VERDE: l'ultimo comando vince: "
                   "storia=terza, ombra assente\n");
        else {
            printf("GATE [G4] ROTTO: la storia non e' l'ordine\n");
            gate = 1;
        }
        wal_chiudi(w);
        motore_chiudi(m);
    }

    /* --------------------------------------- FASE 3: il tick nel record */
    printf("\n-- fase 3: il tick nel record — la TTL rivive col nato giusto --\n");
    {
        ripulisci();
        motore_t *m = motore_apri();
        motore_avanza(m, 1);
        motore_salva(m, DB);
        wal_t *w = wal_apri(SEGMENTO);
        put_con_wal(m, w, "effimera", "muore-presto", 5);  /* ttl 5, tick 1 */
        wal_chiudi(w);                   /* crash */
        motore_chiudi(m);

        m = motore_apri_da(DB, 1, NULL); /* si ricarica a tick 1 */
        w = wal_apri(SEGMENTO);
        size_t applicati = wal_riavvolgi(w, m, NULL);
        int viva = vive_esatte(m, "effimera", "muore-presto");
        motore_avanza(m, 6);             /* 6 >= 1 + 5: scade ORA [I8] */
        int morta = vive_esatte(m, "effimera", NULL);
        if (applicati == 1 && viva && morta) {
            printf("[G5] VERDE: replay a tick 1 -> viva (nato ricostruito), "
                   "avanza(6) -> morta: la TTL e' quella vera\n");
        } else {
            printf("GATE [G5] ROTTO: applicati %zu, viva %d, morta %d — "
                   "il nato replayato e' bugiardo\n", applicati, viva, morta);
            gate = 1;
        }
        wal_chiudi(w);
        motore_chiudi(m);
    }

    /* ------------------------------------ FASE 4: il prefisso sicuro */
    printf("\n-- fase 4: il prefisso sicuro — la coda marcia si tronca --\n");
    {
        ripulisci();
        motore_t *m = motore_apri();
        motore_avanza(m, 1);
        motore_salva(m, DB);
        wal_t *w = wal_apri(SEGMENTO);
        for (int i = 0; i < 5; i++) {
            char k[8], v[16];
            snprintf(k, sizeof k, "s%02d", i);
            snprintf(v, sizeof v, "sana-%02d", i);
            put_con_wal(m, w, k, v, 0);
        }
        wal_chiudi(w);
        motore_chiudi(m);

        /* il crash a meta' record: 50 byte di spazzatura in coda */
        FILE *f = fopen(SEGMENTO, "ab");
        for (int i = 0; i < 50; i++)
            fputc(0xAA, f);
        fclose(f);

        m = motore_apri_da(DB, 1, NULL);
        w = wal_apri(SEGMENTO);
        size_t scartati = 0;
        size_t applicati = wal_riavvolgi(w, m, &scartati);
        size_t byte = 0, record = 0;
        wal_bilancio(w, &byte, &record);
        printf("replay: %zu applicati, %zu byte di coda scartati e troncati\n",
               applicati, scartati);

        int ok = (applicati == 5 && scartati == 50 && record == 5);
        for (int i = 0; i < 5 && ok; i++) {
            char k[8], v[16];
            snprintf(k, sizeof k, "s%02d", i);
            snprintf(v, sizeof v, "sana-%02d", i);
            ok = vive_esatte(m, k, v);
        }
        if (ok)
            printf("[G6a] VERDE: il prefisso sano sopravvive, la coda no "
                   "[I5 del capitolo 4, uguale per il WAL]\n");
        else {
            printf("GATE [G6a] ROTTO: applicati %zu, scartati %zu, "
                   "record %zu\n", applicati, scartati, record);
            gate = 1;
        }

        /* la put NUOVA non finisce dietro la coda morta [W3] */
        put_con_wal(m, w, "dopo-il-crash", "viva", 0);
        wal_chiudi(w);
        motore_chiudi(m);
        m = motore_apri_da(DB, 1, NULL);
        w = wal_apri(SEGMENTO);
        scartati = 0;
        applicati = wal_riavvolgi(w, m, &scartati);
        if (applicati == 6 && scartati == 0
            && vive_esatte(m, "dopo-il-crash", "viva")) {
            printf("[G6b] VERDE: il record nuovo e' dietro il prefisso, "
                   "non dietro la coda troncata\n");
        } else {
            printf("GATE [G6b] ROTTO: applicati %zu, scartati %zu — "
                   "la coda morta ombreggia i record nuovi\n",
                   applicati, scartati);
            gate = 1;
        }
        wal_chiudi(w);
        motore_chiudi(m);
    }

    /* ------------------------------------------ FASE 5: il taglio */
    printf("\n-- fase 5: il taglio dopo lo snapshot [W5] --\n");
    {
        ripulisci();
        motore_t *m = motore_apri();
        motore_avanza(m, 1);
        wal_t *w = wal_apri(SEGMENTO);
        for (int i = 0; i < 30; i++) {
            char k[8], v[16];
            snprintf(k, sizeof k, "t%02d", i);
            snprintf(v, sizeof v, "taglio-%02d", i);
            put_con_wal(m, w, k, v, 0);
        }
        motore_salva(m, DB);             /* lo snapshot PRIMA */
        if (wal_taglia(w) != WAL_OK) {   /* il taglio DOPO [W5] */
            printf("GATE ROTTO (fase 5): il taglio fallisce\n");
            gate = 1;
        }
        size_t byte = 1, record = 1;
        wal_bilancio(w, &byte, &record);
        if (byte == 0 && record == 0)
            printf("[G7a] VERDE: snapshot salvato, segmento azzero: "
                   "il WAL vive fra due salva\n");
        else {
            printf("GATE [G7a] ROTTO: il taglio lascia %zu byte / %zu record\n",
                   byte, record);
            gate = 1;
        }
        put_con_wal(m, w, "dopo-taglio", "viva", 0);
        wal_chiudi(w);                   /* crash */
        motore_chiudi(m);

        m = motore_apri_da(DB, 1, NULL);
        w = wal_apri(SEGMENTO);
        size_t applicati = wal_riavvolgi(w, m, NULL);
        size_t dichiarate = 0;
        size_t vive = motore_audit(m, &dichiarate);
        if (applicati == 1 && vive == 31 && dichiarate == 31
            && vive_esatte(m, "dopo-taglio", "viva")) {
            printf("[G7b] VERDE: il replay dopo il taglio applica solo "
                   "il nuovo batch: 30 dallo snapshot + 1 dal WAL\n");
        } else {
            printf("GATE [G7b] ROTTO: applicati %zu, vive %zu — "
                   "il taglio mente o il replay doppione\n", applicati, vive);
            gate = 1;
        }
        wal_chiudi(w);
        motore_chiudi(m);
    }

    /* ------------------------------------------ FASE 6: il prezzo */
    printf("\n-- fase 6: il prezzo del trittico [W2] --\n");
    {
        enum { N_PREZZO = 200 };
        motore_t *m = motore_apri();
        motore_avanza(m, 1);
        double *tp = malloc(N_PREZZO * sizeof *tp);
        double *tw = malloc(N_PREZZO * sizeof *tw);
        if (!tp || !tw) {
            printf("GATE ROTTO (fase 6): memoria\n");
            free(tp); free(tw);
            motore_chiudi(m);
            return 1;
        }
        for (int i = 0; i < N_PREZZO; i++) {
            char k[8], v[16];
            snprintf(k, sizeof k, "n%03d", i);
            snprintf(v, sizeof v, "nuda-%03d", i);
            double a = ora_us();
            motore_put(m, k, strlen(k), v, strlen(v), 0);
            tp[i] = ora_us() - a;
        }
        remove(SEGMENTO);
        wal_t *w = wal_apri(SEGMENTO);
        for (int i = 0; i < N_PREZZO; i++) {
            char k[8], v[16];
            snprintf(k, sizeof k, "q%03d", i);
            snprintf(v, sizeof v, "inchiostro-%03d", i);
            double a = ora_us();
            wal_put(w, k, strlen(k), v, strlen(v), 0, 1);
            tw[i] = ora_us() - a;
        }
        double nuda = p50(tp, N_PREZZO);
        double trittico = p50(tw, N_PREZZO);
        printf("put nuda p50: %.2f us — put col trittico p50: %.2f us "
               "(fwrite + fflush + fsync)\n", nuda, trittico);
        if (trittico >= 2.0 * nuda) {
            printf("[G8] VERDE: la durabilita' non e' gratis (ratio %.0fx); "
                   "a 200 put il WAL costa %.1f ms in tutto — il salva "
                   "intero a 50k chiavi ne costa 64\n",
                   trittico / nuda, trittico * N_PREZZO / 1000.0);
        } else {
            printf("GATE [G8] ROTTO: il trittico non costa piu' della "
                   "put nuda: misura sospetta\n");
            gate = 1;
        }
        free(tp);
        free(tw);
        wal_chiudi(w);
        motore_chiudi(m);
    }

    ripulisci();
    if (gate) {
        printf("\nGATE FALLITO: il contratto [W1]..[W6] non regge\n");
        return 1;
    }
    printf("\nil WAL regge: la memoria sopravvive al crash, un record "
           "per volta\n");
    return 0;
}
