/* compat_win.c — implementación para Android del subconjunto de Win32 de include/windows.h. */
#define _GNU_SOURCE
#include <windows.h>
#undef fopen
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ProjectBaseball", __VA_ARGS__)

/* --- errores --- */
static __thread DWORD t_err;
DWORD GetLastError(void) { return t_err; }
void SetLastError(DWORD e) { t_err = e; }

/* --- carpeta de la app --- */
static char g_dir[512] = ".";
void pb_android_set_dir(const char *dir) { if (dir && *dir) snprintf(g_dir, sizeof(g_dir), "%s", dir); }
const char *pb_android_dir(void) { return g_dir; }

/* --- tiempo --- */
static void a_systemtime(SYSTEMTIME *st, const struct tm *t, long ms) {
    st->wYear = (WORD)(t->tm_year + 1900); st->wMonth = (WORD)(t->tm_mon + 1); st->wDayOfWeek = (WORD)t->tm_wday;
    st->wDay = (WORD)t->tm_mday; st->wHour = (WORD)t->tm_hour; st->wMinute = (WORD)t->tm_min; st->wSecond = (WORD)t->tm_sec;
    st->wMilliseconds = (WORD)ms;
}
void GetLocalTime(SYSTEMTIME *st) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm t; localtime_r(&ts.tv_sec, &t); a_systemtime(st, &t, ts.tv_nsec / 1000000);
}
void GetSystemTime(SYSTEMTIME *st) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm t; gmtime_r(&ts.tv_sec, &t); a_systemtime(st, &t, ts.tv_nsec / 1000000);
}
void GetSystemTimeAsFileTime(FILETIME *ft) {   /* unidades de 100 ns desde 1601 */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ULONGLONG v = ((ULONGLONG)ts.tv_sec + 11644473600ULL) * 10000000ULL + (ULONGLONG)(ts.tv_nsec / 100);
    ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
}
static void a_filetime(FILETIME *ft, time_t s) {
    ULONGLONG v = ((ULONGLONG)s + 11644473600ULL) * 10000000ULL;
    ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
}
static void abs_ms(struct timespec *t, DWORD ms) {   /* ahora (CLOCK_MONOTONIC) + ms */
    clock_gettime(CLOCK_MONOTONIC, t);
    t->tv_sec += ms / 1000; t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
}

/* --- secciones críticas --- */
void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a); pthread_mutexattr_destroy(&a); cs->ini = 1;
}
void DeleteCriticalSection(CRITICAL_SECTION *cs) { if (cs->ini) pthread_mutex_destroy(&cs->m); cs->ini = 0; }
static pthread_mutex_t g_ini_mx = PTHREAD_MUTEX_INITIALIZER;
static void cs_asegura(CRITICAL_SECTION *cs) {   /* por si se usa sin Initialize (estática a ceros) */
    if (__atomic_load_n(&cs->ini, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&g_ini_mx);
    if (!cs->ini) InitializeCriticalSection(cs);
    pthread_mutex_unlock(&g_ini_mx);
}
void EnterCriticalSection(CRITICAL_SECTION *cs) { cs_asegura(cs); pthread_mutex_lock(&cs->m); }
BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) { cs_asegura(cs); return pthread_mutex_trylock(&cs->m) == 0; }
void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(&cs->m); }
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms) {
    if (ms == INFINITE) { pthread_cond_wait(&c->c, &cs->m); return TRUE; }
    struct timespec t; abs_ms(&t, ms);
    int r = pthread_cond_timedwait_monotonic_np(&c->c, &cs->m, &t);
    if (r == ETIMEDOUT) { t_err = ERROR_TIMEOUT; return FALSE; }
    return TRUE;
}

/* --- objetos con espera --- */
enum { OB_HILO = 1, OB_EVENTO, OB_SEMAFORO, OB_TEMPORIZADOR, OB_PSEUDO, OB_BUSQUEDA, OB_ARCHIVO };
typedef struct Obj {
    int tipo;
    pthread_mutex_t m; pthread_cond_t c;
    int senal, manual;            /* evento */
    LONG cuenta, maximo;          /* semáforo */
    struct timespec vence;        /* temporizador */
    pthread_t hilo; int acabado, unido; DWORD codigo; LPTHREAD_START_ROUTINE fn; void *arg;   /* hilo */
    DIR *dir; char patron[MAX_PATH]; char base[MAX_PATH];   /* búsqueda de archivos */
    int fd;                       /* archivo */
} Obj;
static Obj *obj_nuevo(int tipo) {
    Obj *o = (Obj *)calloc(1, sizeof(Obj));
    if (!o) return NULL;
    o->tipo = tipo;
    pthread_mutex_init(&o->m, NULL);
    pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&o->c, &ca); pthread_condattr_destroy(&ca);
    o->fd = -1;
    return o;
}
static int espera(Obj *o, DWORD ms) {   /* espera con o->m tomado; 0 = señal, ETIMEDOUT */
    if (ms == INFINITE) return pthread_cond_wait(&o->c, &o->m);
    struct timespec t; abs_ms(&t, ms);
    return pthread_cond_timedwait(&o->c, &o->m, &t);
}

static void *hilo_arranque(void *p) {
    Obj *o = (Obj *)p;
    DWORD r = o->fn(o->arg);
    pthread_mutex_lock(&o->m); o->codigo = r; o->acabado = 1; pthread_cond_broadcast(&o->c); pthread_mutex_unlock(&o->m);
    return NULL;
}
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T pila, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD id) {
    (void)sa; (void)flags;
    Obj *o = obj_nuevo(OB_HILO);
    if (!o) return NULL;
    o->fn = fn; o->arg = arg; o->codigo = STILL_ACTIVE;
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, pila ? (pila < 65536 ? 65536 : pila) : (8u << 20));
    int r = pthread_create(&o->hilo, &a, hilo_arranque, o);
    pthread_attr_destroy(&a);
    if (r) { free(o); return NULL; }
    if (id) *id = 0;
    return o;
}
__attribute__((noreturn)) void ExitThread(DWORD code) { (void)code; pthread_exit(NULL); }
BOOL GetExitCodeThread(HANDLE h, LPDWORD code) {
    Obj *o = (Obj *)h;
    if (!o || o->tipo != OB_HILO) return FALSE;
    pthread_mutex_lock(&o->m); *code = o->acabado ? o->codigo : STILL_ACTIVE; pthread_mutex_unlock(&o->m);
    return TRUE;
}
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL inicial, LPCSTR nombre) {
    (void)sa; (void)nombre;
    Obj *o = obj_nuevo(OB_EVENTO);
    if (o) { o->manual = manual; o->senal = inicial; }
    return o;
}
BOOL SetEvent(HANDLE h) {
    Obj *o = (Obj *)h; if (!o) return FALSE;
    pthread_mutex_lock(&o->m); o->senal = 1; pthread_cond_broadcast(&o->c); pthread_mutex_unlock(&o->m); return TRUE;
}
BOOL ResetEvent(HANDLE h) {
    Obj *o = (Obj *)h; if (!o) return FALSE;
    pthread_mutex_lock(&o->m); o->senal = 0; pthread_mutex_unlock(&o->m); return TRUE;
}
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG inicial, LONG maximo, LPCSTR nombre) {
    (void)sa; (void)nombre;
    Obj *o = obj_nuevo(OB_SEMAFORO);
    if (o) { o->cuenta = inicial; o->maximo = maximo; }
    return o;
}
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *antes) {
    Obj *o = (Obj *)h; if (!o) return FALSE;
    pthread_mutex_lock(&o->m);
    if (antes) *antes = o->cuenta;
    if (o->cuenta + n > o->maximo) { pthread_mutex_unlock(&o->m); return FALSE; }
    o->cuenta += n; pthread_cond_broadcast(&o->c); pthread_mutex_unlock(&o->m); return TRUE;
}
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR nombre, DWORD flags, DWORD acceso) {
    (void)sa; (void)nombre; (void)flags; (void)acceso;
    return obj_nuevo(OB_TEMPORIZADOR);
}
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG periodo, void *rutina, void *arg, void *razon, ULONG tolerancia) {
    (void)periodo; (void)rutina; (void)arg; (void)razon; (void)tolerancia;
    Obj *o = (Obj *)h; if (!o) return FALSE;
    LONGLONG d = due->QuadPart;   /* negativo = relativo, en 100 ns */
    clock_gettime(CLOCK_MONOTONIC, &o->vence);
    if (d < 0) {
        LONGLONG ns = -d * 100;
        o->vence.tv_sec += (time_t)(ns / 1000000000LL); o->vence.tv_nsec += (long)(ns % 1000000000LL);
        if (o->vence.tv_nsec >= 1000000000L) { o->vence.tv_sec++; o->vence.tv_nsec -= 1000000000L; }
    }
    o->senal = 0;
    return TRUE;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    Obj *o = (Obj *)h;
    if (!o || o == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    if (o->tipo == OB_PSEUDO) return WAIT_TIMEOUT;
    if (o->tipo == OB_TEMPORIZADOR) {   /* se duerme hasta el vencimiento (alta resolución) */
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &o->vence, NULL) == EINTR) {}
        return WAIT_OBJECT_0;
    }
    pthread_mutex_lock(&o->m);
    int r = 0;
    for (;;) {
        if (o->tipo == OB_HILO && o->acabado) break;
        if (o->tipo == OB_EVENTO && o->senal) { if (!o->manual) o->senal = 0; break; }
        if (o->tipo == OB_SEMAFORO && o->cuenta > 0) { o->cuenta--; break; }
        if (ms == 0) { r = ETIMEDOUT; break; }
        r = espera(o, ms);
        if (r == ETIMEDOUT) {   /* comprobar una última vez */
            if ((o->tipo == OB_HILO && o->acabado) || (o->tipo == OB_EVENTO && o->senal) || (o->tipo == OB_SEMAFORO && o->cuenta > 0)) continue;
            break;
        }
    }
    pthread_mutex_unlock(&o->m);
    return r == ETIMEDOUT ? WAIT_TIMEOUT : WAIT_OBJECT_0;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL todos, DWORD ms) {
    /* sencillo: sondeo (solo lo usa la recompilación del PC, que no va en Android) */
    ULONGLONG fin = ms == INFINITE ? ~0ULL : GetTickCount64() + ms;
    for (;;) {
        DWORD listos = 0;
        for (DWORD i = 0; i < n; i++)
            if (WaitForSingleObject(hs[i], 0) == WAIT_OBJECT_0) { if (!todos) return WAIT_OBJECT_0 + i; listos++; }
        if (todos && listos == n) return WAIT_OBJECT_0;
        if (GetTickCount64() >= fin) return WAIT_TIMEOUT;
        Sleep(1);
    }
}
BOOL CloseHandle(HANDLE h) {
    Obj *o = (Obj *)h;
    if (!o || o == INVALID_HANDLE_VALUE || o->tipo == OB_PSEUDO) return TRUE;
    if (o->tipo == OB_ARCHIVO) { if (o->fd >= 0) close(o->fd); free(o); return TRUE; }
    if (o->tipo == OB_HILO) {   /* el hilo puede seguir: se suelta sin liberar (lo libera nadie; son pocos) */
        pthread_detach(o->hilo);
        return TRUE;
    }
    pthread_mutex_destroy(&o->m); pthread_cond_destroy(&o->c); free(o);
    return TRUE;
}
DWORD GetCurrentThreadId(void) { return (DWORD)gettid(); }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
static Obj g_pseudo = { OB_PSEUDO };
HANDLE GetCurrentThread(void) { return &g_pseudo; }
HANDLE GetCurrentProcess(void) { return &g_pseudo; }
BOOL DuplicateHandle(HANDLE p1, HANDLE h, HANDLE p2, HANDLE *out, DWORD acceso, BOOL hereda, DWORD op) {
    (void)p1; (void)p2; (void)acceso; (void)hereda; (void)op;
    *out = h;
    return TRUE;
}
__attribute__((noreturn)) void ExitProcess(UINT code) { LOGI("ExitProcess(%u)", code); fflush(NULL); _exit((int)code); }

/* --- sistema --- */
void GetSystemInfo(SYSTEM_INFO *si) { si->dwNumberOfProcessors = (DWORD)sysconf(_SC_NPROCESSORS_ONLN); si->dwPageSize = 4096; }
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *m) {
    struct sysinfo s; if (sysinfo(&s)) return FALSE;
    memset(m, 0, sizeof(*m)); m->dwLength = sizeof(*m);
    m->ullTotalPhys = (ULONGLONG)s.totalram * s.mem_unit; m->ullAvailPhys = (ULONGLONG)(s.freeram + s.bufferram) * s.mem_unit;
    m->ullTotalVirtual = m->ullAvailVirtual = 1ULL << 40;
    return TRUE;
}
DWORD GetModuleFileNameA(HMODULE mod, LPSTR buf, DWORD tam) {
    (void)mod;
    /* separador «\» antes del nombre: el port (vfs.c, texrep.c...) recorta el ejecutable buscando la
     * barra de Windows; con «/» las partidas acababan en ProjectBaseball/ProjectBaseball/datos. */
    int n = snprintf(buf, tam, "%s\\ProjectBaseball", g_dir);
    return n < 0 ? 0 : (DWORD)n;
}
DWORD GetCurrentDirectoryA(DWORD tam, LPSTR buf) { int n = snprintf(buf, tam, "%s", g_dir); return n < 0 ? 0 : (DWORD)n; }
HMODULE GetModuleHandleA(LPCSTR nombre) { return nombre ? dlopen(nombre, RTLD_NOLOAD | RTLD_NOW) : dlopen(NULL, RTLD_NOW); }
/* --- Vulkan: el port carga "vulkan-1.dll" (Windows); aquí es libvulkan.so. Además se intercepta la
 *     consulta de capacidades de la superficie: el S22 (y casi todos los móviles) es vertical de
 *     fábrica y el juego va en horizontal. Se le dice al port que la superficie no está girada
 *     (preTransform IDENTITY, con el tamaño en horizontal) y Android hace el giro al componer.
 *     Sin esto la imagen saldría girada 90 grados. --- */
#include <vulkan/vulkan.h>
#include <jni.h>
#include <SDL_system.h>
static void *g_vk_lib;
static PFN_vkGetInstanceProcAddr g_vk_gipa;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_vk_caps;
static int opcion_android_caps(void);
static int opcion_android(const char *clave, const char *env, int def);
static VkResult VKAPI_CALL pb_caps(VkPhysicalDevice d, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    VkResult r = g_vk_caps(d, s, c);
    if (r != VK_SUCCESS) return r;
    static int dicho;
    VkSurfaceTransformFlagBitsKHR t = c->currentTransform;
    int girada = (t & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) != 0;
    if (c->currentExtent.width != 0xFFFFFFFFu && c->currentExtent.width < c->currentExtent.height) {   /* vertical -> horizontal */
        uint32_t w = c->currentExtent.width; c->currentExtent.width = c->currentExtent.height; c->currentExtent.height = w;
        w = c->minImageExtent.width; c->minImageExtent.width = c->minImageExtent.height; c->minImageExtent.height = w;
        w = c->maxImageExtent.width; c->maxImageExtent.width = c->maxImageExtent.height; c->maxImageExtent.height = w;
    }
    if (c->supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    {   static int vs = -1; if (vs < 0) vs = opcion_android("vsync", "PB_VSYNC", 1) != 0;
        if (vs && c->minImageCount >= 2) c->maxImageCount = c->minImageCount;   /* FIFO: cadena mínima (menos retraso) */
    }
    {   /* salida más pequeña: la escala el compositor (ver opcion_android) */
        static int alto = -1;
        if (alto < 0) alto = opcion_android_caps();
        if (alto > 0 && c->currentExtent.width != 0xFFFFFFFFu && (uint32_t)alto < c->currentExtent.height) {
            uint32_t w = (uint32_t)((uint64_t)c->currentExtent.width * (uint32_t)alto / c->currentExtent.height) & ~1u;
            c->currentExtent.width = w; c->currentExtent.height = (uint32_t)alto;
            if (c->minImageExtent.width > w) c->minImageExtent.width = w;
            if (c->minImageExtent.height > (uint32_t)alto) c->minImageExtent.height = (uint32_t)alto;
        }
    }
    if (!dicho++) LOGI("superficie: transformación 0x%X (%s) -> identidad, %ux%u", (unsigned)t, girada ? "girada" : "sin girar",
                       c->currentExtent.width, c->currentExtent.height);
    return r;
}
/* Con preTransform IDENTITY en una pantalla girada, Android contesta VK_SUBOPTIMAL_KHR a cada
 * vkAcquireNextImageKHR / vkQueuePresentKHR y el port rehacía la swapchain en casi cada fotograma
 * (1 433 veces en el primer log del S22, perdiendo fotogramas y memoria). La imagen es correcta
 * (Android gira al componer), así que SUBOPTIMAL se trata como SUCCESS; OUT_OF_DATE sigue igual. */
static double ahora_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
volatile unsigned pb_vk_presentes, pb_vk_npipes, pb_vk_nmods;
volatile double pb_vk_pipe_desde, pb_vk_fence_desde, pb_vk_pipe_ms, pb_vk_pipe_max_ms, pb_vk_ultimo_present;
volatile long long pb_vk_bytes; volatile unsigned pb_vk_nasig;
volatile int pb_vk_perdido;
/* --- Ahorro de energía en el móvil ---
 * (la frecuencia de la pantalla, [android] hz, la pide ProjectBaseballActivity.java)
 * salida_alto: alto de la imagen que genera el port (0 = el de la pantalla). El pase final (FXAA +
 *              escalado + nitidez) se hace a ese tamaño y el compositor del teléfono la escala por
 *              hardware al tamaño real.
 * Se leen de ProjectBaseball.ini [android] o de PB_HZ / PB_SALIDA_ALTO (pruebas). */
UINT GetPrivateProfileIntA(LPCSTR sec, LPCSTR clave, INT def, LPCSTR archivo);
static int opcion_android(const char *clave, const char *env, int def) {
    const char *e = getenv(env);
    if (e && *e) return atoi(e);
    char ini[600]; snprintf(ini, sizeof(ini), "%s/ProjectBaseball.ini", g_dir);
    return (int)GetPrivateProfileIntA("android", clave, def, ini);
}
static int opcion_android_caps(void) { return opcion_android("salida_alto", "PB_SALIDA_ALTO", 0); }
static PFN_vkGetDeviceProcAddr g_vk_gdpa;
static PFN_vkAcquireNextImageKHR g_vk_acq;
static PFN_vkQueuePresentKHR g_vk_pres;
static unsigned g_subopt;
static VkResult VKAPI_CALL pb_acq(VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore se, VkFence f, uint32_t *i) {
    VkResult r = g_vk_acq(d, s, t, se, f, i);
    if (r == VK_SUBOPTIMAL_KHR) { if (!g_subopt++) LOGI("swapchain: SUBOPTIMAL ignorado (pantalla girada por Android)"); r = VK_SUCCESS; }
    return r;
}
static int g_swappy_ok;   /* Swappy iniciado para la swapchain actual */
#include <stdbool.h>
#include <swappy/swappyVk.h>
static VkResult VKAPI_CALL pb_pres(VkQueue q, const VkPresentInfoKHR *pi) {
    VkResult r = g_swappy_ok ? SwappyVk_queuePresent(q, pi) : g_vk_pres(q, pi);
    if (r == VK_SUBOPTIMAL_KHR) { g_subopt++; r = VK_SUCCESS; }
    if (r == VK_ERROR_DEVICE_LOST) pb_vk_perdido = 1;
    {   /* Ritmo real de la pantalla (lo que ve el jugador): Swappy devuelve la presentación alineada con el
         * refresco, así que el tiempo entre presentaciones es lo que dura cada fotograma en pantalla.
         * Cada 5 s: "[pantalla] N fotos: 33 ms a, 50 ms b, >=66 ms c" (b y c = tirones visibles). */
        static double ult, t5; static unsigned n, h33, h50, h66;
        double t = ahora_s();
        if (ult > 0) {
            double ms = (t - ult) * 1000.0;
            n++; if (ms < 41.7) h33++; else if (ms < 58.3) h50++; else h66++;
        }
        ult = t;
        if (t5 == 0) t5 = t;
        if (t - t5 >= 5.0 && n) {
            extern volatile double pb_psarc_ms, pb_psarc_max; extern volatile unsigned pb_psarc_n; extern volatile unsigned long long pb_psarc_bytes;
            printf("[pantalla] %u fotos: 33 ms %u, 50 ms %u, >=66 ms %u (%.1f %% con tirón) | WAD: %u lecturas, %.1f MB, %.0f ms, peor %.1f ms\n",
                   n, h33, h50, h66, 100.0 * (h50 + h66) / n, pb_psarc_n, pb_psarc_bytes / 1048576.0, pb_psarc_ms, pb_psarc_max);
            pb_psarc_ms = pb_psarc_max = 0; pb_psarc_n = 0; pb_psarc_bytes = 0;
            n = h33 = h50 = h66 = 0; t5 = t;
        }
    }
    pb_vk_presentes++; pb_vk_ultimo_present = ahora_s();
    return r;
}
/* --- Estado de Vulkan para el vigilante de launcher_android.c (atascos, memoria, pipelines) y caché
 *     de pipelines en disco: en Adreno crear una pipeline cuesta ~7 ms (en la NVIDIA del PC casi nada) y
 *     al cargar el partido se crean cientos; con la caché el segundo arranque se las salta. --- */
static PFN_vkCreateGraphicsPipelines g_vk_cgp;
static PFN_vkAllocateMemory g_vk_alloc;
static PFN_vkFreeMemory g_vk_free;
static PFN_vkWaitForFences g_vk_wff;
static PFN_vkQueueSubmit g_vk_submit;
static PFN_vkCreateShaderModule g_vk_csm;
static VkDevice g_vk_dev;
static VkPipelineCache g_vk_cache;
static pthread_mutex_t g_vk_mx = PTHREAD_MUTEX_INITIALIZER;
#define ASIG_N 65536
static struct { uint64_t h; VkDeviceSize n; } g_asig[ASIG_N];
static void cache_ruta(char *b, size_t n) { snprintf(b, n, "%s/datos/shaders_cache.bin", g_dir); }
static void cache_crear(VkDevice d) {
    if (g_vk_cache || !g_vk_gdpa) return;
    PFN_vkCreatePipelineCache cpc = (PFN_vkCreatePipelineCache)g_vk_gdpa(d, "vkCreatePipelineCache");
    if (!cpc) return;
    char r[600]; cache_ruta(r, sizeof(r));
    void *datos = NULL; size_t tam = 0;
    FILE *f = fopen(r, "rb");
    if (f) { fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
             if (l > 0 && (datos = malloc((size_t)l)) && fread(datos, 1, (size_t)l, f) == (size_t)l) tam = (size_t)l; fclose(f); }
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = tam; ci.pInitialData = tam ? datos : NULL;
    if (cpc(d, &ci, NULL, &g_vk_cache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; cpc(d, &ci, NULL, &g_vk_cache); }
    free(datos);
    g_vk_dev = d;
    LOGI("caché de pipelines: %zu bytes leídos de %s", tam, r);
}
/* Guarda la caché si se crearon pipelines nuevas desde la última vez (lo llama el vigilante). */
void pb_vk_guarda_cache(void) {
    static unsigned guardadas;
    if (!g_vk_cache || pb_vk_npipes == guardadas) return;
    PFN_vkGetPipelineCacheData gd = (PFN_vkGetPipelineCacheData)g_vk_gdpa(g_vk_dev, "vkGetPipelineCacheData");
    if (!gd) return;
    size_t n = 0;
    if (gd(g_vk_dev, g_vk_cache, &n, NULL) != VK_SUCCESS || !n) return;
    void *b = malloc(n); if (!b) return;
    if (gd(g_vk_dev, g_vk_cache, &n, b) == VK_SUCCESS) {
        char r[600], t[620]; cache_ruta(r, sizeof(r)); snprintf(t, sizeof(t), "%s.tmp", r);
        FILE *f = fopen(t, "wb");
        if (f) { int ok = fwrite(b, 1, n, f) == n; if (fclose(f)) ok = 0; if (ok) rename(t, r); else unlink(t); }
        guardadas = pb_vk_npipes;
    }
    free(b);
}
static VkResult VKAPI_CALL pb_cgp(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                  const VkAllocationCallbacks *a, VkPipeline *p) {
    if (!g_vk_cache) { pthread_mutex_lock(&g_vk_mx); cache_crear(d); pthread_mutex_unlock(&g_vk_mx); }
    double t0 = ahora_s();
    pb_vk_pipe_desde = t0;
    VkResult r = g_vk_cgp(d, c ? c : g_vk_cache, n, ci, a, p);
    double ms = (ahora_s() - t0) * 1000.0;
    pb_vk_pipe_desde = 0;
    pb_vk_npipes += n; pb_vk_pipe_ms += ms;
    if (ms > pb_vk_pipe_max_ms) pb_vk_pipe_max_ms = ms;
    if (r == VK_ERROR_DEVICE_LOST) pb_vk_perdido = 1;
    return r;
}
static VkResult VKAPI_CALL pb_alloc(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *m) {
    VkResult r = g_vk_alloc(d, ai, a, m);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_vk_mx);
        uint64_t h = (uint64_t)*m;
        for (uint32_t i = (uint32_t)(h * 0x9E3779B97F4A7C15ull >> 48), k = 0; k < ASIG_N; i = (i + 1) & (ASIG_N - 1), k++)
            if (g_asig[i].h <= 1) { g_asig[i].h = h; g_asig[i].n = ai->allocationSize; break; }
        pb_vk_bytes += (long long)ai->allocationSize; pb_vk_nasig++;
        pthread_mutex_unlock(&g_vk_mx);
    } else LOGI("vkAllocateMemory(%llu) = %d", (unsigned long long)ai->allocationSize, (int)r);
    return r;
}
static void VKAPI_CALL pb_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    if (m) {
        pthread_mutex_lock(&g_vk_mx);
        uint64_t h = (uint64_t)m;
        for (uint32_t i = (uint32_t)(h * 0x9E3779B97F4A7C15ull >> 48), k = 0; k < ASIG_N && g_asig[i].h; i = (i + 1) & (ASIG_N - 1), k++)
            if (g_asig[i].h == h) {   /* borrado con reubicación sencilla: se marca con h=1 (lápida) */
                pb_vk_bytes -= (long long)g_asig[i].n; pb_vk_nasig--; g_asig[i].h = 1; g_asig[i].n = 0; break;
            }
        pthread_mutex_unlock(&g_vk_mx);
    }
    g_vk_free(d, m, a);
}
static VkResult VKAPI_CALL pb_wff(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) {
    pb_vk_fence_desde = ahora_s();
    VkResult r = g_vk_wff(d, n, f, all, t);
    pb_vk_fence_desde = 0;
    if (r == VK_ERROR_DEVICE_LOST) pb_vk_perdido = 1;
    return r;
}
static VkResult VKAPI_CALL pb_submit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    VkResult r = g_vk_submit(q, n, s, f);
    if (r == VK_ERROR_DEVICE_LOST) pb_vk_perdido = 1;
    return r;
}
static VkResult VKAPI_CALL pb_csm(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    pb_vk_nmods++;
    return g_vk_csm(d, ci, a, m);
}
static PFN_vkGetPhysicalDeviceMemoryProperties g_vk_gpmp;
static void VKAPI_CALL pb_gpmp(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *m) {
    g_vk_gpmp(d, m);
    static int dicho; if (dicho++) return;
    extern void arranque(const char *fmt, ...);
    char b[600]; int n = 0;
    for (uint32_t i = 0; i < m->memoryTypeCount && n < 560; i++) {
        VkMemoryPropertyFlags f = m->memoryTypes[i].propertyFlags;
        n += snprintf(b + n, sizeof(b) - (size_t)n, " %u:%s%s%s%s(h%u)", i, f & 1 ? "D" : "", f & 2 ? "V" : "", f & 4 ? "C" : "", f & 8 ? "K" : "", m->memoryTypes[i].heapIndex);
    }
    arranque("vulkan: tipos de memoria (D=local V=visible C=coherente K=caché):%s", b);
}
/* Vsync (FIFO) en el móvil: con «mailbox» y el limitador del port, un fotograma que llegaba 2-3 ms tarde
 * a su refresco de 30 Hz hacía que la pantalla repitiera el anterior (tirones con 29,9 FPS de media).
 * Con FIFO la pantalla muestra uno por refresco y la cola absorbe esas variaciones; la cadena se deja
 * en el mínimo de imágenes del driver para no añadir retraso a los controles. [android] vsync=0 o
 * PB_VSYNC=0 vuelve a mailbox. */
static PFN_vkGetPhysicalDeviceSurfacePresentModesKHR g_vk_modos;
static VkResult VKAPI_CALL pb_modos(VkPhysicalDevice d, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) {
    if (opcion_android("vsync", "PB_VSYNC", 1) == 0) return g_vk_modos(d, s, n, m);
    if (!m) { *n = 1; return VK_SUCCESS; }
    if (*n < 1) return VK_INCOMPLETE;
    m[0] = VK_PRESENT_MODE_FIFO_KHR; *n = 1;
    return VK_SUCCESS;
}
/* --- Swappy (Android Frame Pacing, AGDK): cada fotograma se presenta a tiempo para su refresco de
 *     30 Hz usando los tiempos de la pantalla (VK_GOOGLE_display_timing), sin llenar la cola de la
 *     swapchain. Con FIFO a secas la cola se llenaba (el limitador va un poco más rápido que la pantalla)
 *     y cada fotograma esperaba 2-3 refrescos: 66-100 ms de retraso en los botones. [android] swappy=0 o
 *     PB_SWAPPY=0 lo quita. --- */
static int swappy_on(void) {
    static int on = -1;
    if (on < 0) on = opcion_android("swappy", "PB_SWAPPY", 1) != 0;
    return on;
}
static PFN_vkCreateDevice g_vk_cdev;
static PFN_vkGetDeviceQueue g_vk_gdq;
static PFN_vkCreateSwapchainKHR g_vk_csc;
static PFN_vkDestroySwapchainKHR g_vk_dsc;
static VkPhysicalDevice g_sw_fis;
static VkDevice g_sw_dev;
static VkResult VKAPI_CALL pb_cdev(VkPhysicalDevice f, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    if (!swappy_on()) return g_vk_cdev(f, ci, a, d);
    typedef VkResult (VKAPI_PTR *FEnum)(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
    FEnum en = (FEnum)dlsym(g_vk_lib, "vkEnumerateDeviceExtensionProperties");
    uint32_t nd = 0; VkExtensionProperties *disp = NULL;
    if (en && en(f, NULL, &nd, NULL) == VK_SUCCESS && nd) { disp = calloc(nd, sizeof(*disp)); if (disp) en(f, NULL, &nd, disp); }
    static char req_buf[8][VK_MAX_EXTENSION_NAME_SIZE]; char *req[8]; uint32_t nreq = 8;
    for (int i = 0; i < 8; i++) req[i] = req_buf[i];
    if (disp) {   /* en dos pasos: primero cuántas, luego los nombres */
        uint32_t cuantas = 0;
        SwappyVk_determineDeviceExtensions(f, nd, disp, &cuantas, NULL);
        if (cuantas > 8) cuantas = 8;
        for (int i = 0; i < 8; i++) req_buf[i][0] = 0;
        nreq = cuantas;
        if (cuantas) SwappyVk_determineDeviceExtensions(f, nd, disp, &nreq, req);
    } else nreq = 0;
    free(disp);
    const char *exts[64]; uint32_t n = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && n < 56; i++) exts[n++] = ci->ppEnabledExtensionNames[i];
    for (uint32_t i = 0; i < nreq && n < 64; i++) {
        if (!req[i][0]) continue;   /* nombre vacío: Vulkan lo rechaza (VK_ERROR_EXTENSION_NOT_PRESENT) */
        int ya = 0; for (uint32_t k = 0; k < n; k++) if (!strcmp(exts[k], req[i])) ya = 1;
        if (!ya) { exts[n++] = req[i]; LOGI("swappy: extensión %s", req[i]); }
    }
    VkDeviceCreateInfo c2 = *ci; c2.enabledExtensionCount = n; c2.ppEnabledExtensionNames = exts;
    VkResult r = g_vk_cdev(f, &c2, a, d);
    if (r == VK_SUCCESS) { g_sw_fis = f; g_sw_dev = *d; }
    return r;
}
static void VKAPI_CALL pb_gdq(VkDevice d, uint32_t fam, uint32_t idx, VkQueue *q) {
    g_vk_gdq(d, fam, idx, q);
    if (swappy_on() && q && *q) SwappyVk_setQueueFamilyIndex(d, *q, fam);
}
static VkResult VKAPI_CALL pb_csc(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *sc) {
    VkResult r = g_vk_csc(d, ci, a, sc);
    g_swappy_ok = 0;
    if (r == VK_SUCCESS && swappy_on() && g_sw_fis) {
        JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
        jobject act = (jobject)SDL_AndroidGetActivity();
        uint64_t refresco = 0;
        if (env && act && SwappyVk_initAndGetRefreshCycleDuration(env, act, g_sw_fis, d, *sc, &refresco)) {
            SwappyVk_setSwapIntervalNS(d, *sc, 33333333ull);
            SwappyVk_setAutoSwapInterval(false);   /* fijo a 30: sin bajar solo a 24/20 FPS */
            SwappyVk_setAutoPipelineMode(false);
            g_swappy_ok = 1;
        }
        if (act && env) (*env)->DeleteLocalRef(env, act);
        LOGI("swappy: %s (refresco %llu ns)", g_swappy_ok ? "activado a 30 FPS" : "no disponible", (unsigned long long)refresco);
        extern void arranque(const char *fmt, ...);
        arranque("swappy: %s (refresco de la pantalla %.1f ms)", g_swappy_ok ? "activado a 30 FPS" : "no disponible", refresco / 1e6);
    }
    return r;
}
static void VKAPI_CALL pb_dsc(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    if (g_swappy_ok && sc) SwappyVk_destroySwapchain(d, sc);
    g_swappy_ok = 0;
    g_vk_dsc(d, sc, a);
}
static PFN_vkVoidFunction envolver(const char *n, PFN_vkVoidFunction f) {
    if (!f || !n) return f;
    if (!strcmp(n, "vkCreateDevice")) { g_vk_cdev = (PFN_vkCreateDevice)f; return (PFN_vkVoidFunction)pb_cdev; }
    if (!strcmp(n, "vkGetDeviceQueue")) { g_vk_gdq = (PFN_vkGetDeviceQueue)f; return (PFN_vkVoidFunction)pb_gdq; }
    if (!strcmp(n, "vkCreateSwapchainKHR")) { g_vk_csc = (PFN_vkCreateSwapchainKHR)f; return (PFN_vkVoidFunction)pb_csc; }
    if (!strcmp(n, "vkDestroySwapchainKHR")) { g_vk_dsc = (PFN_vkDestroySwapchainKHR)f; return (PFN_vkVoidFunction)pb_dsc; }
    if (!strcmp(n, "vkGetPhysicalDeviceSurfacePresentModesKHR")) { g_vk_modos = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)f; return (PFN_vkVoidFunction)pb_modos; }
    if (!strcmp(n, "vkGetPhysicalDeviceMemoryProperties")) { g_vk_gpmp = (PFN_vkGetPhysicalDeviceMemoryProperties)f; return (PFN_vkVoidFunction)pb_gpmp; }
    if (!strcmp(n, "vkCreateGraphicsPipelines")) { g_vk_cgp = (PFN_vkCreateGraphicsPipelines)f; return (PFN_vkVoidFunction)pb_cgp; }
    if (!strcmp(n, "vkAllocateMemory")) { g_vk_alloc = (PFN_vkAllocateMemory)f; return (PFN_vkVoidFunction)pb_alloc; }
    if (!strcmp(n, "vkFreeMemory")) { g_vk_free = (PFN_vkFreeMemory)f; return (PFN_vkVoidFunction)pb_free; }
    if (!strcmp(n, "vkWaitForFences")) { g_vk_wff = (PFN_vkWaitForFences)f; return (PFN_vkVoidFunction)pb_wff; }
    if (!strcmp(n, "vkQueueSubmit")) { g_vk_submit = (PFN_vkQueueSubmit)f; return (PFN_vkVoidFunction)pb_submit; }
    if (!strcmp(n, "vkCreateShaderModule")) { g_vk_csm = (PFN_vkCreateShaderModule)f; return (PFN_vkVoidFunction)pb_csm; }
    if (!strcmp(n, "vkAcquireNextImageKHR")) { g_vk_acq = (PFN_vkAcquireNextImageKHR)f; return (PFN_vkVoidFunction)pb_acq; }
    if (!strcmp(n, "vkQueuePresentKHR")) { g_vk_pres = (PFN_vkQueuePresentKHR)f; return (PFN_vkVoidFunction)pb_pres; }
    return f;
}
static PFN_vkVoidFunction VKAPI_CALL pb_gdpa(VkDevice d, const char *n) { return envolver(n, g_vk_gdpa(d, n)); }
static PFN_vkVoidFunction VKAPI_CALL pb_gipa(VkInstance i, const char *n) {
    PFN_vkVoidFunction f = g_vk_gipa(i, n);
    if (f && n && !strcmp(n, "vkGetDeviceProcAddr")) { g_vk_gdpa = (PFN_vkGetDeviceProcAddr)f; return (PFN_vkVoidFunction)pb_gdpa; }
    if (f && n && !strcmp(n, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) { g_vk_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)f; return (PFN_vkVoidFunction)pb_caps; }
    return envolver(n, f);
}

HMODULE LoadLibraryA(LPCSTR ruta) {
    char b[1024]; pb_ruta(ruta, b, sizeof(b));
    const char *base = strrchr(b, '/'); base = base ? base + 1 : b;
    if (!strcasecmp(base, "vulkan-1.dll")) { g_vk_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL); return g_vk_lib; }
    if (!strncasecmp(base, "xinput", 6)) return NULL;   /* sin XInput: el port usa los mandos de SDL */
    void *h = dlopen(b, RTLD_NOW | RTLD_LOCAL);
    if (!h) LOGI("dlopen(%s): %s", b, dlerror());
    return h;
}
FARPROC GetProcAddress(HMODULE m, LPCSTR nombre) {
    void *f = dlsym(m, nombre);
    if (f && m == g_vk_lib && !strcmp(nombre, "vkGetInstanceProcAddr")) { g_vk_gipa = (PFN_vkGetInstanceProcAddr)f; return (FARPROC)pb_gipa; }
    return (FARPROC)f;
}
BOOL FreeLibrary(HMODULE m) { return dlclose(m) == 0; }
int MessageBoxA(HWND w, LPCSTR texto, LPCSTR titulo, UINT tipo) {
    (void)w;
    LOGI("[aviso] %s: %s", titulo ? titulo : "", texto ? texto : "");
    return (tipo & MB_YESNO) ? IDYES : IDOK;
}
void OutputDebugStringA(LPCSTR s) { if (s) LOGI("%s", s); }

/* --- archivos --- */
char *pb_ruta(const char *ruta, char *buf, size_t tam) {
    size_t i = 0;
    for (; ruta && ruta[i] && i + 1 < tam; i++) buf[i] = ruta[i] == '\\' ? '/' : ruta[i];
    buf[i] = 0;
    return buf;
}
FILE *pb_fopen(const char *ruta, const char *modo) {
    char b[1024]; char m[16]; size_t k = 0;
    for (const char *p = modo; *p && k + 1 < sizeof(m); p++) if (*p != 't') m[k++] = *p;   /* 't' de MSVC */
    m[k] = 0;
    return fopen(pb_ruta(ruta, b, sizeof(b)), m);
}
BOOL CreateDirectoryA(LPCSTR ruta, LPSECURITY_ATTRIBUTES sa) {
    (void)sa; char b[1024];
    if (mkdir(pb_ruta(ruta, b, sizeof(b)), 0777) == 0) return TRUE;
    t_err = errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_FILE_NOT_FOUND;
    return FALSE;
}
BOOL RemoveDirectoryA(LPCSTR ruta) { char b[1024]; return rmdir(pb_ruta(ruta, b, sizeof(b))) == 0; }
BOOL DeleteFileA(LPCSTR ruta) { char b[1024]; return unlink(pb_ruta(ruta, b, sizeof(b))) == 0; }
BOOL MoveFileExA(LPCSTR de, LPCSTR a, DWORD flags) {
    char b1[1024], b2[1024]; pb_ruta(de, b1, sizeof(b1)); pb_ruta(a, b2, sizeof(b2));
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && access(b2, F_OK) == 0) return FALSE;
    return rename(b1, b2) == 0;
}
BOOL CopyFileA(LPCSTR de, LPCSTR a, BOOL fallar) {
    char b1[1024], b2[1024]; pb_ruta(de, b1, sizeof(b1)); pb_ruta(a, b2, sizeof(b2));
    if (fallar && access(b2, F_OK) == 0) return FALSE;
    FILE *fi = fopen(b1, "rb"); if (!fi) return FALSE;
    FILE *fo = fopen(b2, "wb"); if (!fo) { fclose(fi); return FALSE; }
    char buf[65536]; size_t n; int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) if (fwrite(buf, 1, n, fo) != n) { ok = 0; break; }
    fclose(fi); fclose(fo);
    return ok;
}
DWORD GetFileAttributesA(LPCSTR ruta) {
    char b[1024]; struct stat s;
    if (stat(pb_ruta(ruta, b, sizeof(b)), &s)) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(s.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
static void rellena(DWORD *attr, FILETIME *c, FILETIME *a, FILETIME *w, DWORD *hi, DWORD *lo, const struct stat *s) {
    *attr = S_ISDIR(s->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    a_filetime(c, s->st_ctime); a_filetime(a, s->st_atime); a_filetime(w, s->st_mtime);
    *hi = (DWORD)((uint64_t)s->st_size >> 32); *lo = (DWORD)s->st_size;
}
BOOL GetFileAttributesExA(LPCSTR ruta, GET_FILEEX_INFO_LEVELS nivel, LPVOID info) {
    (void)nivel; char b[1024]; struct stat s;
    if (stat(pb_ruta(ruta, b, sizeof(b)), &s)) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)info;
    rellena(&d->dwFileAttributes, &d->ftCreationTime, &d->ftLastAccessTime, &d->ftLastWriteTime, &d->nFileSizeHigh, &d->nFileSizeLow, &s);
    return TRUE;
}
static BOOL busca_sig(Obj *o, WIN32_FIND_DATAA *d) {
    struct dirent *e;
    while ((e = readdir(o->dir))) {
        if (fnmatch(o->patron, e->d_name, FNM_CASEFOLD)) continue;
        memset(d, 0, sizeof(*d));
        snprintf(d->cFileName, sizeof(d->cFileName), "%s", e->d_name);
        char full[MAX_PATH * 2]; struct stat s;
        snprintf(full, sizeof(full), "%s/%s", o->base, e->d_name);
        if (stat(full, &s) == 0) rellena(&d->dwFileAttributes, &d->ftCreationTime, &d->ftLastAccessTime, &d->ftLastWriteTime, &d->nFileSizeHigh, &d->nFileSizeLow, &s);
        return TRUE;
    }
    return FALSE;
}
HANDLE FindFirstFileA(LPCSTR patron, WIN32_FIND_DATAA *d) {
    char b[1024]; pb_ruta(patron, b, sizeof(b));
    Obj *o = obj_nuevo(OB_BUSQUEDA);
    if (!o) return INVALID_HANDLE_VALUE;
    char *sl = strrchr(b, '/');
    if (sl) { *sl = 0; snprintf(o->base, sizeof(o->base), "%s", b[0] ? b : "/"); snprintf(o->patron, sizeof(o->patron), "%s", sl + 1); }
    else { snprintf(o->base, sizeof(o->base), "."); snprintf(o->patron, sizeof(o->patron), "%s", b); }
    if (!strcmp(o->patron, "*.*")) snprintf(o->patron, sizeof(o->patron), "*");
    o->dir = opendir(o->base);
    if (!o->dir || !busca_sig(o, d)) { if (o->dir) closedir(o->dir); free(o); t_err = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    return o;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *d) { Obj *o = (Obj *)h; return o && o->dir && busca_sig(o, d); }
BOOL FindClose(HANDLE h) { Obj *o = (Obj *)h; if (!o || o == INVALID_HANDLE_VALUE) return FALSE; if (o->dir) closedir(o->dir); free(o); return TRUE; }
HANDLE CreateFileA(LPCSTR ruta, DWORD acceso, DWORD compartir, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE plantilla) {
    (void)compartir; (void)sa; (void)flags; (void)plantilla;
    char b[1024]; pb_ruta(ruta, b, sizeof(b));
    int f = (acceso & GENERIC_WRITE) ? ((acceso & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
        case CREATE_NEW: f |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: f |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: f |= O_CREAT; break;
        case TRUNCATE_EXISTING: f |= O_TRUNC; break;
        default: break;
    }
    int fd = open(b, f | O_CLOEXEC, 0666);
    if (fd < 0) { t_err = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    Obj *o = obj_nuevo(OB_ARCHIVO);
    if (!o) { close(fd); return INVALID_HANDLE_VALUE; }
    o->fd = fd;
    return o;
}
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD leidos, void *ovp) {
    Obj *o = (Obj *)h; if (!o || o->fd < 0) return FALSE;
    OVERLAPPED *ov = (OVERLAPPED *)ovp;   /* con OVERLAPPED: lectura en esa posición (pread, sin mover el puntero) */
    ssize_t r = ov ? pread64(o->fd, buf, n, (off64_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset)) : read(o->fd, buf, n);
    if (r < 0) return FALSE;
    if (leidos) *leidos = (DWORD)r;
    return TRUE;
}
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD escritos, void *ov) {
    (void)ov; Obj *o = (Obj *)h; if (!o || o->fd < 0) return FALSE;
    ssize_t r = write(o->fd, buf, n); if (r < 0) return FALSE;
    if (escritos) *escritos = (DWORD)r;
    return TRUE;
}
static int whence(DWORD d) { return d == FILE_CURRENT ? SEEK_CUR : d == FILE_END ? SEEK_END : SEEK_SET; }
DWORD SetFilePointer(HANDLE h, LONG dist, LONG *alto, DWORD desde) {
    Obj *o = (Obj *)h; if (!o || o->fd < 0) return INVALID_SET_FILE_POINTER;
    off64_t d = alto ? (off64_t)(((uint64_t)(uint32_t)*alto << 32) | (uint32_t)dist) : dist;
    off64_t r = lseek64(o->fd, d, whence(desde));
    if (r < 0) return INVALID_SET_FILE_POINTER;
    if (alto) *alto = (LONG)(r >> 32);
    return (DWORD)r;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *nueva, DWORD desde) {
    Obj *o = (Obj *)h; if (!o || o->fd < 0) return FALSE;
    off64_t r = lseek64(o->fd, dist.QuadPart, whence(desde));
    if (r < 0) return FALSE;
    if (nueva) nueva->QuadPart = r;
    return TRUE;
}
DWORD GetFileSize(HANDLE h, LPDWORD alto) {
    Obj *o = (Obj *)h; struct stat s; if (!o || fstat(o->fd, &s)) return 0xFFFFFFFFu;
    if (alto) *alto = (DWORD)((uint64_t)s.st_size >> 32);
    return (DWORD)s.st_size;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *t) { Obj *o = (Obj *)h; struct stat s; if (!o || fstat(o->fd, &s)) return FALSE; t->QuadPart = s.st_size; return TRUE; }
BOOL FlushFileBuffers(HANDLE h) { Obj *o = (Obj *)h; return o && fsync(o->fd) == 0; }
BOOL SetEndOfFile(HANDLE h) { Obj *o = (Obj *)h; if (!o) return FALSE; off64_t p = lseek64(o->fd, 0, SEEK_CUR); return ftruncate64(o->fd, p) == 0; }

/* --- texto: UTF-8 <-> UTF-16 (WCHAR = 16 bits, como en Windows) --- */
int MultiByteToWideChar(UINT cp, DWORD f, LPCSTR s, int n, LPWSTR w, int nw) {
    (void)cp; (void)f;
    if (!s) return 0;
    if (n < 0) n = (int)strlen(s) + 1;
    int k = 0;
    for (int i = 0; i < n;) {
        uint32_t c = (uint8_t)s[i], extra = 0;
        if (c >= 0xF0) { c &= 7; extra = 3; } else if (c >= 0xE0) { c &= 15; extra = 2; } else if (c >= 0xC0) { c &= 31; extra = 1; }
        i++;
        for (uint32_t e = 0; e < extra && i < n; e++, i++) c = (c << 6) | ((uint8_t)s[i] & 63);
        if (c >= 0x10000) {
            if (w && nw) { if (k + 2 > nw) return 0; w[k] = (WCHAR)(0xD800 + ((c - 0x10000) >> 10)); w[k + 1] = (WCHAR)(0xDC00 + ((c - 0x10000) & 0x3FF)); }
            k += 2;
        } else {
            if (w && nw) { if (k + 1 > nw) return 0; w[k] = (WCHAR)c; }
            k++;
        }
    }
    return k;
}
int WideCharToMultiByte(UINT cp, DWORD f, LPCWSTR w, int nw, LPSTR s, int n, LPCSTR def, LPBOOL usado) {
    (void)cp; (void)f; (void)def; if (usado) *usado = FALSE;
    if (!w) return 0;
    if (nw < 0) { nw = 0; while (w[nw]) nw++; nw++; }
    int k = 0;
    for (int i = 0; i < nw; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < nw) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        char b[4]; int m;
        if (c < 0x80) { b[0] = (char)c; m = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 63)); m = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); m = 3; }
        else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); m = 4; }
        if (s && n) { if (k + m > n) return 0; memcpy(s + k, b, (size_t)m); }
        k += m;
    }
    return k;
}
int MessageBoxW(HWND hw, LPCWSTR texto, LPCWSTR titulo, UINT tipo) {
    char t[1024] = "", ti[256] = "";
    if (texto) WideCharToMultiByte(CP_UTF8, 0, texto, -1, t, sizeof(t) - 1, NULL, NULL);
    if (titulo) WideCharToMultiByte(CP_UTF8, 0, titulo, -1, ti, sizeof(ti) - 1, NULL, NULL);
    return MessageBoxA(hw, t, ti, tipo);
}

/* --- .ini: mismo comportamiento que GetPrivateProfileString/WritePrivateProfileString --- */
static pthread_mutex_t g_ini = PTHREAD_MUTEX_INITIALIZER;
static char *lee_todo(const char *ruta, long *n) {
    FILE *f = fopen(ruta, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long t = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)t + 2); if (!b) { fclose(f); return NULL; }
    long r = (long)fread(b, 1, (size_t)t, f); fclose(f); b[r] = 0; if (n) *n = r;
    return b;
}
static void recorta(char *s) {
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s); while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0;
}
DWORD GetPrivateProfileStringA(LPCSTR sec, LPCSTR clave, LPCSTR def, LPSTR out, DWORD tam, LPCSTR archivo) {
    char ruta[1024]; pb_ruta(archivo, ruta, sizeof(ruta));
    if (!tam) return 0;
    snprintf(out, tam, "%s", def ? def : "");
    pthread_mutex_lock(&g_ini);
    char *b = lee_todo(ruta, NULL);
    pthread_mutex_unlock(&g_ini);
    if (!b) return (DWORD)strlen(out);
    int en = 0;
    for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n")) {
        char lin[1024]; snprintf(lin, sizeof(lin), "%s", l); recorta(lin);
        if (lin[0] == '[') { char *e = strchr(lin, ']'); if (e) *e = 0; en = !strcasecmp(lin + 1, sec); continue; }
        if (!en || lin[0] == ';' || lin[0] == '#') continue;
        char *eq = strchr(lin, '='); if (!eq) continue;
        *eq = 0; char k[256]; snprintf(k, sizeof(k), "%s", lin); recorta(k);
        if (!strcasecmp(k, clave)) { char v[1024]; snprintf(v, sizeof(v), "%s", eq + 1); recorta(v); snprintf(out, tam, "%s", v); break; }
    }
    free(b);
    return (DWORD)strlen(out);
}
UINT GetPrivateProfileIntA(LPCSTR sec, LPCSTR clave, INT def, LPCSTR archivo) {
    char v[64]; char d[32]; snprintf(d, sizeof(d), "%d", def);
    GetPrivateProfileStringA(sec, clave, d, v, sizeof(v), archivo);
    return (UINT)atoi(v);
}
BOOL WritePrivateProfileStringA(LPCSTR sec, LPCSTR clave, LPCSTR valor, LPCSTR archivo) {
    char ruta[1024]; pb_ruta(archivo, ruta, sizeof(ruta));
    pthread_mutex_lock(&g_ini);
    long n = 0; char *b = lee_todo(ruta, &n);
    size_t cap = (size_t)n + strlen(sec) + strlen(clave) + (valor ? strlen(valor) : 0) + 64;
    char *o = (char *)malloc(cap); size_t k = 0; o[0] = 0;
    int en = 0, hecho = 0, vista = 0;
    if (b) {
        char *p = b;
        while (*p) {
            char *fin = strchr(p, '\n'); size_t len = fin ? (size_t)(fin - p) : strlen(p);
            char lin[1024]; size_t ll = len < sizeof(lin) - 1 ? len : sizeof(lin) - 1; memcpy(lin, p, ll); lin[ll] = 0;
            char rec[1024]; snprintf(rec, sizeof(rec), "%s", lin); recorta(rec);
            int copiar = 1;
            if (rec[0] == '[') {
                if (en && !hecho && valor) { k += (size_t)snprintf(o + k, cap - k, "%s=%s\n", clave, valor); hecho = 1; }
                char s2[256]; snprintf(s2, sizeof(s2), "%s", rec + 1); char *e = strchr(s2, ']'); if (e) *e = 0;
                en = !strcasecmp(s2, sec); if (en) vista = 1;
            } else if (en && !hecho) {
                char *eq = strchr(rec, '=');
                if (eq) { *eq = 0; char kk[256]; snprintf(kk, sizeof(kk), "%s", rec); recorta(kk);
                    if (!strcasecmp(kk, clave)) { if (valor) k += (size_t)snprintf(o + k, cap - k, "%s=%s\n", clave, valor); hecho = 1; copiar = 0; } }
            }
            if (copiar) { if (k + len + 2 > cap) { cap = (k + len + 2) * 2; o = (char *)realloc(o, cap); } memcpy(o + k, p, len); k += len; o[k++] = '\n'; o[k] = 0; }
            p = fin ? fin + 1 : p + len;
        }
    }
    if (!hecho && valor) {
        if (k + strlen(sec) + strlen(clave) + strlen(valor) + 8 > cap) { cap = k + strlen(sec) + strlen(clave) + strlen(valor) + 64; o = (char *)realloc(o, cap); }
        if (!vista) k += (size_t)snprintf(o + k, cap - k, "[%s]\n", sec);
        else if (en) {}   /* la sección era la última: se añade al final */
        k += (size_t)snprintf(o + k, cap - k, "%s=%s\n", clave, valor);
    }
    FILE *f = fopen(ruta, "wb"); BOOL ok = FALSE;
    if (f) { ok = fwrite(o, 1, k, f) == k; fclose(f); }
    free(o); free(b);
    pthread_mutex_unlock(&g_ini);
    return ok;
}
