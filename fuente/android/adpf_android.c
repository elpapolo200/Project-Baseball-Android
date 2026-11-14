/* adpf_android.c — ADPF (Performance Hint, Android 13+): se le dice al sistema qué hilos marcan el
 * ritmo del juego (el principal y «Display Engine») y cuánto deberían tardar por fotograma (33,3 ms a
 * 30 FPS). Con eso el gobernador de CPU sube la frecuencia cuando un fotograma va justo y la baja cuando
 * sobra tiempo, en vez de reaccionar tarde (los fotogramas de 40-50 ms del partido coincidían con los
 * núcleos recién bajados de frecuencia). Ayuda a la estabilidad y al consumo.
 * [android] adpf=0 o PB_ADPF=0 lo quita. Cada fotograma, hle_display.c informa del trabajo real (sin
 * el tiempo que el limitador está dormido). */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

extern void arranque(const char *fmt, ...);
extern int launcher_ini_int(const char *section, const char *key, int def);

typedef void *(*FGetManager)(void);
typedef void *(*FCreate)(void *, const int32_t *, size_t, int64_t);
typedef int (*FReport)(void *, int64_t);

static pid_t g_tid[3];   /* 0 principal, 1 Display Engine, 2 GPU Hilo */
static void *g_sesion;
static FReport g_report;
static int g_on = -1, g_intentado;
static unsigned long g_n;

void pb_adpf_registra(int quien, pid_t tid) {
    if (quien >= 0 && quien < 3) g_tid[quien] = tid;
}

static void crea(void) {
    g_intentado = 1;
    void *la = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    FGetManager gm = la ? (FGetManager)dlsym(la, "APerformanceHint_getManager") : NULL;
    FCreate cs = la ? (FCreate)dlsym(la, "APerformanceHint_createSession") : NULL;
    g_report = la ? (FReport)dlsym(la, "APerformanceHint_reportActualWorkDuration") : NULL;
    void *m = gm ? gm() : NULL;
    /* Los tres hilos del camino de cada fotograma (2026-11-14: antes faltaba el «GPU Hilo»). Objetivo
     * por debajo de 33,3 ms: ADPF ajusta la frecuencia para que el trabajo dure lo que el objetivo; con
     * 33,3 ms bajaba la CPU a ~1 GHz y cualquier fotograma algo más pesado se pasaba del refresco
     * (se veía dos refrescos: tirón). [android] adpf_objetivo=ms o PB_ADPF_OBJETIVO (25 por defecto). */
    const char *eo = getenv("PB_ADPF_OBJETIVO");
    int ms = eo && *eo ? atoi(eo) : launcher_ini_int("android", "adpf_objetivo", 25);
    if (ms < 10 || ms > 33) ms = 25;
    int64_t obj = (int64_t)ms * 1000000LL;
    int32_t tids[3]; size_t n = 0;
    for (int i = 0; i < 3; i++) if (g_tid[i]) tids[n++] = g_tid[i];
    if (m && cs && g_report && n) g_sesion = cs(m, tids, n, obj);
    if (!g_sesion && m && cs && n > 1) g_sesion = cs(m, tids, n - 1, obj);   /* sin el hilo GPU */
    if (!g_sesion && m && cs && g_tid[1]) { int32_t d = g_tid[1]; g_sesion = cs(m, &d, 1, obj); }   /* solo el hilo de dibujo */
    if (!g_sesion && m && cs) { int32_t yo = (int32_t)gettid(); g_sesion = cs(m, &yo, 1, obj); }
    printf("[adpf] gestor %p, crear %p, informar %p, hilos %d %d %d\n", m, (void *)cs, (void *)g_report, (int)g_tid[0], (int)g_tid[1], (int)g_tid[2]);
    printf("[adpf] sesión de rendimiento: %s (%zu hilos, objetivo %d ms)\n", g_sesion ? "sí" : "no disponible", n, ms);
    arranque("adpf: %s (%zu hilos, objetivo %d ms)", g_sesion ? "activado" : "no disponible", n, ms);
}

void pb_adpf_trabajo(long long ns) {
    if (g_on < 0) {
        const char *e = getenv("PB_ADPF");
        g_on = e && *e ? *e != '0' : launcher_ini_int("android", "adpf", 1);
    }
    if (!g_on || ns <= 0 || ns > 1000000000LL) return;
    if (!g_intentado) {
        if ((!g_tid[1] || !g_tid[2]) && ++g_n < 600) return;   /* esperar a los hilos de dibujo y GPU (~20 s) */
        crea();
    }
    if (g_sesion) g_report(g_sesion, (int64_t)ns);
}

/* Tiempo de CPU (s) de los hilos registrados: para el trazador de fotogramas lentos (hle_display.c). */
#include <time.h>
double pb_cpu_hilo(int quien) {
    if (quien < 0 || quien > 2 || !g_tid[quien]) return 0;
    clockid_t c = (clockid_t)((~(unsigned)g_tid[quien] << 3) | 6);   /* MAKE_THREAD_CPUCLOCK(tid, SCHED) */
    struct timespec t;
    if (clock_gettime(c, &t)) return 0;
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}
