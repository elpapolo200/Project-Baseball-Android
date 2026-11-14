/* dialogos_android.c — los diálogos del sistema de la Vita en Android (sustituye a hle_dialog.c y
 * hle_savedialog.c del PC, que abren ventanas Win32).
 *
 * Misma lógica que en PC con su modo «sin ventana»:
 *   - Mensajes y guardado contestan «Sí/OK»; red/PSN «cancelado» (sin conexión).
 *   - Teclado (ImeDialog): devuelve el texto inicial (más adelante: teclado de Android).
 *   - Lista de partidas (SaveDataDialog modo 2): cargar = la partida más reciente; guardar = la ranura
 *     que el juego tiene enfocada (más adelante: lista táctil propia).
 * Estructuras: ver los comentarios de hle_savedialog.c / hle_dialog.c (fuente\port\host). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"
#include "port.h"

#define ST_NONE     0
#define ST_RUNNING  1
#define ST_FINISHED 2
#define RES_OK            0
#define RES_USER_CANCELED 1
#define BUTTON_YES   1
#define IME_ENTER    2
#define ERR_NOT_RUNNING 0x80020401u

enum { D_MSG, D_SAVE, D_IME, D_NETCHECK, D_FRIENDS, D_NPMSG, D_PROFILE, D_TROPHY, D_COUNT };
static volatile int g_status[D_COUNT];

/* --- teclado: se queda el texto inicial --- */
static uint16_t g_ime_text[512];
static uint32_t g_ime_buf, g_ime_max;
static void ime_init(uint32_t param) {
    g_ime_buf = vita_read32(param + 0x34);
    g_ime_max = vita_read32(param + 0x2C);
    if (g_ime_max > 500) g_ime_max = 500;
    uint32_t a = vita_read32(param + 0x30);
    size_t i = 0;
    for (; a && i + 1 < 512; i++) { uint16_t c = vita_read16(a + 2 * (uint32_t)i); if (!c) break; g_ime_text[i] = c; }
    g_ime_text[i] = 0;
    printf("[dialog] teclado: búfer 0x%08X máx %u (Android: texto inicial)\n", g_ime_buf, g_ime_max);
    g_status[D_IME] = ST_FINISHED;
}
static void ime_store(void) {
    if (!g_ime_buf) return;
    uint32_t n = 0;
    while (n < 511 && g_ime_text[n]) n++;
    if (g_ime_max && n > g_ime_max) n = g_ime_max;
    for (uint32_t i = 0; i < n; i++) vita_write16(g_ime_buf + 2 * i, g_ime_text[i]);
    vita_write16(g_ime_buf + 2 * n, 0);
}

static void dlg_init(int d) { g_status[d] = ST_FINISHED; r0 = 0; }
static void dlg_result(int d, uint32_t res) {
    if (g_status[d] == ST_NONE) { r0 = ERR_NOT_RUNNING; return; }
    if (res) {
        switch (d) {
            case D_MSG: case D_SAVE: vita_write32(res + 4, RES_OK); vita_write32(res + 8, BUTTON_YES); break;
            case D_IME: ime_store(); vita_write32(res + 0, RES_OK); vita_write32(res + 4, IME_ENTER); break;
            case D_TROPHY: vita_write32(res + 0, RES_OK); break;
            default: vita_write32(res + 0, RES_USER_CANCELED); break;
        }
    }
    r0 = 0;
}

/* --- lista de partidas --- */
#define SLOT_PARAM_SIZE 0x348u
#define MAXS 64
static volatile int g_estado;
static uint32_t g_modo, g_tipo, g_slot, g_resultado, g_boton;
static int g_n, g_sel;
static struct { uint32_t id; int hay; int64_t t; } g_e[MAXS];

static int slot_lee(uint32_t id, uint8_t *p, int64_t *t) {
    char g[64], r[MAX_PATH];
    snprintf(g, sizeof(g), "savedata0:sce_slots/slot_%u.bin", id);
    port_vfs_path(g, r, sizeof(r));
    FILE *f = fopen(r, "rb");
    if (!f) return 0;
    memset(p, 0, SLOT_PARAM_SIZE);
    fread(p, 1, SLOT_PARAM_SIZE, f);
    fclose(f);
    if (t) { char b[MAX_PATH]; struct stat s; *t = stat(pb_ruta(r, b, sizeof(b)), &s) == 0 ? (int64_t)s.st_mtime : 0; }
    return 1;
}
static void lista(uint32_t lp) {
    uint32_t sl = vita_read32(lp), n = vita_read32(lp + 4), foco = vita_read32(lp + 0xC);
    uint8_t p[SLOT_PARAM_SIZE];
    g_n = 0; g_sel = 0;
    for (uint32_t i = 0; i < n && g_n < MAXS; i++) {
        uint32_t id = vita_read32(sl + 16 * i);
        int64_t t = 0;
        int hay = slot_lee(id, p, &t);
        if (!hay && g_tipo != 1) continue;   /* cargar/borrar: solo las que existen */
        g_e[g_n].id = id; g_e[g_n].hay = hay; g_e[g_n].t = t;
        if (id == foco) g_sel = g_n;
        g_n++;
    }
    if (g_tipo == 2) {   /* cargar: la más reciente */
        int best = -1;
        for (int i = 0; i < g_n; i++) if (g_e[i].hay && (best < 0 || g_e[i].t > g_e[best].t)) best = i;
        if (best >= 0) g_sel = best;
    }
    printf("[partidas] lista (%s): %d de %u ranuras\n", g_tipo == 1 ? "guardar" : g_tipo == 2 ? "cargar" : "borrar", g_n, n);
}
/* Lista táctil de partidas: ProjectBaseballActivity.elegirPartida (diálogo de Android) por JNI.
 * Devuelve el índice elegido o -1 si se cancela. */
#include <jni.h>
#include <SDL_system.h>
static void ascii(char *d, size_t n, const char *s) {   /* NewStringUTF quiere UTF-8 válido: solo ASCII */
    size_t k = 0;
    for (; s && *s && k + 1 < n; s++) d[k++] = ((unsigned char)*s >= 0x20 && (unsigned char)*s < 0x7F) ? *s : ' ';
    d[k] = 0;
}
static int elegir_android(const char *titulo, char (*items)[200], int n, int sel) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act) return sel;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, cls, "elegirPartida", "(Ljava/lang/String;[Ljava/lang/String;I)I");
    if (!m) { (*env)->ExceptionClear(env); (*env)->DeleteLocalRef(env, act); return sel; }
    jclass scls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, n, scls, NULL);
    for (int i = 0; i < n; i++) { jstring s = (*env)->NewStringUTF(env, items[i]); (*env)->SetObjectArrayElement(env, arr, i, s); (*env)->DeleteLocalRef(env, s); }
    jstring t = (*env)->NewStringUTF(env, titulo);
    jint r = (*env)->CallStaticIntMethod(env, cls, m, t, arr, (jint)sel);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = sel; }
    (*env)->DeleteLocalRef(env, t); (*env)->DeleteLocalRef(env, arr); (*env)->DeleteLocalRef(env, scls);
    (*env)->DeleteLocalRef(env, cls); (*env)->DeleteLocalRef(env, act);
    return (int)r;
}
static int lista_tactil(void) {
    static char items[MAXS][200];
    uint8_t p[SLOT_PARAM_SIZE];
    for (int i = 0; i < g_n; i++) {
        if (!g_e[i].hay || !slot_lee(g_e[i].id, p, NULL)) { snprintf(items[i], sizeof(items[i]), "%d. Ranura vacia", i + 1); continue; }
        char tit[80], sub[100], fecha[32] = "";
        ascii(tit, sizeof(tit), (const char *)p + 4);
        ascii(sub, sizeof(sub), (const char *)p + 0x44);
        if (g_e[i].t) { time_t tt = (time_t)g_e[i].t; struct tm tmv; localtime_r(&tt, &tmv); strftime(fecha, sizeof(fecha), "%d/%m %H:%M", &tmv); }
        snprintf(items[i], sizeof(items[i]), "%d. %s - %s  (%s)", i + 1, tit, sub, fecha);
    }
    const char *titulo = g_tipo == 1 ? "Guardar partida" : g_tipo == 2 ? "Cargar partida" : "Borrar partida";
    return elegir_android(titulo, items, g_n, g_sel);
}

void savedialog_init(uint32_t param) {
    g_modo = vita_read32(param + 0x50);
    g_tipo = vita_read32(param + 0x54);
    g_resultado = 0; g_boton = 1;
    uint32_t fp = vita_read32(param + 0x58), lp = vita_read32(param + 0x5C);
    if (g_modo == 1 && fp) g_slot = vita_read32(fp);
    if (g_modo == 2 && lp) {
        lista(lp);
        if (g_n == 0) { g_resultado = 1; g_boton = 0; g_estado = 2; return; }
        if (!getenv("MLB15_PAD_GUION")) {   /* sin guion de pruebas: que elija el jugador */
            int k = lista_tactil();
            if (k < 0 || k >= g_n) { printf("[partidas] Android: cancelado\n"); g_resultado = 1; g_boton = 0; g_estado = 2; return; }
            g_sel = k;
        }
        g_slot = g_e[g_sel].id;
        printf("[partidas] Android: ranura %u\n", g_slot);
    }
    g_estado = 2;
}
uint32_t savedialog_status(void) { return (uint32_t)g_estado; }
void savedialog_term(void) { g_estado = 0; }
void savedialog_finish(void) { g_estado = 2; }
void savedialog_result(uint32_t res) {
    if (!res) return;
    vita_write32(res + 0, g_modo);
    vita_write32(res + 4, g_resultado);
    vita_write32(res + 8, g_boton);
    vita_write32(res + 0xC, g_slot);
    uint32_t info = vita_read32(res + 0x10);
    if (info && vita_mem_ptr(info, 8)) {
        uint8_t p[SLOT_PARAM_SIZE];
        int hay = slot_lee(g_slot, p, NULL);
        vita_write32(info, hay ? 1u : 0u);
        uint32_t sp_ = vita_read32(info + 4);
        if (hay && sp_ && vita_mem_ptr(sp_, SLOT_PARAM_SIZE)) { memset(p + 0x308, 0, 16); memcpy(vita_mem_ptr(sp_, SLOT_PARAM_SIZE), p, SLOT_PARAM_SIZE); }
    }
}

#define DIALOG(prefix, d)                                                          \
    void vita_hle_##prefix##Init(void)      { if (d == D_IME) { ime_init(r0); r0 = 0; } else dlg_init(d); } \
    void vita_hle_##prefix##GetStatus(void) { r0 = (uint32_t)g_status[d]; }        \
    void vita_hle_##prefix##GetResult(void) { dlg_result(d, r0); }                 \
    void vita_hle_##prefix##Term(void)      { g_status[d] = ST_NONE; r0 = 0; }

DIALOG(sceMsgDialog, D_MSG)
void vita_hle_sceSaveDataDialogInit(void)      { savedialog_init(r0); r0 = 0; }
void vita_hle_sceSaveDataDialogGetStatus(void) { r0 = savedialog_status(); }
void vita_hle_sceSaveDataDialogGetResult(void) { savedialog_result(r0); r0 = 0; }
void vita_hle_sceSaveDataDialogTerm(void)      { savedialog_term(); r0 = 0; }
DIALOG(sceImeDialog, D_IME)
DIALOG(sceNetCheckDialog, D_NETCHECK)
DIALOG(sceNpFriendListDialog, D_FRIENDS)
DIALOG(sceNpMessageDialog, D_NPMSG)
DIALOG(sceNpProfileDialog, D_PROFILE)
DIALOG(sceNpTrophySetupDialog, D_TROPHY)
void vita_hle_sceMsgDialogAbort(void)       { g_status[D_MSG] = ST_FINISHED; r0 = 0; }
void vita_hle_sceImeDialogAbort(void)       { g_status[D_IME] = ST_FINISHED; r0 = 0; }
void vita_hle_sceNpMessageDialogAbort(void) { g_status[D_NPMSG] = ST_FINISHED; r0 = 0; }
void vita_hle_sceSaveDataDialogAbort(void)  { savedialog_finish(); r0 = 0; }
void vita_hle_sceSaveDataDialogContinue(void)     { savedialog_init(r0); r0 = 0; }
void vita_hle_sceSaveDataDialogFinish(void)       { savedialog_finish(); r0 = 0; }
void vita_hle_sceSaveDataDialogSubClose(void)     { r0 = 0; }
void vita_hle_sceSaveDataDialogGetSubStatus(void) { r0 = ST_NONE; }
void vita_hle_sceCommonDialogUpdate(void) { r0 = 0; }
