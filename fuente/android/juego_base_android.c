/* juego_base_android.c — lo mínimo que necesita libjuego.so (el juego traducido, ARM64 Android):
 * los registros de la CPU emulada por hilo (TLS ELF nativa: Android 10+), la ventana de memoria en
 * línea (mem.h, VITA_MEM_DIRECTA) y memcpy/memset/memmove (se compila sin libc). Equivale a
 * dist/juego_base.c del PC, sin el directorio TLS de Windows. */
#include <stddef.h>
#include <stdint.h>
#include "vitarecomp/cpu.h"

#define EXPORTA __attribute__((visibility("default")))

VITA_TLS vita_cpu_state vita_cpu;
EXPORTA vita_cpu_state *vita_cpu_tls(void) { return &vita_cpu; }

uintptr_t vita_md_off;
uint32_t  vita_md_base;
uint32_t  vita_md_lim;
EXPORTA void vita_mem_directa(unsigned char *mem, uint32_t base, unsigned long long size) {
    vita_md_off = (uintptr_t)mem - base;
    vita_md_base = base;
    vita_md_lim = size > 3 ? (uint32_t)(size - 3) : 0;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    while (n >= 8) { uint64_t v; __builtin_memcpy_inline(&v, b, 8); __builtin_memcpy_inline(a, &v, 8); a += 8; b += 8; n -= 8; }
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    if (a == b || !n) return d;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *a = (unsigned char *)d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
double sqrt(double x) { return __builtin_sqrt(x); }
double trunc(double x) { return __builtin_trunc(x); }
float truncf(float x) { return __builtin_truncf(x); }
