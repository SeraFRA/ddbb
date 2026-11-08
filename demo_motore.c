/*
 * demo_motore.c — il banco di prova del motore unico.
 *
 * Non e' solo una demo: ogni INVARIANTE ha un gate. Se un gate salta,
 * il processo restituisce 1 e la riga colpevole dice quale legge e'
 * caduta. (La lezione del decimo file, fatta politica: un test che
 * stampa senza bloccare non e' un test.)
 *
 *   GATE [I5/I6] round trip: 60000 valori, versioni e cancellazioni
 *   GATE [I7]    intervallo == conteggio indipendente + merge coda
 *   GATE [I7]    audit: le vive a vista == contatore interno
 *   GATE [I9]    bloom: zero falsi negativi sulle vive
 *   GATE [I1]    stesse 500 query pre/post reload: confidenze identiche
 *   GATE [I8]    anello: scadenze deterministiche senza un get
 *   GATE [I10]   corruzione e troncamento: prefisso sicuro
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wpedantic"
# include <windows.h>
# pragma GCC diagnostic pop
#endif

#include "motore.h"

#define KCH 8                   /* il top-k di ogni chiedi della demo */

/* il top-k di ifade non serve qui: la teoria del banding e' nel
 * corpus (file 7 e 10); il motore si misura, non si spiegacima */

static double ora_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = { { 0, 0 } };
    LARGE_INTEGER c;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000. / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static uint32_t prng(void)
{
    static uint32_t x = 0x12345678u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void genera_sostituzione(char *dest, size_t cap, const char *originale,
                               uint32_t errori)
{
    snprintf(dest, cap, "%s", originale);
    size_t l = strlen(dest);
    for (uint32_t e = 0; e < errori; e++) {
        size_t p = (size_t)(prng() % (uint32_t)l);
        if (dest[p] < '0' || dest[p] > '9')
            continue;
        dest[p] = (char)('a' + (prng() % 26));
    }
}

static void genera_lettere(char *dest, size_t cap)
{
    for (size_t i = 0; i < 8 && i < cap - 1; i++)
        dest[i] = (char)('a' + (prng() % 26));
    dest[8] = '\0';
}

static long dimensione_file(const char *nome)
{
    FILE *f = fopen(nome, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int corrompi_byte(const char *nome, long off)
{
    FILE *f = fopen(nome, "r+b");
    if (!f) return -1;
    if (fseek(f, off, SEEK_SET) != 0) { fclose(f); return -1; }
    int c = fgetc(f);
    if (c == EOF) { fclose(f); return -1; }
    fseek(f, off, SEEK_SET);
    fputc(c ^ 0x5A, f);
    fclose(f);
    return 0;
}

static int tronca_file(const char *nome, long togli)
{
    long n = dimensione_file(nome);
    if (n < togli) return -1;
    FILE *f = fopen(nome, "rb");
    if (!f) return -1;
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        { free(buf); fclose(f); return -1; }
    fclose(f);
    f = fopen(nome, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, (size_t)(n - togli), f);
    fclose(f);
    free(buf);
    return 0;
}

static int test_intervallo(motore_t *m);
static int autotest(void);

/* ----------------------------------------------------------------- main */

enum { N_CHIAVI = 60000, N_QUERY = 10000 };

int main(void)
{
    static char kbuf[40], vbuf[64];
    printf("==== IL MOTORE UNICO: dieci file, un sistema ====\n\n");

    motore_t *m = motore_apri();
    if (!m) {
        fprintf(stderr, "memoria\n");
        return 1;
    }
    motore_avanza(m, 1000);                    /* l'orologio e' nostro */

    printf("riempio l'arena: %d chiavi coi loro valori...\n", N_CHIAVI);
    double t0 = ora_ms();
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-di-%u", i);
        if (motore_put(m, kbuf, (size_t)kl, vbuf, (size_t)vl, 0) != MOTORE_OK)
            { fprintf(stderr, "put fallita\n"); return 1; }
    }
    printf("pieno: %d chiavi in %.0f ms\n", N_CHIAVI, ora_ms() - t0);
    motore_statistiche(m, "dopo carico");

    /* il passato sporco: sovrascrivo le pari, cancello le seste */
    for (uint32_t i = 0; i < N_CHIAVI; i += 2) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", i);
        int vl = snprintf(vbuf, sizeof vbuf, "valore-nuovo-di-%u", i);
        motore_put(m, kbuf, (size_t)kl, vbuf, (size_t)vl, 0);
    }
    for (uint32_t i = 0; i < N_CHIAVI; i += 6) {
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", i);
        motore_del(m, kbuf, (size_t)kl);
    }
    motore_statistiche(m, "dopo upd+del");

    /* ---------- la compattazione che impara ---------- */
    t0 = ora_ms();
    if (motore_compatta(m) != MOTORE_OK)
        { fprintf(stderr, "compattazione fallita\n"); return 1; }
    printf("\nmotore_compatta: svuota, ORDINA l'arena per chiave e rideriva "
           "tutto in %.0f ms — il \"merge\" che produce la geometria\n",
           ora_ms() - t0);
    motore_statistiche(m, "ordinata");

    /* ---------- il disco [I10] ---------- */
    t0 = ora_ms();
    if (motore_salva(m, "motore.db") != MOTORE_OK)
        { fprintf(stderr, "salva fallito\n"); return 1; }
    printf("motore_salva: %ld byte in %.0f ms\n",
           dimensione_file("motore.db"), ora_ms() - t0);

    t0 = ora_ms();
    motore_t *n2 = motore_apri_da("motore.db", 1000, NULL);
    if (!n2)
        { fprintf(stderr, "apri_da fallito\n"); return 1; }
    printf("motore_apri_da: %.0f ms (LSH + bloom + anello + recinto "
           "ricostruiti dal solo arena+CRC [I1])\n", ora_ms() - t0);
    motore_statistiche(n2, "ricaricato");

    if (test_intervallo(n2) != 0)
        return 1;

    /* ---------- i valori tornano dal disco ---------- */
    uint32_t sbagliate = 0;
    size_t dichiarate = 0;
    size_t audit_vive = motore_audit(n2, &dichiarate);
    if (audit_vive != dichiarate) {
        fprintf(stderr, "GATE [I7] ROTTO: audit %zu != contatore %zu\n",
                audit_vive, dichiarate);
        return 1;
    }
    for (uint32_t i = 0; i < N_CHIAVI; i++) {
        uint32_t id = (i * 7919u) % N_CHIAVI;
        int kl = snprintf(kbuf, sizeof kbuf, "u%07u", id);
        if (id % 6 == 0 || id == 2755) {             /* il merge-coda ha
                                                      * cancellato 2755  */
            if (motore_get(n2, kbuf, (size_t)kl, NULL) != NULL)
                sbagliate++;                          /* risorta!   */
        } else {
            /* e ha sovrascritto 2505: il valore atteso e' quello nuovo */
            int vl;
            if (id == 2505)
                vl = snprintf(vbuf, sizeof vbuf, "aggiornato-in-coda");
            else
                vl = snprintf(vbuf, sizeof vbuf,
                              id % 2 == 0 ? "valore-nuovo-di-%u" : "valore-di-%u",
                              id);
            uint32_t dl;
            const uint8_t *g = motore_get(n2, kbuf, (size_t)kl, &dl);
            if (!g || dl != (uint32_t)vl || memcmp(g, vbuf, dl) != 0)
                sbagliate++;
        }
    }
    printf("verifica dal disco: %u sbagliate su %d verifiche (valori, "
           "versioni, cancellazioni) + audit %zu vive == contatore: "
           "[I7] regge\n\n", sbagliate, N_CHIAVI, audit_vive);
    if (sbagliate != 0) {
        fprintf(stderr, "GATE [I5/I6] ROTTO: %u sbagliate dal disco\n",
                sbagliate);
        return 1;
    }

    /* ---------- esatti, e la porta del certamente-no ---------- */
    {
        enum { CAMPIONI = 100000 };
        static double t[CAMPIONI];
        double media = 0;
        for (int i = 0; i < CAMPIONI; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "u%07u",
                              (uint32_t)((i * 7919) % N_CHIAVI));
            double a = ora_ms();
            motore_get(n2, kbuf, (size_t)kl, NULL);
            t[i] = ora_ms() - a;
            media += t[i];
        }
        qsort(t, CAMPIONI, sizeof t[0], cmp_double);
        printf("esatti: media %.3f us  p50 %.3f  p99 %.3f  max %.1f us\n",
               media / CAMPIONI * 1000.0,
               t[CAMPIONI / 2] * 1000.0,
               t[CAMPIONI * 99 / 100] * 1000.0,
               t[CAMPIONI - 1] * 1000.0);

        uint32_t forse = 0;
        for (int i = 0; i < CAMPIONI; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "a%07u", (uint32_t)i);
            if (motore_presente(n2, kbuf, (size_t)kl))
                forse++;                              /* il bloom dice "va' a vedere" */
            if (motore_get(n2, kbuf, (size_t)kl, NULL) != NULL) {
                fprintf(stderr, "GATE [I6] ROTTO: un alieno ha risposto\n");
                return 1;
            }
        }
        printf("alieni: la porta bloom ha detto \"certamente no\" senza "
               "toccare lo slot nel %.2f%% dei casi; falsi \"forse\" "
               "misurati: %.3f%% (la teoria vive nella m/n di statistiche)\n\n",
               100.0 * (1.0 - (double)forse / CAMPIONI),
               100.0 * (double)forse / CAMPIONI);
    }

    /* ---------- la domanda probabile ---------- */
    {
        static char qs[N_QUERY][16], veri[N_QUERY][16];
        static double lat[N_QUERY];
        static motore_risposta_t ris[KCH];
        uint32_t top1 = 0, topk = 0, dichiarate_q = 0;
        double somma_conf = 0, somma_pari = 0;
        size_t somma_cand = 0;

        for (int q = 0; q < N_QUERY; q++) {
            uint32_t id = prng() % N_CHIAVI;
            if (id % 6 == 0)
                id = (id + 1) % N_CHIAVI;             /* le seste morte */
            snprintf(veri[q], sizeof veri[q], "u%07u", id);
            genera_sostituzione(qs[q], sizeof qs[q], veri[q], 1);
        }

        printf("== %d refusi di sostituzione, sul RICARICATO ==\n", N_QUERY);
        for (int q = 0; q < N_QUERY; q++) {
            size_t n_cand;
            double a = ora_ms();
            size_t n = motore_chiedi(n2, qs[q], strlen(qs[q]), KCH, ris,
                                     &n_cand);
            lat[q] = ora_ms() - a;
            somma_cand += n_cand;

            int vv1 = n > 0 && ris[0].chiave_len == strlen(veri[q]) &&
                      memcmp(ris[0].chiave, veri[q], ris[0].chiave_len) == 0;
            int vvk = vv1;
            for (size_t r = 1; r < n; r++)
                if (ris[r].chiave_len == strlen(veri[q]) &&
                    memcmp(ris[r].chiave, veri[q], ris[r].chiave_len) == 0)
                    vvk = 1;
            top1 += (uint32_t)vv1;
            topk += (uint32_t)vvk;

            size_t pari = 0;
            for (size_t r = 0; r < n; r++)
                if (ris[r].confidenza >= ris[0].confidenza - 1e-9)
                    pari++;
            somma_pari += (double)pari;
            if (n > 0 && ris[0].confidenza >= 0.40) {
                dichiarate_q++;
                somma_conf += ris[0].confidenza;
            }
        }
        qsort(lat, N_QUERY, sizeof lat[0], cmp_double);
        printf("   verita' al 1o posto: %4.1f%%   nel top-%d: %4.1f%%   "
               "(candidati medi: %zu)\n",
               100.0 * top1 / N_QUERY, KCH, 100.0 * topk / N_QUERY,
               somma_cand / N_QUERY);
        printf("   dichiarate sopra 0.40: %4.1f%% (confidenza media %.2f), "
               "gruppo dei pari-merito: %.1f\n",
               100.0 * dichiarate_q / N_QUERY,
               dichiarate_q ? somma_conf / dichiarate_q : 0.0,
               somma_pari / N_QUERY);
        printf("   latenza: p50 %.3f ms   p99 %.3f ms\n\n",
               lat[N_QUERY / 2], lat[N_QUERY * 99 / 100]);
        if ((uint64_t)topk * 100 < (uint64_t)N_QUERY * 80) {
            fprintf(stderr, "GATE [recall] ROTTO: top-%d %.1f%% sotto 80%%\n",
                    KCH, 100.0 * topk / N_QUERY);
            return 1;
        }

        uint32_t spurie = 0;
        for (int q = 0; q < N_QUERY; q++) {
            char aliena[16];
            genera_lettere(aliena, sizeof aliena);
            size_t n = motore_chiedi(n2, aliena, 8, KCH, ris, NULL);
            if (n > 0 && ris[0].confidenza >= 0.40)
                spurie++;
        }
        printf("alieni semantici con risposta spuria sopra soglia: %u su %d\n\n",
               spurie, N_QUERY);
        if (spurie > 100) {
            fprintf(stderr, "GATE [I6-precisione] ROTTO: %u spurie su %d\n",
                    spurie, N_QUERY);
            return 1;
        }
    }

    /* ---------- l'anello: la tempesta senza un get ---------- */
    {
        motore_t *st = motore_apri();
        if (!st)
            { fprintf(stderr, "memoria\n"); return 1; }
        motore_avanza(st, 100);
        enum { N_STESSA = 20000 };
        t0 = ora_ms();
        for (uint32_t i = 0; i < N_STESSA; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "s%06u", i);
            motore_put(st, kbuf, (size_t)kl, "v", 1, 500);
        }
        printf("l'anello: %d chiavi con la STESSA scadenza inserite in "
               "%.0f ms\n", N_STESSA, ora_ms() - t0);

        t0 = ora_ms();
        motore_avanza(st, 600);                       /* il tick mortale */
        size_t dich = 0;
        size_t vive = motore_audit(st, &dich);
        printf("avanza al tick mortale: %.1f ms per %d scadenze in batch, "
               "SENZA leggere una chiave: restano %zu vite (attese 0)\n",
               ora_ms() - t0, N_STESSA, vive);
        if (vive != 0 || dich != 0) {
            fprintf(stderr, "GATE [I8] ROTTO: restano %zu vite "
                    "(dichiarate %zu)\n", vive, dich);
            return 1;
        }
        motore_statistiche(st, "post-tempesta");

        uint32_t ripopolate = 0;
        for (uint32_t i = 0; i < 200; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "s%06u", i);
            motore_put(st, kbuf, (size_t)kl, "v2", 2, 400);
        }
        for (uint32_t i = 0; i < 200; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "s%06u", i);
            uint32_t dl;
            const uint8_t *g = motore_get(st, kbuf, (size_t)kl, &dl);
            if (g && dl == 2 && memcmp(g, "v2", 2) == 0)
                ripopolate++;
        }
        printf("riciclo: %u/200 chiavi subito ripopolate coi valori "
               "giusti (nodi dell'anello riciclati con loro)\n\n",
               ripopolate);
        if (ripopolate != 200) {
            fprintf(stderr, "GATE [I8] ROTTO: riciclo parziale %u/200\n",
                    ripopolate);
            return 1;
        }
        motore_chiudi(st);
    }

    /* ---------- il banco degli invarianti ---------- */
    if (autotest() != 0)
        return 1;

    remove("motore.db");
    motore_chiudi(n2);
    motore_chiudi(m);
    printf("verde: il motore unico tiene le promesse di motore.h, e ogni "
           "contratto rotto avrebbe fermato la demo prima.\n");
    return 0;
}

/* -------------------------------------------------------------- intervallo */

/*
 * [I7] in tre atti:
 *  1. l'intervallo conta esattamente cio' che conta un giro di get
 *     indipendente (un get per chiave dell'intervallo);
 *  2. merge con la coda: overwrite dopo l'ultimo ordine => valore
 *     nuovo; nata in coda dentro il range => trovata; cancellata =>
 *     assente;
 *  3. l'audit: le vive a vista == il contatore interno.
 */
static int test_intervallo(motore_t *m)
{
    static motore_risposta_t out[4096];
    const char *da = "u0002500", *a = "u0003499";
    char kbuf[16];

    size_t attese = 0;
    for (uint32_t i = 2500; i <= 3499; i++) {
        snprintf(kbuf, sizeof kbuf, "u%07u", i);
        if (motore_get(m, kbuf, 8, NULL))
            attese++;
    }
    size_t totali = 0;
    uint32_t passi = 0;
    size_t n = motore_intervallo(m, da, 8, a, 8, 4096, out, &totali, &passi);
    printf("intervallo [%s .. %s]: indipendente %zu, scan %zu totali "
           "(%zu in out) in %u passi\n",
           da, a, attese, totali, n, passi);
    if (totali != attese || n != attese || attese == 0) {
        fprintf(stderr, "GATE [I7] ROTTO: scan %zu != conteggio %zu\n",
                totali, attese);
        return 1;
    }

    /* il merge vivo: la coda dopo l'ultimo merge */
    motore_put(m, "u0002505", 8, "aggiornato-in-coda", 18, 0);
    motore_del(m, "u0002755", 8);
    motore_put(m, "u00029zz", 8, "nata-in-coda", 13, 0);

    attese = 0;
    for (uint32_t i = 2500; i <= 3499; i++) {
        snprintf(kbuf, sizeof kbuf, "u%07u", i);
        if (motore_get(m, kbuf, 8, NULL))
            attese++;
    }
    attese += 1;                                     /* u00029zz, in coda */

    size_t totale2 = 0;
    n = motore_intervallo(m, da, 8, a, 8, 4096, out, &totale2, &passi);
    printf("dopo il merge coda: scan %zu, indipendente %zu\n", totale2, attese);
    if (totale2 != attese) {
        fprintf(stderr, "GATE [I7] ROTTO: merge coda %zu != attese %zu\n",
                totale2, attese);
        return 1;
    }

    int trovata_coda = 0, valore_nuovo = 0, cancellata_risorta = 0;
    for (size_t r = 0; r < n; r++) {
        if (out[r].chiave_len == 8 && memcmp(out[r].chiave, "u00029zz", 8) == 0 &&
            out[r].valore_len == 13 &&
            memcmp(out[r].valore, "nata-in-coda", 13) == 0)
            trovata_coda = 1;
        if (out[r].chiave_len == 8 && memcmp(out[r].chiave, "u0002505", 8) == 0 &&
            out[r].valore_len == 18 &&
            memcmp(out[r].valore, "aggiornato-in-coda", 18) == 0)
            valore_nuovo = 1;
        if (out[r].chiave_len == 8 && memcmp(out[r].chiave, "u0002755", 8) == 0)
            cancellata_risorta = 1;
    }
    if (!trovata_coda || !valore_nuovo || cancellata_risorta) {
        fprintf(stderr, "GATE [I7] ROTTO: coda nata %d, valore nuovo %d, "
                "cancellata risorta %d\n",
                trovata_coda, valore_nuovo, cancellata_risorta);
        return 1;
    }
    printf("merge della coda: la nata in coda e' dentro col suo valore, "
           "l'overwrite ha il valore NUOVO, la cancellata non risorge: "
           "[I7] regge su tutti i conti\n\n");
    return 0;
}

/* ------------------------------------------------------------- autotest */

enum { N_CASI = 500, NK = 3000 };

/*
 * A esatto+valori [I6]
 * B stesse 500 query pre/post reload: le confidenze del top-k sono una
 *   multiset IDENTICA — il file porta solo l'arena: se ricostruisse una
 *   memoria diversa, qui cadrebbe tutto [I1]
 * C bloom mai bugiardo in negativo sulle vive [I9]
 * D la cancellata non risponde MAI [I3][I6]
 * E l'anello deterministico: scadenze al tick, tempesta, ttl risorto
 *   dal disco e bruciato nel ricaricato [I8]
 * F corruzione 1 byte: persa SOLO l'ultima [I5]
 * G troncamento: il prefisso resta [I5]
 */
static int autotest(void)
{
    static char kbuf[32], vbuf[32];
    int errori = 0;

    motore_t *db = motore_apri();
    if (!db)
        return 1;
    for (uint32_t i = 0; i < NK; i++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", i);
        snprintf(vbuf, sizeof vbuf, "val%06u", i);
        motore_put(db, kbuf, strlen(kbuf), vbuf, strlen(vbuf), 0);
    }

    /* A */
    for (uint32_t i = 0; i < NK; i++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", i);
        snprintf(vbuf, sizeof vbuf, "val%06u", i);
        uint32_t dl;
        const uint8_t *g = motore_get(db, kbuf, strlen(kbuf), &dl);
        if (!g || dl != strlen(vbuf) ||
            memcmp(g, vbuf, strlen(vbuf)) != 0) {
            printf("autotest FALLITO [A]: %s sbagliata\n", kbuf);
            errori++;
            break;
        }
    }
    if (!errori)
        printf("autotest [A]: 3000 esatte col valore giusto: OK\n");
    if (errori)
        { motore_chiudi(db); return errori; }

    /* B: le STESSE query prima e dopo il reload */
    static struct { uint32_t id; char q[16]; } casi[N_CASI];
    static double conf_prima[N_CASI][KCH];
    static size_t n_prima[N_CASI];
    for (int t = 0; t < N_CASI; t++) {
        casi[t].id = prng() % NK;
        snprintf(kbuf, sizeof kbuf, "k%06u", casi[t].id);
        genera_sostituzione(casi[t].q, sizeof casi[t].q, kbuf, 1);
    }
    uint32_t ripresi_prima = 0;
    for (int t = 0; t < N_CASI; t++) {
        motore_risposta_t ris[KCH];
        size_t n = motore_chiedi(db, casi[t].q, strlen(casi[t].q), KCH, ris,
                                 NULL);
        n_prima[t] = n;
        for (size_t r = 0; r < n; r++)
            conf_prima[t][r] = ris[r].confidenza;
        snprintf(kbuf, sizeof kbuf, "k%06u", casi[t].id);
        for (size_t r = 0; r < n; r++)
            if (ris[r].chiave_len == 7 && memcmp(ris[r].chiave, kbuf, 7) == 0)
                { ripresi_prima++; break; }
    }

    if (motore_salva(db, "prova_m.db") != MOTORE_OK) {
        printf("autotest FALLITO: salva\n");
        motore_chiudi(db);
        return 1;
    }
    motore_t *car = motore_apri_da("prova_m.db", 0, NULL);
    if (!car) {
        printf("autotest FALLITO: apri_da\n");
        motore_chiudi(db);
        return 1;
    }
    size_t dich = 0;
    size_t vive_car = motore_audit(car, &dich);
    uint32_t ripresi_dopo = 0;
    for (int t = 0; t < N_CASI; t++) {
        motore_risposta_t ris[KCH];
        size_t n = motore_chiedi(car, casi[t].q, strlen(casi[t].q), KCH, ris,
                                 NULL);
        int pari = (n == n_prima[t]);
        for (size_t r = 0; pari && r < n; r++)
            if (fabs(conf_prima[t][r] - ris[r].confidenza) > 1e-9)
                pari = 0;
        if (!pari) {
            printf("autotest FALLITO [B]: il reload propone un'altra "
                   "memoria su \"%s\"\n", casi[t].q);
            errori++;
            break;
        }
        snprintf(kbuf, sizeof kbuf, "k%06u", casi[t].id);
        for (size_t r = 0; r < n; r++)
            if (ris[r].chiave_len == 7 && memcmp(ris[r].chiave, kbuf, 7) == 0)
                { ripresi_dopo++; break; }
    }
    if (ripresi_prima != ripresi_dopo || vive_car != NK || dich != NK) {
        printf("autotest FALLITO [B]: ripresi %u/%u, audit del ricaricato "
               "%zu (dichiarate %zu)\n",
               ripresi_dopo, ripresi_prima, vive_car, dich);
        errori++;
    }
    /* la soglia: un gate senza soglia lascia passare perfino lo zero
     * (nel primo run questo gate era verde con 0/500: la stessa
     * lezione del decimo, di nuovo) */
    if (ripresi_prima < 300) {
        printf("autotest FALLITO [B]: %u/%d refusi ripresi sotto la "
               "soglia (300): il layer semantico non sta pescando\n",
               ripresi_prima, N_CASI);
        errori++;
    }
    if (!errori)
        printf("autotest [B]: %u/%d refusi ripresi PRIMA e DOPO, "
               "confidenze identiche multiset: il disco ridia' la stessa "
               "memoria: OK\n", ripresi_prima, N_CASI);

    /* C: il bloom non mente in negativo */
    for (uint32_t i = 0; i < NK; i++) {
        snprintf(kbuf, sizeof kbuf, "k%06u", i);
        if (!motore_presente(car, kbuf, strlen(kbuf))) {
            printf("autotest FALLITO [C]: falso negativo bloom su %s\n",
                   kbuf);
            errori++;
            break;
        }
    }
    if (!errori)
        printf("autotest [C]: bloom, zero falsi negativi su 3000 vive: OK\n");
    motore_chiudi(car);
    if (errori)
        { motore_chiudi(db); return errori; }

    /* D: la cancellata non risponde mai */
    motore_del(db, "k000010", 7);
    if (motore_get(db, "k000010", 7, NULL) != NULL) {
        printf("autotest FALLITO [D]: risorta al get\n");
        errori++;
    } else {
        motore_risposta_t ris[KCH];
        size_t n = motore_chiedi(db, "h000010", 7, KCH, ris, NULL);
        int risorta = 0;
        for (size_t r = 0; r < n; r++)
            if (ris[r].chiave_len == 7 &&
                memcmp(ris[r].chiave, "k000010", 7) == 0)
                risorta = 1;
        if (risorta) {
            printf("autotest FALLITO [D]: proposta dalla via semantica\n");
            errori++;
        } else {
            printf("autotest [D]: cancellata: get muto, semantica muta, "
                   "e la porta bloom dice ancora \"%s\" (un forse, non una "
                   "verita': il filtro non cancella, la compattazione "
                   "riassetta): OK\n",
                   motore_presente(db, "k000010", 7) ? "forse" : "no");
        }
    }
    motore_chiudi(db);
    if (errori)
        return errori;

    /* E: l'anello deterministico */
    {
        motore_t *st = motore_apri();
        if (!st)
            return 1;
        motore_put(st, "breve", 5, "vb", 2, 5);
        motore_put(st, "media", 5, "vm", 2, 10);
        motore_put(st, "eterna", 6, "ve", 2, 0);
        motore_avanza(st, 4);
        if (motore_get(st, "breve", 5, NULL) == NULL) {
            printf("autotest FALLITO [E]: breve morta al tick 4 "
                   "(scade a 5)\n");
            errori++;
        }
        motore_avanza(st, 5);                      /* il tick della morte */
        if (motore_get(st, "breve", 5, NULL) != NULL) {
            printf("autotest FALLITO [E]: breve viva alla scadenza\n");
            errori++;
        }
        if (motore_get(st, "media", 5, NULL) == NULL) {
            printf("autotest FALLITO [E]: media morta in anticipo\n");
            errori++;
        }
        motore_avanza(st, 10);
        if (motore_get(st, "media", 5, NULL) != NULL) {
            printf("autotest FALLITO [E]: media viva oltre la scadenza\n");
            errori++;
        }
        if (motore_get(st, "eterna", 6, NULL) == NULL) {
            printf("autotest FALLITO [E]: l'eterna trascinata nella "
                   "tempesta\n");
            errori++;
        }
        enum { NT = 4000 };
        char kb[24];
        for (uint32_t i = 0; i < NT; i++) {
            int kl = snprintf(kb, sizeof kb, "x%05u", i);
            motore_put(st, kb, (size_t)kl, "v", 1, 30);
        }
        motore_avanza(st, 40);
        size_t dich_e = 0;
        size_t vive_e = motore_audit(st, &dich_e);
        if (vive_e != 1 || dich_e != 1) {
            printf("autotest FALLITO [E]: tempesta, restano %zu vite "
                   "(dichiarate %zu)\n", vive_e, dich_e);
            errori++;
        }

        /* il ttl viaggia nel formato: l'anello risorge dal disco */
        motore_put(st, "con-ttl", 7, "vv", 2, 40);   /* nato=40, scade a 80 */
        if (motore_salva(st, "prova_m.db") != MOTORE_OK) {
            printf("autotest FALLITO [E]: salva con ttl\n");
            errori++;
        }
        motore_t *bi = motore_apri_da("prova_m.db", 40, NULL);
        if (!bi) {
            printf("autotest FALLITO [E]: apri_da con ttl\n");
            errori++;
        } else {
            size_t dich2 = 0;
            size_t cariche = motore_audit(bi, &dich2);
            motore_avanza(bi, 80);                   /* il fuoco del ricaricato */
            size_t vive2 = motore_audit(bi, NULL);
            if (cariche != 2 || dich2 != 2 || vive2 != 1) {
                printf("autotest FALLITO [E]: l'anello del ricaricato: "
                       "caricate %zu (dich %zu), dopo il fuoco %zu\n",
                       cariche, dich2, vive2);
                errori++;
            } else {
                printf("autotest [E]: breve/media/eterna ai tick giusti, "
                       "tempesta di 4000 senza un get, il ttl risorge dal "
                       "disco e brucia al suo tick: OK\n");
            }
            motore_chiudi(bi);
        }
        motore_chiudi(st);
    }
    if (errori)
        return errori;

    /* F+G: le regole del disco */
    {
        motore_t *p = motore_apri();
        if (!p)
            return 1;
        enum { M = 2000 };
        char pk[16], pv[16];
        for (int i = 0; i < M; i++) {
            snprintf(pk, sizeof pk, "p%04d", i);
            snprintf(pv, sizeof pv, "vp%04d", i);
            motore_put(p, pk, strlen(pk), pv, strlen(pv), 0);
        }
        for (int i = 0; i < M; i += 4) {
            snprintf(pk, sizeof pk, "p%04d", i);
            motore_del(p, pk, strlen(pk));
        }
        if (motore_salva(p, "prova_m.db") != MOTORE_OK) {
            printf("autotest FALLITO [F]: salva\n");
            motore_chiudi(p);
            return 1;
        }
        corrompi_byte("prova_m.db", dimensione_file("prova_m.db") - 10);

        motore_t *c = motore_apri_da("prova_m.db", 0, NULL);
        if (!c) {
            printf("autotest FALLITO [F]: apri_da del corrotto\n");
            errori++;
        } else {
            size_t vive_f = motore_audit(c, NULL);
            int solo_ultima = vive_f == (size_t)(M - M / 4 - 1) &&
                              motore_get(c, "p1999", 5, NULL) == NULL;
            for (int i = 1; i < M - 1 && solo_ultima; i += 2) {
                snprintf(pk, sizeof pk, "p%04d", i);
                snprintf(pv, sizeof pv, "vp%04d", i);
                uint32_t dl;
                const uint8_t *g = motore_get(c, pk, strlen(pk), &dl);
                if (!g || dl != strlen(pv) || memcmp(g, pv, dl) != 0)
                    solo_ultima = 0;
            }
            if (!solo_ultima) {
                printf("autotest FALLITO [F]: corruzione: %zu vive "
                       "(attese %d, persa SOLO p1999)\n",
                       vive_f, M - M / 4 - 1);
                errori++;
            } else {
                printf("autotest [F]: 1 byte corrotto in coda: persa SOLO "
                       "p1999, %zu vive col valore giusto: OK\n", vive_f);
            }
            motore_chiudi(c);
        }

        motore_salva(p, "prova_m.db");
        tronca_file("prova_m.db", 2000);
        motore_t *tt = motore_apri_da("prova_m.db", 0, NULL);
        if (!tt) {
            printf("autotest FALLITO [G]: troncamento non gestito\n");
            errori++;
        } else {
            size_t vive_g = motore_audit(tt, NULL);
            if (vive_g < (size_t)(M - M / 4 - 60) ||
                vive_g >= (size_t)(M - M / 4)) {
                printf("autotest FALLITO [G]: troncamento: %zu vive\n",
                       vive_g);
                errori++;
            } else {
                printf("autotest [G]: coda di 2000 byte persa: %zu vive, "
                       "prefisso intatto: OK\n", vive_g);
            }
            motore_chiudi(tt);
        }
        motore_chiudi(p);
    }

    remove("prova_m.db");
    return errori;
}
