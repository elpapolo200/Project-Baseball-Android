/* launcher_android.c — el launcher del port en Android (sustituye a launcher.c del PC).
 *
 * Todo vive en una carpeta pública del teléfono, para que el jugador pueda copiar sus archivos con
 * cualquier método (cable, Drive, explorador):
 *
 *   /storage/emulated/0/ProjectBaseball/
 *       PCSA00511/            la carpeta del juego (con VITA_COMPRESSED_WAD.arc, en claro)
 *       eboot.elf             el código del juego en claro (lo genera el launcher de PC)
 *       libjuego.so           el código del juego traducido para ARM64 (lo genera el launcher de PC)
 *       ProjectBaseball.ini   opciones (mismas claves que en PC)
 *       datos/                partidas, log.txt, arranque.txt
 *
 * libjuego.so se copia a la memoria interna de la app antes de cargarla: Android no deja ejecutar
 * código desde la memoria compartida (está montada sin permiso de ejecución). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <pthread.h>
#include <ucontext.h>
#include <time.h>
#define SIN_PAC(a) ((uintptr_t)(a) & 0x000000FFFFFFFFFFull)   /* quita la firma PAC de una dirección */
#include <windows.h>
#include <SDL.h>

#include "launcher.h"

static char g_dir[512], g_ini[600], g_dll[600];

static void base(void) {
    if (g_dir[0]) return;
    snprintf(g_dir, sizeof(g_dir), "%s", pb_android_dir());
    snprintf(g_ini, sizeof(g_ini), "%s/ProjectBaseball.ini", g_dir);
}

const char *launcher_juego_dll(void) { return g_dll; }
const char *launcher_ini_path(void) { base(); return g_ini; }
/* Valores por defecto del móvil (si el .ini no dice otra cosa): resolución interna x1. A x3 (lo del
 * PC) las superficies del partido ocupan cientos de MB (el mapa de sombras sería 6144x3072) y
 * Android cerró el juego al cargar el partido en el S22 (primer log, 2026-11-09). */
int launcher_ini_int(const char *section, const char *key, int def) {
    base();
    if (section && key && !strcmp(section, "video") && !strcmp(key, "resolucion")) def = 1;
    if (section && key && !strcmp(section, "video") && !strcmp(key, "pantalla_completa")) def = 1;   /* sin barras del sistema */
    if (section && key && !strcmp(section, "video") && !strcmp(key, "resolucion")) {   /* x1 o x2 en el móvil (x3 calienta demasiado) */
        int r = (int)GetPrivateProfileIntA(section, key, 1, g_ini);
        return r < 1 ? 1 : r > 2 ? 2 : r;
    }
    if (section && key && !strcmp(section, "video") && !strcmp(key, "suavizado")) {   /* a x2 la imagen ya va supermuestreada: sin FXAA */
        int r = (int)GetPrivateProfileIntA("video", "resolucion", 1, g_ini);
        if (r >= 2) return 0;
    }
    if (section && key && !strcmp(section, "video") && !strcmp(key, "fps_max")) {   /* siempre 30 FPS en Android (sin opción) */
        /* con vsync (FIFO, pantalla a 30 Hz) el ritmo lo da la pantalla: el limitador va a 32 solo como red */
        const char *e = getenv("PB_VSYNC");
        int vs = e && *e ? *e != '0' : (int)GetPrivateProfileIntA("android", "vsync", 1, g_ini);
        return vs ? 32 : 30;
    }
    return (int)GetPrivateProfileIntA(section, key, def, g_ini);
}
void launcher_ini_set_int(const char *section, const char *key, int v) {
    char s[32]; base(); snprintf(s, sizeof(s), "%d", v); WritePrivateProfileStringA(section, key, s, g_ini);
}
int opciones_dialogo(void) { return 1; }

void arranque(const char *fmt, ...) {
    char p[700];
    base();
    snprintf(p, sizeof(p), "%s/datos", g_dir); mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/datos/arranque.txt", g_dir);
    FILE *f = fopen(p, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void launcher_console(void) {   /* stdout/stderr -> datos/log.txt */
    char p[700];
    base();
    snprintf(p, sizeof(p), "%s/datos", g_dir); mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/datos/log.txt", g_dir);
    if (freopen(p, "w", stdout)) { dup2(fileno(stdout), fileno(stderr)); setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0); }
}

static int existe(const char *p) { struct stat s; return stat(p, &s) == 0; }
static long long tam(const char *p) { struct stat s; return stat(p, &s) == 0 ? (long long)s.st_size : -1; }
static int aviso(const char *titulo, const char *texto) {
    printf("[launcher] %s: %s\n", titulo, texto);
    arranque("%s: %s", titulo, texto);
    return SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, titulo, texto, NULL);
}
static int copia(const char *de, const char *a) {
    FILE *fi = fopen(de, "rb"); if (!fi) return 0;
    FILE *fo = fopen(a, "wb"); if (!fo) { fclose(fi); return 0; }
    static char buf[1 << 20]; size_t n; int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) if (fwrite(buf, 1, n, fo) != n) { ok = 0; break; }
    fclose(fi); if (fclose(fo)) ok = 0;
    return ok;
}

/* --- Diagnóstico en el teléfono (no hay crash.txt como en PC) ---
 * 1. Fallos (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT): línea en arranque.txt con la dirección, el PC y
 *    el LR traducidos a biblioteca + desplazamiento (libjuego.so / libmain.so), y luego el fallo
 *    normal de Android.
 * 2. Memoria: cada 5 s se mira la memoria del proceso (VmRSS) y la libre del sistema (MemAvailable);
 *    se apunta si cambia más de 64 MB o la libre baja de 600 MB. Si Android mata el juego por falta
 *    de memoria no hay señal que capturar: la última línea de memoria lo delata. */
static void lugar(const char *que, uintptr_t a, char *b, size_t n) {
    Dl_info di;
    if (a && dladdr((void *)a, &di) && di.dli_fname) {
        const char *f = strrchr(di.dli_fname, '/'); f = f ? f + 1 : di.dli_fname;
        if (di.dli_sname && di.dli_saddr)
            snprintf(b, n, "%s=%s+0x%lx (%s+0x%lx)", que, f, (unsigned long)(a - (uintptr_t)di.dli_fbase), di.dli_sname,
                     (unsigned long)(a - (uintptr_t)di.dli_saddr));
        else snprintf(b, n, "%s=%s+0x%lx", que, f, (unsigned long)(a - (uintptr_t)di.dli_fbase));
    } else snprintf(b, n, "%s=0x%lx", que, (unsigned long)a);
}
static void al_fallar(int sig, siginfo_t *si, void *uc) {
    static volatile int dentro;
    if (!dentro++) {
        char pc[200], lr[200]; uintptr_t vpc = 0, vlr = 0;
#if defined(__aarch64__)
        ucontext_t *u = (ucontext_t *)uc; vpc = (uintptr_t)u->uc_mcontext.pc; vlr = SIN_PAC(u->uc_mcontext.regs[30]);
#else
        (void)uc;
#endif
        lugar("pc", vpc, pc, sizeof(pc)); lugar("lr", vlr, lr, sizeof(lr));
        arranque("FALLO: señal %d (código %d) en dirección %p | %s | %s | hilo %ld", sig, si ? si->si_code : 0, si ? si->si_addr : NULL,
                 pc, lr, (long)gettid());
        printf("[fallo] señal %d en %p | %s | %s\n", sig, si ? si->si_addr : NULL, pc, lr);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
static long kb_de(const char *archivo, const char *clave) {
    FILE *f = fopen(archivo, "r"); if (!f) return -1;
    char l[256]; long v = -1; size_t n = strlen(clave);
    while (fgets(l, sizeof(l), f)) if (!strncmp(l, clave, n)) { v = atol(l + n); break; }
    fclose(f); return v;
}
/* Volcado de pilas de todos los hilos cuando el juego se atasca: se manda una señal de tiempo real a
 * cada hilo; su manejador escribe en datos/pilas.txt el nombre del hilo y la pila (PC, LR y
 * cadena de marcos) como biblioteca+desplazamiento. Se traduce en el PC con el .map / nm. */
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
/* Pila por la cadena de punteros de marco (x29): _Unwind_Backtrace dentro del manejador de señal
 * fallaba en Android 16 al autenticar direcciones de retorno firmadas (PAC: SIGILL en autia1716 de
 * libunwind) y cerraba el juego. Aquí solo se leen marcos dentro de la pila del hilo y se quitan los
 * bits de firma de las direcciones. */
static void pila_fp(FILE *f, uintptr_t fp, uintptr_t sp) {
    for (int n = 0; n < 40; n++) {
        if (!fp || (fp & 15) || fp < sp || fp > sp + (16u << 20)) break;
        uintptr_t sig = ((uintptr_t *)fp)[0], ret = SIN_PAC(((uintptr_t *)fp)[1]);
        if (!ret) break;
        char b[200]; lugar("  ", ret, b, sizeof(b)); fprintf(f, "%s\n", b);
        if (sig <= fp) break;
        fp = sig;
    }
}
static volatile int g_pila_hecha;
static void al_pedir_pila(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    char r[700]; snprintf(r, sizeof(r), "%s/datos/pilas.txt", g_dir);
    FILE *f = fopen(r, "a");
    if (f) {
        char nom[32] = { 0 }; prctl(PR_GET_NAME, nom);
        char pc[200], lr[200]; uintptr_t vpc = 0, vlr = 0;
#if defined(__aarch64__)
        ucontext_t *u = (ucontext_t *)uc; vpc = (uintptr_t)u->uc_mcontext.pc; vlr = SIN_PAC(u->uc_mcontext.regs[30]);
#else
        (void)uc;
#endif
        lugar("pc", vpc, pc, sizeof(pc)); lugar("lr", vlr, lr, sizeof(lr));
        fprintf(f, "--- hilo %ld «%s» | %s | %s\n", (long)gettid(), nom, pc, lr);
#if defined(__aarch64__)
        pila_fp(f, (uintptr_t)u->uc_mcontext.regs[29], (uintptr_t)u->uc_mcontext.sp);
#endif
        fclose(f);
    }
    g_pila_hecha = 1;
}
static void volcar_pilas(const char *motivo) {
    char r[700]; snprintf(r, sizeof(r), "%s/datos/pilas.txt", g_dir);
    FILE *f = fopen(r, "a"); if (f) { fprintf(f, "\n===== %s =====\n", motivo); fclose(f); }
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = al_pedir_pila; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 3, &sa, NULL);
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    struct dirent *e; pid_t yo = gettid();
    while ((e = readdir(d))) {
        pid_t t = (pid_t)atoi(e->d_name);
        if (t <= 0 || t == yo) continue;
        char cr[64], nom[64] = { 0 }; snprintf(cr, sizeof(cr), "/proc/self/task/%d/comm", t);
        FILE *c = fopen(cr, "r"); if (c) { if (fgets(nom, sizeof(nom), c)) nom[strcspn(nom, "\n")] = 0; fclose(c); }
        /* hilos de Java/ART y del sistema: no hacen falta (y ART usa sus propias señales) */
        if (!strncmp(nom, "Jit", 3) || !strncmp(nom, "HeapTask", 8) || !strncmp(nom, "Binder", 6) || !strncmp(nom, "binder", 6) ||
            !strncmp(nom, "Signal Catcher", 14) || !strncmp(nom, "FinalizerD", 10) || !strncmp(nom, "ReferenceQueue", 14) ||
            !strncmp(nom, "Profile Saver", 13) || !strncmp(nom, "perfetto", 8) || !strncmp(nom, "ADB-JDWP", 8)) continue;
        g_pila_hecha = 0;
        if (syscall(SYS_tgkill, getpid(), t, SIGRTMIN + 3) == 0)
            for (int k = 0; k < 50 && !g_pila_hecha; k++) usleep(10000);
    }
    closedir(d);
}
extern volatile unsigned pb_vk_presentes, pb_vk_npipes, pb_vk_nmods;
extern volatile double pb_vk_pipe_desde, pb_vk_fence_desde, pb_vk_pipe_ms, pb_vk_pipe_max_ms;
extern volatile long long pb_vk_bytes; extern volatile unsigned pb_vk_nasig;
extern volatile int pb_vk_perdido;
void pb_vk_guarda_cache(void);
static double reloj(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
static void *vigila_memoria(void *x) {
    (void)x; long ult = 0; unsigned seg = 0, pres_ant = 0, pipes_ant = 0; double sin_desde = 0; int volcado = 0, perdido_dicho = 0;
    unsigned pipes_log = 0;
    for (;; seg++) {
        sleep(1);
        double t = reloj();
        if (pb_vk_perdido && !perdido_dicho++) { arranque("FALLO: la GPU se perdió (VK_ERROR_DEVICE_LOST)"); volcar_pilas("GPU perdida"); }
        {   /* volcado a petición (pruebas): crear datos/volcar */
            char vr[600]; snprintf(vr, sizeof(vr), "%s/datos/volcar", g_dir);
            if (access(vr, F_OK) == 0) { unlink(vr); volcar_pilas("a petición"); arranque("pilas volcadas a petición"); }
        }
        /* atasco: 30 s sin fotogramas (cargar el partido en el móvil tarda) */
        unsigned pres = pb_vk_presentes;
        if (pres != pres_ant) { pres_ant = pres; sin_desde = t; volcado = 0; }
        else if (pres && sin_desde && t - sin_desde > 30 && !volcado) {
            volcado = 1;
            double pd = pb_vk_pipe_desde, fd = pb_vk_fence_desde;
            arranque("ATASCO: %.0f s sin fotogramas | %s%s | pipelines %u (media %.1f ms, máx %.0f ms) | memoria GPU %lld MB",
                     t - sin_desde, pd ? "creando una pipeline desde hace " : "", pd ? "" : (fd ? "esperando a la GPU" : "ni pipeline ni GPU"),
                     pb_vk_npipes, pb_vk_npipes ? pb_vk_pipe_ms / pb_vk_npipes : 0.0, pb_vk_pipe_max_ms, pb_vk_bytes >> 20);
            if (pd) arranque("  (la pipeline lleva %.1f s)", t - pd);
            if (fd) arranque("  (la espera a la GPU lleva %.1f s)", t - fd);
            volcar_pilas("ATASCO");
            arranque("  pilas de los hilos en datos/pilas.txt");
        }
        if (pb_vk_npipes - pipes_log >= 200) {
            pipes_log = pb_vk_npipes;
            arranque("pipelines: %u creadas (media %.1f ms, máx %.0f ms), %u shaders", pb_vk_npipes, pb_vk_pipe_ms / pb_vk_npipes, pb_vk_pipe_max_ms, pb_vk_nmods);
        }
        if (seg % 15 == 0 && pb_vk_npipes != pipes_ant) { pipes_ant = pb_vk_npipes; pb_vk_guarda_cache(); }
        if (seg % 5) continue;
        long rss = kb_de("/proc/self/status", "VmRSS:"), libre = kb_de("/proc/meminfo", "MemAvailable:"), swap = kb_de("/proc/self/status", "VmSwap:");
        if (rss >= 0 && (labs(rss - ult) > 64 * 1024 || (libre >= 0 && libre < 600 * 1024))) {
            arranque("memoria: juego %ld MB (en swap %ld MB), GPU %lld MB en %u bloques, libre en el sistema %ld MB",
                     rss >> 10, swap >> 10, pb_vk_bytes >> 20, pb_vk_nasig, libre >> 10);
            ult = rss;
        }
    }
    return NULL;
}
static void diagnostico_android(void) {
    static int hecho; if (hecho++) return;
    static char alt[64 * 1024];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof(alt), .ss_flags = 0 }; sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = al_fallar; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
    pthread_t t; if (pthread_create(&t, NULL, vigila_memoria, NULL) == 0) pthread_detach(t);
    { extern void pb_perfil_inicia(void); pb_perfil_inicia(); }   /* PB_PERFIL=inicio,duración */
}

int launcher_prepare(char *elf_out, size_t max) {
    base();
    char p[700], game[600], texto[1600];
    mkdir(g_dir, 0777);
    snprintf(p, sizeof(p), "%s/datos", g_dir); mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/datos/arranque.txt", g_dir); unlink(p);
    {
        MEMORYSTATUSEX ms = { sizeof(ms) }; GlobalMemoryStatusEx(&ms);
        SYSTEM_INFO si; GetSystemInfo(&si);
        arranque("=== Project Baseball Android (%s) | %lu núcleos | RAM %llu MB (libre %llu MB) | carpeta %s", __DATE__,
                 (unsigned long)si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPhys >> 20), g_dir);
        arranque("memoria disponible (MemAvailable): %ld MB", kb_de("/proc/meminfo", "MemAvailable:") >> 10);
    }
    diagnostico_android();
    {   /* Partidas guardadas por versiones anteriores en ProjectBaseball/ProjectBaseball/datos (ruta doble):
         * se mueven a ProjectBaseball/datos sin pisar nada. */
        char viejo[700], nuevo[700], a[1400], b[1400];
        snprintf(viejo, sizeof(viejo), "%s/ProjectBaseball/datos", g_dir);
        snprintf(nuevo, sizeof(nuevo), "%s/datos", g_dir);
        DIR *d = opendir(viejo);
        if (d) {
            struct dirent *e; int n = 0;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                snprintf(a, sizeof(a), "%s/%s", viejo, e->d_name);
                snprintf(b, sizeof(b), "%s/%s", nuevo, e->d_name);
                if (!existe(b) && rename(a, b) == 0) n++;
            }
            closedir(d);
            if (n) arranque("partidas: %d carpetas movidas de ProjectBaseball/ProjectBaseball/datos a datos", n);
        }
    }
    if (access(g_dir, R_OK | W_OK) != 0) {
        snprintf(texto, sizeof(texto), "No puedo usar la carpeta %s.\n\nDale a Project Baseball el permiso «Acceso a todos los archivos» "
                 "(Ajustes > Aplicaciones > Project Baseball > Permisos) y vuelve a abrirla.", g_dir);
        aviso("Project Baseball", texto);
        return 1;
    }
    /* 1. carpeta del juego */
    GetPrivateProfileStringA("juego", "carpeta", "", game, sizeof(game), g_ini);
    snprintf(p, sizeof(p), "%s/VITA_COMPRESSED_WAD.arc", game);
    if (!game[0] || !existe(p)) {
        snprintf(game, sizeof(game), "%s/PCSA00511", g_dir);
        snprintf(p, sizeof(p), "%s/VITA_COMPRESSED_WAD.arc", game);
    }
    if (!existe(p)) {
        snprintf(texto, sizeof(texto), "Falta la carpeta del juego.\n\nCopia tu carpeta PCSA00511 (la de VITA_COMPRESSED_WAD.arc, "
                 "descifrada: la de Vita3K o VitaShell «Open decrypted») dentro de:\n%s\n\nLuego vuelve a abrir Project Baseball.", g_dir);
        aviso("Project Baseball", texto);
        return 1;
    }
    {   /* .arc cifrado = pantalla negra: avisar (igual que en PC) */
        FILE *f = fopen(p, "rb"); char m[4] = { 0 };
        if (f) { fread(m, 1, 4, f); fclose(f); }
        if (memcmp(m, "PSAR", 4)) {
            snprintf(texto, sizeof(texto), "Los archivos del juego de %s están CIFRADOS.\n\nUsa la carpeta de Vita3K o una copiada con VitaShell «Open decrypted».", game);
            aviso("Project Baseball", texto);
            return 1;
        }
    }
    WritePrivateProfileStringA("juego", "carpeta", game, g_ini);
    /* 2. eboot.elf */
    snprintf(elf_out, max, "%s/eboot.elf", g_dir);
    if (tam(elf_out) < 1000000) {
        snprintf(elf_out, max, "%s/datos/eboot.elf", g_dir);
        if (tam(elf_out) < 1000000) {
            snprintf(texto, sizeof(texto), "Falta eboot.elf.\n\nLo crea el launcher de PC (botón «Crear versión para Android») junto con "
                     "libjuego.so. Copia los dos a:\n%s", g_dir);
            aviso("Project Baseball", texto);
            return 1;
        }
    }
    /* 3. libjuego.so -> memoria interna (la compartida no permite ejecutar código) */
    char so[700];
    snprintf(so, sizeof(so), "%s/libjuego.so", g_dir);
    if (!existe(so)) {
        snprintf(texto, sizeof(texto), "Falta libjuego.so (el juego traducido para tu teléfono).\n\nLo crea el launcher de PC a partir de tu "
                 "copia del juego (botón «Crear versión para Android»). Cópialo a:\n%s", g_dir);
        aviso("Project Baseball", texto);
        return 1;
    }
    const char *interna = SDL_AndroidGetInternalStoragePath();
    snprintf(g_dll, sizeof(g_dll), "%s/libjuego.so", interna ? interna : g_dir);
    if (tam(g_dll) != tam(so)) {
        printf("[launcher] copiando libjuego.so a %s\n", g_dll);
        if (!copia(so, g_dll)) { aviso("Project Baseball", "No se pudo copiar libjuego.so a la memoria de la app."); return 1; }
    }
    setenv("MLB15_APP0", game, 1);
    printf("[launcher] juego: %s\n[launcher] elf: %s\n[launcher] libjuego: %s\n", game, elf_out, g_dll);
    arranque("juego: %s | elf: %s | libjuego: %s (%lld bytes)", game, elf_out, g_dll, tam(g_dll));
    return 0;
}
