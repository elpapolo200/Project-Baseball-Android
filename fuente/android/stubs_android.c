/* stubs_android.c — funciones de archivos que solo existen en PC (watchdog, recompilación, interfaz
 * Win32...) y que otras partes del port llaman. En Android no hacen nada. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* watchdog.c (PC): vigilante de cuelgues, perfilador y volcado de cierres con SuspendThread /
 * GetThreadContext. En Android no se usan (los cierres los recoge el propio Android en logcat). */
volatile unsigned long port_draw_tid;
void port_watchdog_start(void) {}
void port_perfil_start(void) {}
void port_watch_start(void) {}
void port_watch_thread_start(void) {}
/* Nombre del hilo de la Vita (máx. 15 caracteres en Linux): lo llama hle_kernel.c (parche de Android). */
#include <pthread.h>
/* Núcleos en los que pueden correr los hilos del juego: [android] nucleos o PB_NUCLEOS (máscara en
 * hexadecimal; 0 = todos). En el Snapdragon 8 Gen 1: 0-3 A510, 4-6 A710, 7 X2 (el que más calienta). */
#include <sched.h>
#include <sys/resource.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
void pb_afinidad(void) {
    static long mascara = -1;
    if (mascara < 0) {
        extern int launcher_ini_int(const char *section, const char *key, int def);
        const char *e = getenv("PB_NUCLEOS");
        mascara = e && *e ? strtol(e, NULL, 16) : launcher_ini_int("android", "nucleos", 0);
        if (mascara) printf("[android] núcleos permitidos: 0x%lX\n", mascara);
    }
    if (mascara <= 0) return;
    cpu_set_t cs; CPU_ZERO(&cs);
    for (int c = 0; c < 32; c++) if (mascara >> c & 1) CPU_SET(c, &cs);
    sched_setaffinity(0, sizeof(cs), &cs);
}
/* Reparto de núcleos por papel del hilo. En el partido el hilo principal del juego pasaba ~11 % del
 * tiempo listo pero esperando núcleo (schedstat): los A710/X2 estaban ocupados por los trabajadores
 * (Kinematics, KineCore, audio...). Críticos (principal, «Display Engine», «GPU Hilo") -> [android]
 * nucleos_criticos (por defecto F0 = A710 + X2); el resto -> [android] nucleos_resto (por defecto 0
 * = sin tocar). PB_AFIN_CRITICOS / PB_AFIN_RESTO (hexadecimal) para pruebas. */
static long mascara_de(const char *env, const char *clave, long def) {
    extern int launcher_ini_int(const char *section, const char *key, int def);
    const char *e = getenv(env);
    return e && *e ? strtol(e, NULL, 16) : launcher_ini_int("android", clave, (int)def);
}
static void aplica_mascara(long m) {
    if (m <= 0) return;
    cpu_set_t cs; CPU_ZERO(&cs);
    for (int c = 0; c < 32; c++) if (m >> c & 1) CPU_SET(c, &cs);
    sched_setaffinity(0, sizeof(cs), &cs);
}
void pb_afinidad_nombre(const char *n) {
    static long crit = -1, resto = -1;
    if (crit < 0) {
        crit = mascara_de("PB_AFIN_CRITICOS", "nucleos_criticos", 0xF0);
        resto = mascara_de("PB_AFIN_RESTO", "nucleos_resto", 0);
        printf("[android] núcleos: críticos 0x%lX, resto 0x%lX\n", crit, resto);
    }
    int critico = !n || !strcmp(n, "main") || !strcmp(n, "Display Engine") || !strcmp(n, "GPU Hilo");
    aplica_mascara(critico ? crit : resto);
    /* Prioridad: los críticos por delante de los trabajadores que se despiertan miles de veces por
     * segundo ([android] prioridad_criticos, nice; por defecto -10; PB_PRIO_CRIT para pruebas). */
    {
        static int prio = 1000;
        if (prio == 1000) {
            extern int launcher_ini_int(const char *section, const char *key, int def);
            const char *e = getenv("PB_PRIO_CRIT");
            prio = e && *e ? atoi(e) : launcher_ini_int("android", "prioridad_criticos", -10);
        }
        if (critico && prio) {
            int r = setpriority(PRIO_PROCESS, 0, prio);
            printf("[android] prioridad %d para «%s»: %s\n", prio, n ? n : "principal", r == 0 ? "ok" : "no permitida");
        }
    }
}
void pb_nombre_hilo(const char *n) {
    pb_afinidad();
    pb_afinidad_nombre(n);
    { extern void pb_adpf_registra(int, pid_t); if (n && !strcmp(n, "Display Engine")) pb_adpf_registra(1, gettid()); }
    char b[16]; int i = 0;
    for (; n && n[i] && i < 15; i++) b[i] = n[i];
    b[i] = 0;
    if (i) pthread_setname_np(pthread_self(), b);
}
void port_crash_handler_install(void) {}

/* recompilar.c (PC): en Android libjuego.so llega ya hecho desde el launcher de PC. */
int recompilar_desarrollo(const char *elf) { (void)elf; printf("[android] recompilar: no disponible\n"); return 1; }

/* Importaciones del juego que en PC salen de hle_defaults.lib (armrecomp: «trampa por nombre» si se
 * llaman). En Android solo faltaban estas dos (errores de longitud/rango de la libstdc++ de la Vita). */
#include "vitarecomp/recomp_rt.h"
void vita_hle__ZSt14_Xlength_errorPKc(void) { vita_trap_import(0x81C7C610, 0x2F6A7265, "SceLibstdcxx::_ZSt14_Xlength_errorPKc"); }
void vita_hle__ZSt14_Xout_of_rangePKc(void) { vita_trap_import(0x81C7C620, 0x553E56BD, "SceLibstdcxx::_ZSt14_Xout_of_rangePKc"); }


/* Palancas del mando en pantalla (MandoEnPantalla.java): posición analógica de cada una, en 0..255 con
 * 128 en el centro; -1 = no se está tocando (mandan el mando físico o las teclas). Las lee input.c. */
#include <jni.h>
volatile int pb_tactil_eje[4] = { -1, -1, -1, -1 };   /* lx, ly, rx, ry */
JNIEXPORT void JNICALL Java_org_libsdl_app_MandoEnPantalla_nativoStick(JNIEnv *env, jclass cls, jint cual, jfloat x, jfloat y, jboolean activo) {
    (void)env; (void)cls;
    int b = cual ? 2 : 0;
    if (!activo) { pb_tactil_eje[b] = pb_tactil_eje[b + 1] = -1; return; }
    int vx = (int)(128.0f + x * 127.0f + 0.5f), vy = (int)(128.0f + y * 127.0f + 0.5f);
    pb_tactil_eje[b] = vx < 0 ? 0 : vx > 255 ? 255 : vx;
    pb_tactil_eje[b + 1] = vy < 0 ? 0 : vy > 255 ? 255 : vy;
}
