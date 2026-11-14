/* arranque_android.c — punto de entrada en Android. SDLActivity llama a SDL_main en su hilo; aquí se
 * fija la carpeta pública del port y se llama al main del port (main.c, compilado con
 * -Dmain=port_main) en un hilo con 16 MB de pila, como el hilo principal del exe de PC
 * (/STACK:16777216): el código traducido anida muchas llamadas y la pila del hilo de SDL es pequeña. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <windows.h>
#include <SDL.h>
#include <SDL_main.h>
#include <android/log.h>

int port_main(int argc, char **argv);

static int g_r;
static void *hilo_port(void *x) {
    (void)x;
    char *args[2] = { (char *)"ProjectBaseball", NULL };
    extern void pb_afinidad(void); pb_afinidad();   /* el hilo principal del juego también */
    { extern void pb_afinidad_nombre(const char *); pb_afinidad_nombre(NULL); }   /* hilo principal = crítico */
    { extern void pb_adpf_registra(int, pid_t); pb_adpf_registra(0, gettid()); }
    g_r = port_main(1, args);
    return NULL;
}

/* Ciclo de vida de Android en arranque.txt: si el juego pasa a segundo plano o el sistema avisa de
 * poca memoria, queda apuntado (para saber por qué Android lo cerró). */
extern void arranque(const char *fmt, ...);
extern void gpu_android_ciclo(int estado);   /* gpu_vk.c, parche 37 */
static int SDLCALL ciclo(void *u, SDL_Event *e) {
    (void)u;
    switch (e->type) {
        case SDL_APP_LOWMEMORY:           arranque("android: el sistema avisa de POCA MEMORIA"); break;
        case SDL_APP_WILLENTERBACKGROUND: arranque("android: la app pasa a segundo plano"); gpu_android_ciclo(1); break;
        case SDL_APP_DIDENTERFOREGROUND:  arranque("android: la app vuelve al primer plano"); gpu_android_ciclo(2); break;
        case SDL_APP_TERMINATING:         arranque("android: el sistema cierra la app"); break;
        default: break;
    }
    return 1;
}

int SDL_main(int argc, char **argv) {
    /* Siempre en horizontal: SDL elige la orientación por el tamaño de la ventana que pide el port
     * (calculado con la pantalla en vertical) y la ponía en vertical. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_AddEventWatch(ciclo, NULL);
    const char *ext = getenv("EXTERNAL_STORAGE");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/ProjectBaseball", ext && *ext ? ext : "/storage/emulated/0");
    mkdir(dir, 0777);
    pb_android_set_dir(dir);
    {   /* entorno.txt (pruebas): una variable por línea, NOMBRE=valor, p. ej. MLB15_PAD_GUION=... o
         * MLB15_FPS=0. Es el equivalente a las variables de entorno del PC. */
        char r[600]; snprintf(r, sizeof(r), "%s/entorno.txt", dir);
        FILE *f = fopen(r, "r");
        if (f) {
            static char l[8192];
            while (fgets(l, sizeof(l), f)) {
                l[strcspn(l, "\r\n")] = 0;
                char *eq = strchr(l, '=');
                if (l[0] == '#' || !eq || eq == l) continue;
                *eq = 0; setenv(l, eq + 1, 1);
                __android_log_print(ANDROID_LOG_INFO, "ProjectBaseball", "entorno: %s", l);
            }
            fclose(f);
        }
    }
    /* Multijugador en la misma wifi (OpcionesActivity -> "--multi=host" o "--multi=<ip del anfitrión>"):
     * modo de conexión directa del juego (olHost / olConnect=), ver docs/MULTIJUGADOR.md. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--multi=", 8) != 0) continue;
        const char *m = argv[i] + 8;
        static char args[256];
        int host = strcmp(m, "host") == 0;
        snprintf(args, sizeof(args), host ? "olHost olUser=Local" : "olConnect=%s olUser=Visitante", m);
        setenv("MLB15_EN_LINEA", "1", 1);
        setenv("MLB15_ARGS", args, 1);
        setenv("MLB15_JUGADOR", host ? "Local" : "Visitante", 1);
        if (!host) setenv("MLB15_SERVIDOR", m, 1);
        __android_log_print(ANDROID_LOG_INFO, "ProjectBaseball", "multijugador: %s", args);
    }
    __android_log_print(ANDROID_LOG_INFO, "ProjectBaseball", "carpeta: %s", dir);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 16u << 20);
    pthread_t t;
    if (pthread_create(&t, &a, hilo_port, NULL) == 0) pthread_join(t, NULL);
    else g_r = hilo_port(NULL) == NULL ? g_r : 1;
    pthread_attr_destroy(&a);
    __android_log_print(ANDROID_LOG_INFO, "ProjectBaseball", "fin (%d)", g_r);
    exit(g_r);   /* el proceso de Android se cierra: no se puede volver a entrar a SDL_main en el mismo */
}
