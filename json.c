/*
 * json.c — il parser/serializzatore minimale (vedi json.h per i limiti).
 *
 * Discesa ricorsiva con cursore: nessuna allocazione oltre quella
 * dell'albero e dei buffer di stringa. La grammatica e' quella del
 * RFC 8259, senza sconti: numeri con zero non significativo, stringhe
 * con controllo nudo, virgole finali — tutto rifiutato. Un parser che
 * "capisce e corregge" e' un bug di sicurezza con la faccia buona.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "json.h"

#define PROFONDITA_MAX 64

/* ------------------------------------------------------------- lettura */

typedef struct {
    const char *s;
    size_t      pos, n;
    int         prof;
} lettura_t;

static void salta_spazi(lettura_t *L)
{
    while (L->pos < L->n) {
        char c = L->s[L->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            L->pos++;
        else
            break;
    }
}

static jval_t *nuovo(jtipo_t t)
{
    jval_t *v = calloc(1, sizeof *v);
    if (v)
        v->tipo = t;
    return v;
}

void json_libera(jval_t *v)
{
    if (!v)
        return;
    if (v->tipo == J_STR)
        free(v->str);
    if (v->tipo == J_ARR || v->tipo == J_OBJ) {
        for (size_t i = 0; i < v->n; i++) {
            json_libera(v->figli[i]);
            if (v->chiavi)
                free(v->chiavi[i]);
        }
        free(v->figli);
        free(v->chiavi);
    }
    free(v);
}

/* --------------------------------------------------------- il buffer */

int jbuf_init(jbuf_t *b)
{
    b->cap = 256;
    b->n = 0;
    b->rotto = 0;
    b->p = malloc(b->cap);
    if (!b->p) {
        b->rotto = 1;
        return -1;
    }
    b->p[0] = '\0';
    return 0;
}

void jbuf_libera(jbuf_t *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

int jbuf_add(jbuf_t *b, const char *s, size_t len)
{
    if (b->rotto)
        return 0;                       /* gia' morto: no-op onesto */
    if (b->n + len + 1 > b->cap) {
        size_t cap = b->cap;
        while (b->n + len + 1 > cap)
            cap *= 2;
        char *nuovo_p = realloc(b->p, cap);
        if (!nuovo_p) {
            b->rotto = 1;                /* morte atomica del buffer */
            return -1;
        }
        b->p = nuovo_p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
    return 0;
}

int jbuf_testo(jbuf_t *b, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  if (jbuf_add(b, "\\\"", 2)) return -1; break;
        case '\\': if (jbuf_add(b, "\\\\", 2)) return -1; break;
        case '\b': if (jbuf_add(b, "\\b", 2))  return -1; break;
        case '\f': if (jbuf_add(b, "\\f", 2))  return -1; break;
        case '\n': if (jbuf_add(b, "\\n", 2))  return -1; break;
        case '\r': if (jbuf_add(b, "\\r", 2))  return -1; break;
        case '\t': if (jbuf_add(b, "\\t", 2))  return -1; break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
                if (jbuf_add(b, esc, 6)) return -1;
            } else {
                if (jbuf_add(b, s + i, 1)) return -1;
            }
        }
    }
    return 0;
}

int jbuf_num(jbuf_t *b, double v)
{
    char tmp[40];
    int scritti;
    if (v >= -9.0e15 && v <= 9.0e15 && v == floor(v))
        scritti = snprintf(tmp, sizeof tmp, "%.0f", v);
    else
        scritti = snprintf(tmp, sizeof tmp, "%.17g", v);
    if (scritti < 0 || (size_t)scritti >= sizeof tmp)
        return -1;
    return jbuf_add(b, tmp, (size_t)scritti);
}

/* ------------------------------------------------------------- parse */

static int accedi_utf8(jbuf_t *b, unsigned cp)
{
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    return jbuf_add(b, tmp, n);
}

static int hex4(lettura_t *L, unsigned *val)
{
    *val = 0;
    for (int i = 0; i < 4; i++) {
        if (L->pos >= L->n)
            return 0;
        char c = L->s[L->pos++];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (unsigned)(c - 'A' + 10);
        else
            return 0;
        *val = (*val << 4) | d;
    }
    return 1;
}

/* la virgoletta iniziale e' gia' consumata */
static jval_t *p_stringa(lettura_t *L)
{
    jbuf_t b;
    if (jbuf_init(&b) != 0)
        return NULL;
    for (;;) {
        if (L->pos >= L->n)
            goto errore;
        unsigned char c = (unsigned char)L->s[L->pos++];
        if (c == '"')
            break;
        if (c == '\\') {
            if (L->pos >= L->n)
                goto errore;
            char e = L->s[L->pos++];
            switch (e) {
            case '"':  if (jbuf_add(&b, "\"", 1))  goto errore; break;
            case '\\': if (jbuf_add(&b, "\\", 1))  goto errore; break;
            case '/':  if (jbuf_add(&b, "/", 1))   goto errore; break;
            case 'b':  if (jbuf_add(&b, "\b", 1))  goto errore; break;
            case 'f':  if (jbuf_add(&b, "\f", 1))  goto errore; break;
            case 'n':  if (jbuf_add(&b, "\n", 1))  goto errore; break;
            case 'r':  if (jbuf_add(&b, "\r", 1))  goto errore; break;
            case 't':  if (jbuf_add(&b, "\t", 1))  goto errore; break;
            case 'u': {
                unsigned cp;
                if (!hex4(L, &cp))
                    goto errore;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* surrogata alta: la bassa e' obbligatoria */
                    if (L->pos + 1 < L->n &&
                        L->s[L->pos] == '\\' && L->s[L->pos + 1] == 'u') {
                        L->pos += 2;
                        unsigned bassa;
                        if (!hex4(L, &bassa))
                            goto errore;
                        if (bassa >= 0xDC00 && bassa <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10)
                                         + (bassa - 0xDC00);
                        else
                            cp = 0xFFFD;  /* spaiata: dichiarata in json.h */
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                if (accedi_utf8(&b, cp))
                    goto errore;
                break;
            }
            default:
                goto errore;
            }
        } else if (c < 0x20) {
            goto errore;                  /* controllo nudo: non e' JSON */
        } else {
            char t = (char)c;
            if (jbuf_add(&b, &t, 1))
                goto errore;
        }
    }
    if (b.rotto)
        goto errore;
    char z = '\0';
    if (jbuf_add(&b, &z, 1))
        goto errore;
    jval_t *v = nuovo(J_STR);
    if (!v)
        goto errore;
    v->str = b.p;                        /* il buffer passa di mano */
    v->str_len = b.n - 1;
    return v;
errore:
    jbuf_libera(&b);
    return NULL;
}

static jval_t *p_numero(lettura_t *L)
{
    size_t inizio = L->pos;
    if (L->pos < L->n && L->s[L->pos] == '-')
        L->pos++;
    if (L->pos >= L->n || L->s[L->pos] < '0' || L->s[L->pos] > '9')
        return NULL;
    if (L->s[L->pos] == '0') {
        L->pos++;                        /* "0" e basta: niente "01" */
    } else {
        while (L->pos < L->n && L->s[L->pos] >= '0' && L->s[L->pos] <= '9')
            L->pos++;
    }
    if (L->pos < L->n && L->s[L->pos] == '.') {
        L->pos++;
        if (L->pos >= L->n || L->s[L->pos] < '0' || L->s[L->pos] > '9')
            return NULL;
        while (L->pos < L->n && L->s[L->pos] >= '0' && L->s[L->pos] <= '9')
            L->pos++;
    }
    if (L->pos < L->n && (L->s[L->pos] == 'e' || L->s[L->pos] == 'E')) {
        L->pos++;
        if (L->pos < L->n && (L->s[L->pos] == '+' || L->s[L->pos] == '-'))
            L->pos++;
        if (L->pos >= L->n || L->s[L->pos] < '0' || L->s[L->pos] > '9')
            return NULL;
        while (L->pos < L->n && L->s[L->pos] >= '0' && L->s[L->pos] <= '9')
            L->pos++;
    }
    size_t len = L->pos - inizio;
    char *copia = malloc(len + 1);
    if (!copia)
        return NULL;
    memcpy(copia, L->s + inizio, len);
    copia[len] = '\0';
    char *fine;
    double val = strtod(copia, &fine);
    int ok = (*fine == '\0');
    free(copia);
    if (!ok)
        return NULL;
    jval_t *v = nuovo(J_NUM);
    if (!v)
        return NULL;
    v->num = val;
    return v;
}

static jval_t *p_valore(lettura_t *L);

static int aggiungi(jval_t *contenitore, char *chiave, jval_t *figlio)
{
    if (contenitore->n == contenitore->cap) {
        size_t cap = contenitore->cap ? contenitore->cap * 2 : 8;
        jval_t **f = realloc(contenitore->figli, cap * sizeof *f);
        if (!f)
            return -1;
        contenitore->figli = f;
        if (contenitore->tipo == J_OBJ) {
            char **c = realloc(contenitore->chiavi, cap * sizeof *c);
            if (!c)
                return -1;
            contenitore->chiavi = c;
        }
        contenitore->cap = cap;
    }
    contenitore->figli[contenitore->n] = figlio;
    if (contenitore->tipo == J_OBJ)
        contenitore->chiavi[contenitore->n] = chiave;
    contenitore->n++;
    return 0;
}

static jval_t *p_array(lettura_t *L)     /* '[' gia' consumato */
{
    jval_t *v = nuovo(J_ARR);
    if (!v)
        return NULL;
    salta_spazi(L);
    if (L->pos < L->n && L->s[L->pos] == ']') {
        L->pos++;
        return v;
    }
    for (;;) {
        jval_t *elem = p_valore(L);
        if (!elem || aggiungi(v, NULL, elem) != 0) {
            json_libera(elem);
            json_libera(v);
            return NULL;
        }
        salta_spazi(L);
        if (L->pos < L->n && L->s[L->pos] == ',') {
            L->pos++;
            continue;
        }
        if (L->pos < L->n && L->s[L->pos] == ']') {
            L->pos++;
            return v;
        }
        json_libera(v);
        return NULL;
    }
}

static jval_t *p_oggetto(lettura_t *L)   /* '{' gia' consumato */
{
    jval_t *v = nuovo(J_OBJ);
    if (!v)
        return NULL;
    salta_spazi(L);
    if (L->pos < L->n && L->s[L->pos] == '}') {
        L->pos++;
        return v;
    }
    for (;;) {
        salta_spazi(L);
        if (L->pos >= L->n || L->s[L->pos] != '"') {
            json_libera(v);
            return NULL;
        }
        L->pos++;
        jval_t *chiave = p_stringa(L);
        if (!chiave) {
            json_libera(v);
            return NULL;
        }
        salta_spazi(L);
        if (L->pos >= L->n || L->s[L->pos] != ':') {
            json_libera(chiave);
            json_libera(v);
            return NULL;
        }
        L->pos++;
        jval_t *val = p_valore(L);
        if (!val || aggiungi(v, chiave->str, val) != 0) {
            json_libera(val);
            json_libera(chiave);         /* libera anche str se resta */
            json_libera(v);
            return NULL;
        }
        chiave->str = NULL;             /* la stringa e' del contenitore */
        json_libera(chiave);
        salta_spazi(L);
        if (L->pos < L->n && L->s[L->pos] == ',') {
            L->pos++;
            continue;
        }
        if (L->pos < L->n && L->s[L->pos] == '}') {
            L->pos++;
            return v;
        }
        json_libera(v);
        return NULL;
    }
}

static jval_t *p_letterale(lettura_t *L, const char *parola, jtipo_t t, int val)
{
    size_t len = strlen(parola);
    if (L->n - L->pos < len || memcmp(L->s + L->pos, parola, len) != 0)
        return NULL;
    L->pos += len;
    jval_t *v = nuovo(t);
    if (!v)
        return NULL;
    v->booleano = val;
    return v;
}

static jval_t *p_valore(lettura_t *L)
{
    if (L->prof >= PROFONDITA_MAX)
        return NULL;
    salta_spazi(L);
    if (L->pos >= L->n)
        return NULL;
    char c = L->s[L->pos];
    if (c == '{') {
        L->pos++;
        L->prof++;
        jval_t *v = p_oggetto(L);
        L->prof--;
        return v;
    }
    if (c == '[') {
        L->pos++;
        L->prof++;
        jval_t *v = p_array(L);
        L->prof--;
        return v;
    }
    if (c == '"') {
        L->pos++;
        return p_stringa(L);
    }
    if (c == 't')
        return p_letterale(L, "true", J_BOOL, 1);
    if (c == 'f')
        return p_letterale(L, "false", J_BOOL, 0);
    if (c == 'n')
        return p_letterale(L, "null", J_NULL, 0);
    return p_numero(L);
}

jval_t *json_parse(const char *s, size_t len)
{
    if (!s || len == 0)
        return NULL;
    lettura_t L = { s, 0, len, 0 };
    jval_t *v = p_valore(&L);
    if (!v)
        return NULL;
    salta_spazi(&L);
    if (L.pos != L.n) {                  /* spazzatura in coda: rifiuto */
        json_libera(v);
        return NULL;
    }
    return v;
}

/* ---------------------------------------------------------- accesso */

const jval_t *json_get(const jval_t *oggetto, const char *chiave)
{
    if (!oggetto || oggetto->tipo != J_OBJ || !chiave)
        return NULL;
    for (size_t i = 0; i < oggetto->n; i++)
        if (strcmp(oggetto->chiavi[i], chiave) == 0)
            return oggetto->figli[i];
    return NULL;
}

const char *json_str(const jval_t *v, size_t *len)
{
    if (!v || v->tipo != J_STR)
        return NULL;
    if (len)
        *len = v->str_len;
    return v->str;
}

double json_num(const jval_t *v, int *valido)
{
    if (!v || v->tipo != J_NUM) {
        if (valido)
            *valido = 0;
        return 0.0;
    }
    if (valido)
        *valido = 1;
    return v->num;
}

/* ------------------------------------------------------ serializzazione */

int json_scrivi(jbuf_t *b, const jval_t *v)
{
    if (!v)
        return JBUF_LIT(b, "null");
    switch (v->tipo) {
    case J_NULL:
        return JBUF_LIT(b, "null");
    case J_BOOL:
        return jbuf_add(b, v->booleano ? "true" : "false",
                         v->booleano ? 4 : 5);
    case J_NUM:
        return jbuf_num(b, v->num);
    case J_STR:
        if (jbuf_add(b, "\"", 1) != 0)
            return -1;
        if (jbuf_testo(b, v->str, v->str_len) != 0)
            return -1;
        return JBUF_LIT(b, "\"");
    case J_ARR:
        if (jbuf_add(b, "[", 1) != 0)
            return -1;
        for (size_t i = 0; i < v->n; i++) {
            if (i && jbuf_add(b, ",", 1) != 0)
                return -1;
            if (json_scrivi(b, v->figli[i]) != 0)
                return -1;
        }
        return JBUF_LIT(b, "]");
    case J_OBJ:
        if (jbuf_add(b, "{", 1) != 0)
            return -1;
        for (size_t i = 0; i < v->n; i++) {
            if (i && jbuf_add(b, ",", 1) != 0)
                return -1;
            if (jbuf_add(b, "\"", 1) != 0)
                return -1;
            if (jbuf_testo(b, v->chiavi[i], strlen(v->chiavi[i])) != 0)
                return -1;
            if (jbuf_add(b, "\":", 2) != 0)
                return -1;
            if (json_scrivi(b, v->figli[i]) != 0)
                return -1;
        }
        return JBUF_LIT(b, "}");
    }
    return -1;
}
