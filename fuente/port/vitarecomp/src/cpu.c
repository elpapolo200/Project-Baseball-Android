/* cpu.c — the machine state the generated C operates on (see cpu.h). */

#include "vitarecomp/cpu.h"

#if defined(VITA_CPU_REMOTA)
VITA_TLS vita_cpu_state *vita_cpu_p;
vita_cpu_state *(*vita_cpu_fuente)(void);
static VITA_TLS vita_cpu_state vita_cpu_local;   /* until the game DLL is loaded */
vita_cpu_state *vita_cpu_alta(void) {
    if (!vita_cpu_fuente) return &vita_cpu_local;
    return vita_cpu_p = vita_cpu_fuente();
}
#else
VITA_TLS vita_cpu_state vita_cpu;
#endif
