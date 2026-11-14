/*
 * misura_fsync.c — la MISURA che precede la decisione (capitolo 13: WAL).
 *
 * LA DOMANDA: quanto costa rendere durabile UNA put? Il quaderno del
 * capitolo 13 tiene la decisione (2) in bilico — fsync per put vs
 * group commit vs fsync solo a salva — e una decisione in bilico si
 * chiude coi numeri, non coi gusti. Questo file e' il banco che li
 * produce, prima che esista una sola riga di wal.c.
 *
 * Cosa misura (record da 104 byte, la taglia di una voce del motore:
 * header 24 + chiave 16 + valore 64):
 *   A) append puro      : fwrite nel buffer user-space (il costo minimo)
 *   B) append + fflush  : il kernel vede i byte (sopravvive al processo,
 *                         NON alla corrente: durabilita' a meta')
 *   C) append + fsync  : durabile ORA (la domanda della decisione)
 *   D) group commit     : fsync ogni 32 record (l'ammortizzazione)
 *
 * I GATE (il confronto senza soglia non e' un gate):
 *   [G1] fsync per record > 10x append per record: la durabilita'
 *        costa ordini di grandezza. Se non fosse cosi', la misura
 *        stessa sarebbe sospetta (RAM-disk o cache bugiarda).
 *   [G2] group commit (32) per record < fsync per record, ratio >= 2:
 *        l'ammortizzazione esiste davvero, o il group commit e'
 *        un'idea senza dato.
 *
 * NOTA PORTABILE: su Windows la sincronizzazione e' _commit (io.h);
 * su POSIX fsync. fdatasync (che saltarebbe i metadati) qui non
 * comprerebbe niente: un WAL append-only cambia la dimensione del
 * file a ogni record, e la dimensione E' metadata — fdatasync la
 * deve comunque scrivere. fsync e' la risposta onesta per un log
 * che cresce.
 *
 * L'orologio qui e' da muro per contratto: questo file e' il banco
 * che misura, non il motore che decide [I8].
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define sincronizza(fd) _commit((fd))
#else
#include <unistd.h>
#define sincronizza(fd) fsync((fd))
#endif

#define DIM_RECORD   104u          /* header 24 + chiave 16 + valore 64 */
#define N_APPEND     100000       /* A e B: il buffer non si difende   */
#define N_FSYNC      512          /* C: lento per definizione           */
#define N_GROUP      32768        /* D: 1024 sincronizzazioni          */
#define GRUPPO       32           /* l'N del group commit              */

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

static double percentile(double *v, size_t n, double p)
{
    size_t i = (size_t)(p * (double)(n - 1));
    return v[i];
}

/* il record e' sempre lo stesso: la misura e' la sincronizzazione,
 * non la generazione dei byte */
static uint8_t record[DIM_RECORD];

static void prepara_record(void)
{
    memcpy(record, "wal", 3);
    for (size_t i = 24; i < DIM_RECORD; i++)
        record[i] = (uint8_t)(i * 31u);
}

/* ---------------------------------------------------------- i banchi */

static double banco_append(const char *nome, size_t n, int flusso)
{
    FILE *f = fopen(nome, "wb");
    if (!f) { perror(nome); exit(1); }
    double t0 = ora_us();
    for (size_t i = 0; i < n; i++) {
        if (fwrite(record, 1, DIM_RECORD, f) != DIM_RECORD)
            { fclose(f); perror("fwrite"); exit(1); }
        if (flusso == 1 && fflush(f) != 0)
            { fclose(f); perror("fflush"); exit(1); }
    }
    double t = ora_us() - t0;
    fclose(f);
    return t;
}

/* fsync per record: ritorna il totale e riempie le latenze singole.
 * Il trittico e' fwrite + fflush + fsync: fsync senza fflush vedrebbe
 * un kernel che non ha ancora ricevuto i byte del buffer del processo
 * — durabilita' bugiarda (il post mortem di questo banco) */
static double banco_fsync(const char *nome, size_t n, double *lat)
{
    FILE *f = fopen(nome, "wb");
    if (!f) { perror(nome); exit(1); }
    double t0 = ora_us();
    for (size_t i = 0; i < n; i++) {
        if (fwrite(record, 1, DIM_RECORD, f) != DIM_RECORD)
            { fclose(f); perror("fwrite"); exit(1); }
        double a = ora_us();
        if (fflush(f) != 0 || sincronizza(fileno(f)) != 0)
            { fclose(f); perror("sync"); exit(1); }
        lat[i] = ora_us() - a;
    }
    double t = ora_us() - t0;
    fclose(f);
    return t;
}

/* group commit: fsync ogni GRUPPO record */
static double banco_gruppo(const char *nome, size_t n, size_t gruppo)
{
    FILE *f = fopen(nome, "wb");
    if (!f) { perror(nome); exit(1); }
    double t0 = ora_us();
    for (size_t i = 0; i < n; i++) {
        if (fwrite(record, 1, DIM_RECORD, f) != DIM_RECORD)
            { fclose(f); perror("fwrite"); exit(1); }
        if ((i + 1) % gruppo == 0 &&
            (fflush(f) != 0 || sincronizza(fileno(f)) != 0))
            { fclose(f); perror("sync"); exit(1); }
    }
    double t = ora_us() - t0;
    fclose(f);
    return t;
}

int main(void)
{
    prepara_record();
    int gate = 0;

    printf("== misura_fsync: il costo della durabilita' (record %u byte) ==\n",
           DIM_RECORD);

    /* A: append puro */
    double ta = banco_append("banco_a.wal", N_APPEND, 0);
    printf("A) append puro      : %6zu record in %8.0f us -> %6.2f us/record, %7.0f record/s\n",
           (size_t)N_APPEND, ta, ta / N_APPEND, N_APPEND / (ta / 1e6));

    /* B: append + fflush */
    double tb = banco_append("banco_b.wal", N_APPEND, 1);
    printf("B) append + fflush  : %6zu record in %8.0f us -> %6.2f us/record, %7.0f record/s\n",
           (size_t)N_APPEND, tb, tb / N_APPEND, N_APPEND / (tb / 1e6));

    /* C: append + fsync per record, coi percentuali */
    double *lat = malloc(N_FSYNC * sizeof *lat);
    if (!lat) { perror("malloc"); return 1; }
    double tc = banco_fsync("banco_c.wal", N_FSYNC, lat);
    qsort(lat, N_FSYNC, sizeof *lat, cmp_double);
    printf("C) append + fsync   : %6d record in %8.0f us -> %6.2f us/record, %7.0f record/s",
           N_FSYNC, tc, tc / N_FSYNC, N_FSYNC / (tc / 1e6));
    printf("   [p50 %.0f us, p99 %.0f us, max %.0f us]\n",
           percentile(lat, N_FSYNC, 0.50),
           percentile(lat, N_FSYNC, 0.99),
           lat[N_FSYNC - 1]);

    /* D: group commit */
    double td = banco_gruppo("banco_d.wal", N_GROUP, GRUPPO);
    printf("D) group commit (%2d): %6d record in %8.0f us -> %6.2f us/record, %7.0f record/s\n",
           GRUPPO, N_GROUP, td, td / N_GROUP, N_GROUP / (td / 1e6));

    /* ------------------------------------------------------- i gate */
    double per_a = ta / N_APPEND, per_c = tc / N_FSYNC, per_d = td / N_GROUP;

    printf("\n-- gate --\n");
    if (per_c > 10.0 * per_a) {
        printf("[G1] VERDE: fsync/record (%.2f us) > 10x append/record (%.2f us): "
               "ratio %.0fx — la durabilita' costa\n", per_c, per_a, per_c / per_a);
    } else {
        printf("[G1] ROSSO: fsync/record (%.2f us) <= 10x append/record (%.2f us): "
               "misura sospetta (RAM-disk? cache bugiarda?)\n", per_c, per_a);
        gate = 1;
    }
    if (per_c >= 2.0 * per_d) {
        printf("[G2] VERDE: group/record (%.2f us) <= meta' di fsync/record (%.2f us): "
               "l'ammortizzazione esiste, ratio %.1fx\n", per_d, per_c, per_c / per_d);
    } else {
        printf("[G2] ROSSO: group/record (%.2f us) non ammortizza vs fsync/record (%.2f us)\n",
               per_d, per_c);
        gate = 1;
    }

    free(lat);
    remove("banco_a.wal");
    remove("banco_b.wal");
    remove("banco_c.wal");
    remove("banco_d.wal");

    if (gate) {
        printf("\nGATE FALLITO: la misura non sostiene le soglie\n");
        return 1;
    }
    printf("\nmisura completata: i numeri per la decisione (2) sono qui\n");
    return 0;
}
