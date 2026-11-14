/* perfil_android.c — perfilador por muestreo dentro del juego (simpleperf necesita desactivar una
 * protección del sistema). PB_PERFIL=inicio,duración (segundos desde que arranca el port): cada 10 ms
 * se mira qué hilos están ejecutándose (estado R en /proc) y a esos se les manda una señal; el
 * manejador apunta el PC. Al acabar se agrupa por hilo y por función/biblioteca en datos/perfil.txt
 * (desplazamientos: llvm-symbolizer con libmain.so sin recortar y libjuego.so en el PC). */
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

extern void arranque(const char *fmt, ...);
const char *pb_android_dir(void);

#define NMUES (1 << 20)
#define NMARCOS 6
typedef struct { int32_t tid; uint64_t pc; uint64_t ret[NMARCOS]; } Muestra;
static Muestra *g_m;
static volatile uint32_t g_n;
static volatile int g_ok;

static void al_muestrear(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    uint32_t i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED);
    if (i >= NMUES) return;
#if defined(__aarch64__)
    ucontext_t *u = (ucontext_t *)uc;
    g_m[i].pc = (uint64_t)u->uc_mcontext.pc;
    /* pila: LR y luego la cadena de marcos x29 (dentro de la pila del hilo; sin firma PAC) */
    uint64_t sp = u->uc_mcontext.sp, fp = u->uc_mcontext.regs[29];
    g_m[i].ret[0] = u->uc_mcontext.regs[30] & 0x0000FFFFFFFFFFFFull;
    for (int k = 1; k < NMARCOS; k++) {
        if (!fp || (fp & 15) || fp < sp || fp > sp + (8u << 20)) { g_m[i].ret[k] = 0; continue; }
        g_m[i].ret[k] = ((uint64_t *)fp)[1] & 0x0000FFFFFFFFFFFFull;
        uint64_t sig = ((uint64_t *)fp)[0];
        fp = sig > fp ? sig : 0;
    }
#else
    (void)uc; g_m[i].pc = 0;
#endif
    g_m[i].tid = (int32_t)gettid();
    g_ok = 1;
}

typedef struct { int32_t tid; char nom[20]; uint32_t n; } Hilo;
typedef struct { uint64_t clave; int32_t tid; uint32_t n; } Cuenta;
static int cmp_cuenta(const void *a, const void *b) { return (int)((const Cuenta *)b)->n - (int)((const Cuenta *)a)->n; }
static int cmp_hilo(const void *a, const void *b) { return (int)((const Hilo *)b)->n - (int)((const Hilo *)a)->n; }

/* clave de agrupación: inicio del símbolo si dladdr lo sabe; si no, el PC redondeado a 64 B */
static uint64_t clave_de(uint64_t pc) {
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_saddr) return (uint64_t)di.dli_saddr;
    return pc & ~63ull;
}
static void nombre_de(uint64_t a, char *b, size_t n) {
    Dl_info di;
    if (a && dladdr((void *)a, &di) && di.dli_fname) {
        const char *f = strrchr(di.dli_fname, '/'); f = f ? f + 1 : di.dli_fname;
        if (di.dli_sname) snprintf(b, n, "%s+0x%lx %s", f, (unsigned long)(a - (uint64_t)di.dli_fbase), di.dli_sname);
        else snprintf(b, n, "%s+0x%lx", f, (unsigned long)(a - (uint64_t)di.dli_fbase));
    } else snprintf(b, n, "0x%llx", (unsigned long long)a);
}

static void *perfilar(void *x) {
    (void)x;
    const char *e = getenv("PB_PERFIL");
    int ini = 0, dur = 20;
    if (sscanf(e, "%d,%d", &ini, &dur) < 1) return NULL;
    sleep((unsigned)ini);
    g_m = (Muestra *)calloc(NMUES, sizeof(Muestra));
    if (!g_m) return NULL;
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = al_muestrear; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 4, &sa, NULL);
    arranque("perfil: empieza (%d s)", dur);
    pid_t yo = gettid(), pid = getpid();
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned rondas = 0;
    for (;;) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) >= dur) break;
        DIR *d = opendir("/proc/self/task");
        if (!d) break;
        struct dirent *de;
        while ((de = readdir(d))) {
            pid_t tid = (pid_t)atoi(de->d_name);
            if (tid <= 0 || tid == yo) continue;
            char r[64], st[256]; snprintf(r, sizeof(r), "/proc/self/task/%d/stat", tid);
            FILE *f = fopen(r, "r"); if (!f) continue;
            size_t n = fread(st, 1, sizeof(st) - 1, f); fclose(f); st[n] = 0;
            char *p = strrchr(st, ')');
            if (p && p[1] == ' ' && p[2] == 'R') syscall(SYS_tgkill, pid, tid, SIGRTMIN + 4);
        }
        closedir(d);
        rondas++;
        struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL);
    }
    usleep(50000);
    uint32_t n = g_n < NMUES ? g_n : NMUES;
    /* hilos */
    static Hilo hs[512]; int nh = 0;
    for (uint32_t i = 0; i < n; i++) {
        int k; for (k = 0; k < nh && hs[k].tid != g_m[i].tid; k++) {}
        if (k == nh && nh < 512) { hs[nh].tid = g_m[i].tid; hs[nh].n = 0; nh++; }
        if (k < 512) hs[k].n++;
    }
    for (int k = 0; k < nh; k++) {
        char r[64]; snprintf(r, sizeof(r), "/proc/self/task/%d/comm", hs[k].tid);
        FILE *f = fopen(r, "r"); hs[k].nom[0] = 0;
        if (f) { if (fgets(hs[k].nom, sizeof(hs[k].nom), f)) hs[k].nom[strcspn(hs[k].nom, "\n")] = 0; fclose(f); }
    }
    qsort(hs, (size_t)nh, sizeof(Hilo), cmp_hilo);
    /* funciones (por hilo y en total) */
    uint32_t cap = 1 << 16;
    Cuenta *c = (Cuenta *)calloc(cap, sizeof(Cuenta)), *tot = (Cuenta *)calloc(cap, sizeof(Cuenta));
    for (uint32_t i = 0; i < n && c && tot; i++) {
        uint64_t k = clave_de(g_m[i].pc);
        uint32_t h = (uint32_t)((k ^ (uint64_t)g_m[i].tid * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 48) & (cap - 1);
        while (c[h].n && (c[h].clave != k || c[h].tid != g_m[i].tid)) h = (h + 1) & (cap - 1);
        c[h].clave = k; c[h].tid = g_m[i].tid; c[h].n++;
        h = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 48) & (cap - 1);
        while (tot[h].n && tot[h].clave != k) h = (h + 1) & (cap - 1);
        tot[h].clave = k; tot[h].n++;
    }
    /* quién llama: el primer marco (PC o retorno) dentro de libmain.so o libjuego.so */
    Dl_info dm, dj; void *pm = dlsym(RTLD_DEFAULT, "port_main"), *pj = dlsym(RTLD_DEFAULT, "vita_func_810056D4");
    uint64_t m0 = 0, m1 = 0, j0 = 0, j1 = 0;
    if (pm && dladdr(pm, &dm)) { m0 = (uint64_t)dm.dli_fbase; m1 = m0 + (64u << 20); }
    if (pj && dladdr(pj, &dj)) { j0 = (uint64_t)dj.dli_fbase; j1 = j0 + (256u << 20); }
    Cuenta *q = (Cuenta *)calloc(cap, sizeof(Cuenta));
    for (uint32_t i = 0; i < n && q; i++) {
        uint64_t a = g_m[i].pc;
        int k = -1;
        while (!((a >= m0 && a < m1) || (a >= j0 && a < j1)) && ++k < NMARCOS) a = g_m[i].ret[k];
        if (k >= NMARCOS) a = 0;
        uint64_t key = a ? clave_de(a) : 0;
        uint32_t h = (uint32_t)((key ^ (uint64_t)g_m[i].tid * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 48) & (cap - 1);
        while (q[h].n && (q[h].clave != key || q[h].tid != g_m[i].tid)) h = (h + 1) & (cap - 1);
        q[h].clave = key; q[h].tid = g_m[i].tid; q[h].n++;
    }
    char ruta[600]; snprintf(ruta, sizeof(ruta), "%s/datos/perfil.txt", pb_android_dir());
    FILE *f = fopen(ruta, "w");
    if (f && c && tot) {
        fprintf(f, "muestras %u en %u rondas de 10 ms (%d s)\n\n== por hilo\n", n, rondas, dur);
        for (int k = 0; k < nh; k++) fprintf(f, "%6u %5.1f%%  %s (%d)\n", hs[k].n, 100.0 * hs[k].n / (n ? n : 1), hs[k].nom, hs[k].tid);
        qsort(tot, cap, sizeof(Cuenta), cmp_cuenta);
        fprintf(f, "\n== funciones (todas)\n");
        for (uint32_t i = 0; i < 80 && tot[i].n; i++) { char b[300]; nombre_de(tot[i].clave, b, sizeof(b)); fprintf(f, "%6u %5.1f%%  %s\n", tot[i].n, 100.0 * tot[i].n / n, b); }
        qsort(c, cap, sizeof(Cuenta), cmp_cuenta);
        for (int k = 0; k < nh && k < 12; k++) {
            fprintf(f, "\n== %s (%d)\n", hs[k].nom, hs[k].tid);
            int m = 0;
            for (uint32_t i = 0; i < cap && c[i].n && m < 25; i++) if (c[i].tid == hs[k].tid) {
                char b[300]; nombre_de(c[i].clave, b, sizeof(b)); fprintf(f, "%6u %5.1f%%  %s\n", c[i].n, 100.0 * c[i].n / hs[k].n, b); m++;
            }
        }
    }
    if (f && q) {
        qsort(q, cap, sizeof(Cuenta), cmp_cuenta);
        for (int k = 0; k < nh && k < 12; k++) {
            fprintf(f, "\n== quién llama (primer marco del port/juego): %s (%d)\n", hs[k].nom, hs[k].tid);
            int m = 0;
            for (uint32_t i = 0; i < cap && q[i].n && m < 12; i++) if (q[i].tid == hs[k].tid) {
                char b[300]; nombre_de(q[i].clave, b, sizeof(b)); fprintf(f, "%6u %5.1f%%  %s\n", q[i].n, 100.0 * q[i].n / hs[k].n, b); m++;
            }
        }
    }
    if (f) {   /* reparto por biblioteca de los hilos principales (dónde está el PC) */
        for (int k = 0; k < nh && k < 8; k++) {
            struct { char lib[48]; unsigned n; } lb[16]; int nl = 0;
            for (uint32_t i = 0; i < n; i++) if (g_m[i].tid == hs[k].tid) {
                Dl_info di; const char *nom = "?";
                if (dladdr((void *)g_m[i].pc, &di) && di.dli_fname) { nom = strrchr(di.dli_fname, '/'); nom = nom ? nom + 1 : di.dli_fname; }
                int j; for (j = 0; j < nl && strcmp(lb[j].lib, nom); j++) {}
                if (j == nl && nl < 16) { snprintf(lb[nl].lib, sizeof(lb[nl].lib), "%s", nom); lb[nl].n = 0; nl++; }
                if (j < 16) lb[j].n++;
            }
            fprintf(f, "\n== bibliotecas: %s (%d):", hs[k].nom, hs[k].tid);
            for (int j = 0; j < nl; j++) fprintf(f, "  %s %.0f%%", lb[j].lib, 100.0 * lb[j].n / (hs[k].n ? hs[k].n : 1));
            fprintf(f, "\n");
        }
    }
    free(q);
    if (f) fclose(f);
    arranque("perfil: %u muestras en datos/perfil.txt", n);
    free(c); free(tot);
    return NULL;
}

void pb_perfil_inicia(void) {
    if (!getenv("PB_PERFIL")) return;
    pthread_t t;
    if (pthread_create(&t, NULL, perfilar, NULL) == 0) pthread_detach(t);
}
