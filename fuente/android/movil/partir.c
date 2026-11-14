/* partir: parte el C que genera armrecomp en trozos para compilarlos en paralelo y aplica las correcciones
 * (lo mismo que recompilar.c en el PC, antes split_recomp.py + fix_recomp.py). Para generar libjuego.so dentro
 * del teléfono.
 *   partir <juego.c> <carpeta de salida> [MB por trozo = 12]
 * Salida: parte_000.c ... y parte_tabla.c. Imprime "trozos N". */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int MB_POR_PARTE = 12;

typedef struct { char *p; size_t n, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = (char *)realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static int leer_linea(FILE *f, Buf *b) {
    b->n = 0;
    char tmp[65536];
    while (fgets(tmp, sizeof(tmp), f)) {
        size_t n = strlen(tmp);
        buf_add(b, tmp, n);
        if (n && tmp[n - 1] == '\n') return 1;
    }
    return b->n > 0;
}
static int empieza(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }
static int es_def(const char *s) {   /* void vita_func_XXXXXXXX(void) { */
    if (!empieza(s, "void vita_func_")) return 0;
    for (int i = 0; i < 8; i++) if (!s[15 + i] || !strchr("0123456789ABCDEF", s[15 + i])) return 0;
    return empieza(s + 23, "(void) {");
}
static void sin_comentarios(const char *s, Buf *o) {
    o->n = 0; buf_add(o, "", 0);
    while (*s) {
        if (s[0] == '/' && s[1] == '*') { const char *e = strstr(s + 2, "*/"); if (!e) break; s = e + 2; }
        else if (*s == '"') { const char *e = strchr(s + 1, '"'); if (!e) { buf_add(o, s, strlen(s)); break; } s = e + 1; }
        else { buf_add(o, s, 1); s++; }
    }
}
static int cuenta(const char *s, char c) { int n = 0; for (; *s; s++) n += *s == c; return n; }
static int es_etiqueta(const char *s) {   /* ^L_[0-9A-F]+:\s*$ */
    if (!empieza(s, "L_")) return 0;
    const char *p = s + 2;
    if (!*p || !strchr("0123456789ABCDEF", *p)) return 0;
    while (*p && strchr("0123456789ABCDEF", *p)) p++;
    if (*p != ':') return 0;
    for (p++; *p; p++) if (!strchr(" \t\r\n", *p)) return 0;
    return 1;
}
static const char *salta_blancos(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static int es_blanco(const char *s) { for (; *s; s++) if (!strchr(" \t\r\n", *s)) return 0; return 1; }
static int es_cierre(const char *s) { s = salta_blancos(s); return *s == '}' && es_blanco(s + 1); }

typedef struct { FILE *out; size_t tam; char addr[9]; Buf pend; int hay_pend, saltando; long trampas, etiquetas; } Fix;
static void fix_salida(Fix *x, const char *s, size_t n) { fwrite(s, 1, n, x->out); x->tam += n; }
static void fix_vacia_pend(Fix *x, int poner_punto_coma) {
    if (!x->hay_pend) return;
    if (poner_punto_coma) {
        char *nl = strchr(x->pend.p, '\n');
        size_t n1 = nl ? (size_t)(nl - x->pend.p) : x->pend.n, m = n1;
        while (m && (x->pend.p[m - 1] == '\r' || x->pend.p[m - 1] == ' ' || x->pend.p[m - 1] == '\t')) m--;
        fix_salida(x, x->pend.p, m); fix_salida(x, " ;", 2);
        if (nl) fix_salida(x, x->pend.p + n1, x->pend.n - n1);
        x->etiquetas++;
    } else fix_salida(x, x->pend.p, x->pend.n);
    x->pend.n = 0; x->hay_pend = 0;
}
static void fix_linea(Fix *x, const char *s) {
    static Buf code;
    size_t n = strlen(s);
    const char *t = salta_blancos(s);
    if (t[0] == '/' && t[1] == '*' && t[2] == ' ') {
        int ok = 1;
        for (int i = 0; i < 8; i++) if (!t[3 + i] || !strchr("0123456789ABCDEF", t[3 + i])) { ok = 0; break; }
        if (ok && t[11] == ' ') { memcpy(x->addr, t + 3, 8); x->addr[8] = 0; }
    }
    sin_comentarios(s, &code);
    if (x->saltando > 0) { x->saltando += cuenta(code.p, '{') - cuenta(code.p, '}'); return; }
    if (x->hay_pend) {
        if (es_blanco(s) || empieza(t, "/*")) { buf_add(&x->pend, s, n); return; }
        fix_vacia_pend(x, es_cierre(s));
    }
    if (strchr(code.p, '?') && !strchr(code.p, ':') && *t != '#') {
        char linea[160];
        snprintf(linea, sizeof(linea), "%.*svita_trap_unimpl(0x%s, 0, \"operando ?\");", (int)(t - s), s, x->addr);
        fix_salida(x, linea, strlen(linea)); fix_salida(x, "\n", 1);
        x->trampas++;
        int depth = cuenta(code.p, '{') - cuenta(code.p, '}');
        if (depth > 0) x->saltando = depth;
        return;
    }
    if (es_etiqueta(s)) { x->pend.n = 0; buf_add(&x->pend, s, n); x->hay_pend = 1; return; }
    fix_salida(x, s, n);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: partir <juego.c> <carpeta> [MB]\n"); return 2; }
    if (argc > 3) MB_POR_PARTE = atoi(argv[3]) > 0 ? atoi(argv[3]) : 12;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "no se pudo abrir %s\n", argv[1]); return 1; }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    Buf linea = { 0 }, cab = { 0 };
    Fix x; memset(&x, 0, sizeof(x)); strcpy(x.addr, "00000000");
    int cabecera = 1, tabla = 0, idx = 0;
    size_t limite = (size_t)MB_POR_PARTE << 20;
    char ruta[4096];
    while (leer_linea(f, &linea)) {
        if (cabecera) { if (es_def(linea.p)) cabecera = 0; else { buf_add(&cab, linea.p, linea.n); continue; } }
        int nueva = 0;
        if (!tabla && empieza(linea.p, "static const vita_dispatch_entry")) { tabla = 1; snprintf(ruta, sizeof(ruta), "%s/parte_tabla.c", argv[2]); nueva = 1; }
        else if (!tabla && es_def(linea.p) && (!x.out || x.tam > limite)) { snprintf(ruta, sizeof(ruta), "%s/parte_%03d.c", argv[2], idx++); nueva = 1; }
        if (nueva) {
            fix_vacia_pend(&x, 0);
            if (x.out) fclose(x.out);
            x.out = fopen(ruta, "wb");
            if (!x.out) { fprintf(stderr, "no se pudo crear %s\n", ruta); return 1; }
            setvbuf(x.out, NULL, _IOFBF, 1 << 20);
            fwrite(cab.p, 1, cab.n, x.out);
            x.tam = 0; x.saltando = 0;
        }
        fix_linea(&x, linea.p);
    }
    fix_vacia_pend(&x, 0);
    if (x.out) fclose(x.out);
    fclose(f);
    if (!tabla) { fprintf(stderr, "el C generado no tiene tabla de despacho\n"); return 1; }
    printf("trozos %d (trampas '?' %ld, etiquetas %ld)\n", idx, x.trampas, x.etiquetas);
    return 0;
}
