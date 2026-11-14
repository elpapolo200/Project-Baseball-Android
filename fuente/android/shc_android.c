/* shc_android.c — en Android no se compila GLSL en el teléfono (glslang 16 fuera). Los shaders
 * propios del port ya van precompilados a SPIR-V (host/shaders/*_vert.h, *_frag.h) y los del juego
 * los traduce v3k directamente a SPIR-V. shc_compile solo lo usaban el traductor viejo (usse.c,
 * desactivado por defecto) y trazas de depuración. */
#include <stdio.h>
#include "shc.h"
int shc_compile(const char *glsl, int frag, uint32_t **spv, size_t *nwords, char *err, size_t errlen) {
    (void)glsl; (void)frag;
    *spv = NULL; *nwords = 0;
    if (err && errlen) snprintf(err, errlen, "compilador GLSL no disponible en Android");
    return 0;
}
