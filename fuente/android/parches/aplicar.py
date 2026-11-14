# Parches de Android sobre la copia del port (fuente\port), que sale del PC con
# sincronizar_desde_pc.ps1. Se aplican después de cada sincronización; son idempotentes.
#   py -I aplicar.py <ruta de fuente\port>
#
# 1. v3k sin bucles con índice variable (2026-11-09). En Adreno, una tabla privada del shader a la
#    que se accede con un índice que solo se conoce al ejecutar no cabe en registros: el driver le da
#    memoria privada por cada hilo de la GPU y por cada pipeline. Con los bancos de registros de la
#    Vita (pa/sa/r/outs, ~1,7 KB por invocación) y ~200 pipelines del estadio, la memoria del driver
#    («GL mtrack») subía de 0,8 a 3,9 GB al cargar el partido y Android cerraba el juego. Los dos
#    bucles (copiar los uniformes al banco sa y poner a cero outs) se desenrollan con índices fijos.
import sys, os

raiz = sys.argv[1]
f = os.path.join(raiz, 'v3k', 'src', 'shader', 'spirv_recompiler.cpp')
s = open(f, encoding='utf-8').read()
cambios = 0

ini = s.index('static void copy_uniform_block_to_register(')
fin = s.index('\n}\n', ini) + 3
nuevo = r'''static void copy_uniform_block_to_register(spv::Builder &builder, spv::Id sa_bank, spv::Id block, spv::Id ite, const int start, const int vec4_count) {
    // ANDROID: desenrollado con índices fijos (ver fuente/android/parches/aplicar.py).
    (void)ite;
    int start_in_vec4_granularity = start / 4;

    for (int k = 0; k < vec4_count; k++) {
        spv::Id to_copy = utils::create_access_chain(builder, spv::StorageClassStorageBuffer, block, { builder.makeIntConstant(k) });
        to_copy = builder.createLoad(to_copy, spv::NoPrecision);
        spv::Id dest = utils::create_access_chain(builder, spv::StorageClassPrivate, sa_bank, { builder.makeIntConstant(k + start_in_vec4_granularity) });

        if (start % 4 == 0) {
            builder.createStore(to_copy, dest);
        } else {
            spv::Id dest_friend = utils::create_access_chain(builder, spv::StorageClassPrivate, sa_bank, { builder.makeIntConstant(k + start_in_vec4_granularity + 1) });

            std::vector<spv::Id> ops_copy_1 = { builder.createLoad(dest, spv::NoPrecision), to_copy };
            std::vector<spv::Id> ops_copy_2 = { builder.createLoad(dest_friend, spv::NoPrecision), to_copy };

            for (int i = 0; i < start % 4; i++) {
                ops_copy_1.push_back(i);
                ops_copy_2.push_back(4 + (4 - start % 4) + i);
            }

            for (int i = 0; i < (4 - start % 4); i++) {
                ops_copy_1.push_back(4 + i);
                ops_copy_2.push_back((start % 4) + i);
            }

            to_copy = builder.createOp(spv::OpVectorShuffle, builder.getTypeId(to_copy), ops_copy_1);
            spv::Id to_copy_2 = builder.createOp(spv::OpVectorShuffle, builder.getTypeId(to_copy), ops_copy_2);

            builder.createStore(to_copy, dest);
            builder.createStore(to_copy_2, dest_friend);
        }
    }
}
'''
if 'ANDROID: desenrollado' not in s[ini:fin]:
    s = s[:ini] + nuevo + s[fin:]
    cambios += 1

viejo = '''            utils::make_for_loop(b, ite, b.makeIntConstant(0), b.makeIntConstant(REG_O_COUNT / 4), [&]() {
                spv::Id dest = utils::create_access_chain(b, spv::StorageClassPrivate, parameters.outs, { b.createLoad(ite, spv::NoPrecision) });
                b.createStore(rezero_v, dest);
            });'''
nuevo2 = '''            (void)ite;   // ANDROID: desenrollado con índices fijos (ver fuente/android/parches/aplicar.py)
            for (int k = 0; k < REG_O_COUNT / 4; k++) {
                spv::Id dest = utils::create_access_chain(b, spv::StorageClassPrivate, parameters.outs, { b.makeIntConstant(k) });
                b.createStore(rezero_v, dest);
            }'''
if viejo in s:
    s = s.replace(viejo, nuevo2, 1)
    cambios += 1

open(f, 'w', encoding='utf-8').write(s)

# 2. Nombre de cada hilo de la Vita en el hilo de Linux (para medir la CPU por hilo en el teléfono).
f2 = os.path.join(raiz, 'host', 'hle_kernel.c')
s2 = open(f2, encoding='utf-8').read()
viejo2 = '    port_watch_thread_start();          /* MLB15_VIGILA (watchdog.c) */\n'
nuevo2b = viejo2 + '#ifdef __ANDROID__\n    { extern void pb_nombre_hilo(const char *); pb_nombre_hilo(t->name); }   /* ANDROID: parches/aplicar.py */\n#endif\n'
if viejo2 in s2 and 'pb_nombre_hilo' not in s2:
    s2 = s2.replace(viejo2, nuevo2b, 1); cambios += 1
    open(f2, 'w', encoding='utf-8').write(s2)

# 3. Limitador de FPS sin espera activa: en Windows Sleep es impreciso y el último 1,5 ms se espera
#    cediendo la CPU en bucle; en Android nanosleep es preciso y se duerme todo.
f3 = os.path.join(raiz, 'host', 'hle_display.c')
s3 = open(f3, encoding='utf-8').read()
viejo3 = """            if (falta > 0.002) Sleep((DWORD)((falta - 0.0015) * 1000.0));
            do { SwitchToThread(); QueryPerformanceCounter(&now); } while (now.QuadPart / f < plazo);"""
nuevo3 = """#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (sin espera activa) */
            { struct timespec ts = { (time_t)falta, (long)((falta - (double)(time_t)falta) * 1e9) }; nanosleep(&ts, NULL); }
#else
            if (falta > 0.002) Sleep((DWORD)((falta - 0.0015) * 1000.0));
            do { SwitchToThread(); QueryPerformanceCounter(&now); } while (now.QuadPart / f < plazo);
#endif"""
if viejo3 in s3 and 'sin espera activa' not in s3:
    s3 = s3.replace(viejo3, nuevo3, 1); cambios += 1
    open(f3, 'w', encoding='utf-8').write(s3)

# 4. Esperas con timeout 0 (sondeos) que duermen de verdad. El juego sondea semáforos y el LwMutex
#    «Render» con timeout 0 cientos de miles de veces por segundo. En PC se cede la CPU 4 veces y se
#    duerme 1 ms; en Android cada cesión/despertar es una llamada al núcleo y el perfil del partido
#    salía con el 60 % de la CPU en syscall/nanosleep/sched_yield (~1 núcleo entre 10 hilos parados).
#    Aquí el sondeo fallido espera 1-2 ms EN la variable de condición del objeto: si llega la señal se
#    despierta al momento (sin retraso) y toma el semáforo/mutex; si no, devuelve timeout como antes.
f4 = os.path.join(raiz, 'host', 'hle_kernel.c')
s4 = open(f4, encoding='utf-8').read()
viejo4 = """    while (o->count < need) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) {
            LeaveCriticalSection(&o->cs);
            if (ms == 0) poll_backoff();"""
nuevo4 = """    while (o->count < need) {
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (sondeo que duerme en la variable de condición) */
        if (ms == 0) {
            SleepConditionVariableCS(&o->cv, &o->cs, ++t_failed_polls < 3 ? 1 : 2);
            if (o->count >= need) break;
            LeaveCriticalSection(&o->cs);
            r0 = ERR_WAIT_TIMEOUT;
            return;
        }
#endif
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) {
            LeaveCriticalSection(&o->cs);
            if (ms == 0) poll_backoff();"""
if viejo4 in s4 and 'sondeo que duerme' not in s4:
    s4 = s4.replace(viejo4, nuevo4, 1); cambios += 1
viejo5 = """        vueltas++;
        if (try_only || !SleepConditionVariableCS(&o->cv, &o->cs, ms)) {"""
nuevo5 = """        vueltas++;
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (sondeo que duerme en la variable de condición) */
        if (!try_only && ms == 0) {
            SleepConditionVariableCS(&o->cv, &o->cs, 1);
            if (o->recursion <= 0) continue;
            LeaveCriticalSection(&o->cs);
            return ERR_WAIT_TIMEOUT;
        }
#endif
        if (try_only || !SleepConditionVariableCS(&o->cv, &o->cs, ms)) {"""
if viejo5 in s4 and s4.count('sondeo que duerme') < 2:
    s4 = s4.replace(viejo5, nuevo5, 1); cambios += 1
open(f4, 'w', encoding='utf-8').write(s4)

# 5. Audio sin sondeo: para no adelantarse más de 4 bloques, el hilo de audio preguntaba cada 1 ms
#    cuánto quedaba en la cola de SDL (1000 despertares por segundo). Se calcula lo que sobra y se
#    duerme eso de una vez.
f5 = os.path.join(raiz, 'host', 'hle_audio.c')
s5 = open(f5, encoding='utf-8').read()
viejo5 = "    while (SDL_GetQueuedAudioSize(g_dev) > block * 4) Sleep(1);\n"
nuevo5 = """#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (audio sin sondeo) */
    for (Uint32 q; (q = SDL_GetQueuedAudioSize(g_dev)) > block * 4;)
        Sleep((DWORD)((uint64_t)(q - block * 4) * 1000ull / ((uint64_t)freq * 4ull)) + 1);
#else
    while (SDL_GetQueuedAudioSize(g_dev) > block * 4) Sleep(1);
#endif
"""
if viejo5 in s5 and 'audio sin sondeo' not in s5:
    s5 = s5.replace(viejo5, nuevo5, 1); cambios += 1
    open(f5, 'w', encoding='utf-8').write(s5)

# 6. Sondeos de semáforo que siguen fallando mucho rato (hilos Kinematics parados): la espera en la
#    variable de condición sube a 4 ms (una señal sigue despertando al momento).
f6 = os.path.join(raiz, 'host', 'hle_kernel.c')
s6 = open(f6, encoding='utf-8').read()
viejo6 = "SleepConditionVariableCS(&o->cv, &o->cs, ++t_failed_polls < 3 ? 1 : 2);"
nuevo6 = "SleepConditionVariableCS(&o->cv, &o->cs, ++t_failed_polls < 3 ? 1 : t_failed_polls < 40 ? 2 : 4);"
if viejo6 in s6:
    s6 = s6.replace(viejo6, nuevo6, 1); cambios += 1
    open(f6, 'w', encoding='utf-8').write(s6)

# 7. Los búferes de pantalla (sceDisplaySetFrameBuf) no vuelven a la memoria del juego: la presentación
#    sale de la imagen de la GPU y la CPU no los lee. Eran 2 de las ~10 escenas por fotograma que se
#    copiaban GPU -> memoria intermedia -> memoria del juego (2,3 MB por fotograma). Si la CPU los
#    escribe, el hash de la memoria cambia y la escena siguiente los recarga como siempre.
#    PB_FB_MEMORIA=1 vuelve a copiarlos.
f7 = os.path.join(raiz, 'host', 'gpu_vk.c')
s7 = open(f7, encoding='utf-8').read()
if 'pb_fb_solo_gpu' not in s7:
    defs = """
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (búferes de pantalla solo en la GPU) */
int port_display_is_framebuf(uint32_t addr);
static int pb_fb_solo_gpu(uint32_t a) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("PB_FB_MEMORIA"); on = !(e && *e == '1'); }
    return on && port_display_is_framebuf(a);
}
#define PB_FB_SOLO_GPU(t) pb_fb_solo_gpu((t)->addr)
#else
#define PB_FB_SOLO_GPU(t) 0
#endif
"""
    ancla = 'static void scene_finish(Target *t) {'
    v1 = '    uint8_t *dst = t->resident ? NULL : (uint8_t *)vita_mem_ptr(t->addr, t->gstride * t->h * bpp);'
    n1 = '    uint8_t *dst = (t->resident || PB_FB_SOLO_GPU(t)) ? NULL : (uint8_t *)vita_mem_ptr(t->addr, t->gstride * t->h * bpp);'
    v2 = '    if (!t->resident || (volcar && t->addr == volcar)) {'
    n2 = '    if ((!t->resident && !PB_FB_SOLO_GPU(t)) || (volcar && t->addr == volcar)) {'
    i = s7.index(ancla)
    # la definición va antes de la primera función que la usa (el fin de escena con la copia a la GPU está antes de scene_finish o después: ponerla al principio del archivo tras los includes)
    j = s7.index('\n', s7.index('#include "gpu.h"')) + 1 if '#include "gpu.h"' in s7 else 0
    if v1 in s7 and v2 in s7:
        s7 = s7[:j] + defs + s7[j:]
        s7 = s7.replace(v1, n1, 1).replace(v2, n2, 1)
        cambios += 1
        open(f7, 'w', encoding='utf-8').write(s7)

# 8. Más escenas en vuelo con búferes más pequeños. El juego hace ~55 escenas por fotograma y con 4 en
#    vuelo el hilo de dibujo pasaba ~20 % del tiempo esperando a la GPU (21 -> 23 FPS con 8). Para no
#    gastar más memoria, los búferes por escena bajan a lo medido en el partido con margen
#    (vértices 8,3 MB -> 16; índices 1,4 -> 3; uniformes 1,6 -> 3; subidas -> 4).
#    2026-11-09: 16 en vuelo. Con 8 la GPU iba a ~220 MHz (la escena principal se entrega de golpe y
#    entre medias la GPU se queda sin trabajo, así que Android no le sube el reloj) y el hilo de
#    dibujo esperaba ~160 ms/s a que acabara: 101 de 129 fotogramas pasaban de 36 ms.
f8 = os.path.join(raiz, 'host', 'gpu_vk.c')
s8 = open(f8, encoding='utf-8').read()
reps = [
    ('#define VB_SIZE   (32u << 20)', '#ifdef __ANDROID__\n#define VB_SIZE   (16u << 20)   /* ANDROID: parches/aplicar.py */\n#else\n#define VB_SIZE   (32u << 20)\n#endif'),
    ('#define IB_SIZE   (8u << 20)', '#ifdef __ANDROID__\n#define IB_SIZE   (3u << 20)\n#else\n#define IB_SIZE   (8u << 20)\n#endif'),
    ('#define MB_SIZE   (8u << 20)', '#ifdef __ANDROID__\n#define MB_SIZE   (3u << 20)\n#else\n#define MB_SIZE   (8u << 20)\n#endif'),
    ('#define NCTX 8\n', '#ifdef __ANDROID__\n#define NCTX 16   /* ANDROID: parches/aplicar.py */\n#else\n#define NCTX 8\n#endif\n'),
    ('#define UP_SIZE (8u << 20)', '#ifdef __ANDROID__\n#define UP_SIZE (4u << 20)\n#else\n#define UP_SIZE (8u << 20)\n#endif\n/* (en PC 8 MB) */'),
    ('            g_nctx = e ? atoi(e) : 4;', '#ifdef __ANDROID__\n            g_nctx = e ? atoi(e) : 10;   /* ANDROID: parches/aplicar.py (10: 29,7 FPS y 3,6 GB; 16: igual y 4,9 GB; 8: 28,3) */\n#else\n            g_nctx = e ? atoi(e) : 4;\n#endif'),
]
if '#ifdef __ANDROID__\n#define VB_SIZE' not in s8:   # (antes miraba «16u»: el parche 42 lo sube a 32 y se volvía a aplicar)
    for v, n in reps:
        if v in s8:
            s8 = s8.replace(v, n, 1); cambios += 1
    open(f8, 'w', encoding='utf-8').write(s8)

# (9. La intro con logos se deja como en el original: lo pidió el usuario el 2026-11-09.)

# 10. do_draw: índices en rango compacto sin tabla de sellos. Para cada dibujo se renumeraban los
#     vértices usados con g_stamp/g_slot (tablas grandes, accesos al azar: ~8 % del hilo de dibujo en el
#     teléfono). Si los índices caen en un rango [mín, máx] poco mayor que su número (lo normal en
#     mallas), se usa el rango tal cual: g_vsrc = mín + i, índice = x - mín.
f10 = os.path.join(raiz, 'host', 'gxm.c')
s10 = open(f10, encoding='utf-8').read()
v10 = """    uint32_t nvb = 0;
    if (++g_stamp_id == 0) { memset(g_stamp, 0, sizeof(g_stamp)); g_stamp_id = 1; }
    for (uint32_t i = 0; i < icount; i++) {"""
n10 = """    uint32_t nvb = 0;
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (índices en rango compacto) */
    {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        if (u32) { for (uint32_t i = 0; i < icount; i++) { uint32_t x; memcpy(&x, ip + 4 * i, 4); if (x < lo) lo = x; if (x > hi) hi = x; } }
        else { const uint16_t *h16 = (const uint16_t *)ip; for (uint32_t i = 0; i < icount; i++) { uint32_t x = h16[i]; if (x < lo) lo = x; if (x > hi) hi = x; } }
        if (icount && hi < MAX_INDEX_VALUE && hi - lo < MAX_DRAW_VERTS && hi - lo + 1 <= icount + icount / 2 + 64) {
            nvb = hi - lo + 1;
            for (uint32_t i = 0; i < nvb; i++) g_vsrc[i] = lo + i;
            if (u32) { for (uint32_t i = 0; i < icount; i++) { uint32_t x; memcpy(&x, ip + 4 * i, 4); g_idx[i] = x - lo; } }
            else { const uint16_t *h16 = (const uint16_t *)ip; for (uint32_t i = 0; i < icount; i++) g_idx[i] = (uint32_t)h16[i] - lo; }
            goto indices_listos;
        }
    }
#endif
    if (++g_stamp_id == 0) { memset(g_stamp, 0, sizeof(g_stamp)); g_stamp_id = 1; }
    for (uint32_t i = 0; i < icount; i++) {"""
v10b = """    uint32_t maxsrc = 0;
    for (uint32_t i = 0; i < nvb; i++) if (g_vsrc[i] > maxsrc) maxsrc = g_vsrc[i];"""
n10b = """#ifdef __ANDROID__
indices_listos:;
#endif
    uint32_t maxsrc = 0;
    for (uint32_t i = 0; i < nvb; i++) if (g_vsrc[i] > maxsrc) maxsrc = g_vsrc[i];"""
if v10 in s10 and v10b in s10 and 'rango compacto' not in s10:
    s10 = s10.replace(v10, n10, 1).replace(v10b, n10b, 1); cambios += 1
    open(f10, 'w', encoding='utf-8').write(s10)

# 11. Superficies extra solo en la GPU (pruebas): PB_SOLO_GPU=dir,dir,... (hex). Amplía el parche 7.
f11 = os.path.join(raiz, 'host', 'gpu_vk.c')
s11 = open(f11, encoding='utf-8').read()
v11 = "    return on && port_display_is_framebuf(a);"
n11 = """    if (!on) return 0;
    if (port_display_is_framebuf(a)) return 1;
    static uint32_t extra[16]; static int nextra = -1;
    if (nextra < 0) {
        nextra = 0;
        const char *e = getenv("PB_SOLO_GPU");
        for (const char *q = e; q && *q && nextra < 16; ) { extra[nextra++] = (uint32_t)strtoul(q, NULL, 16); q = strchr(q, ','); if (q) q++; }
    }
    for (int i = 0; i < nextra; i++) if (extra[i] == a) return 1;
    return 0;"""
if v11 in s11 and 'PB_SOLO_GPU' not in s11:
    s11 = s11.replace(v11, n11, 1); cambios += 1
    open(f11, 'w', encoding='utf-8').write(s11)

# 12. Resumen de escenas (pruebas): PB_RESUMEN=N lista las escenas del fotograma N con su superficie,
#     dibujos e índices, agrupados por par de programas (fragmento + vértice) y los nombres de las
#     texturas del fragmento (para saber qué sistema del juego dibuja cada cosa).
f12 = os.path.join(raiz, 'host', 'gxm.c')
s12 = open(f12, encoding='utf-8').read()
if 'pb_res_empieza' not in s12:
    defs = r"""
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (resumen de escenas, PB_RESUMEN=fotograma) */
static int pb_res_on = -1, pb_res_activo; static long pb_res_fot; static unsigned pb_res_nesc;
typedef struct { uint32_t f, v; unsigned n, idx; } PbRes;
static PbRes pb_res[96]; static int pb_nres; static unsigned pb_res_draws, pb_res_idx;
static uint32_t pb_res_addr, pb_res_w, pb_res_h;
static void pb_res_nombre(uint32_t g, char *b, size_t n) {
    size_t k = 0; b[0] = 0;
    if (!g) { snprintf(b, n, "?"); return; }
    uint32_t np = gxp_nparams(g), base = gxp_params(g);
    for (uint32_t i = 0; i < np && i < 64 && k + 2 < n; i++) {
        if (p_cat(base + 16 * i) != 2) continue;
        char nm[48]; p_name_str(base + 16 * i, nm, sizeof(nm));
        k += (size_t)snprintf(b + k, n - k, "%s%s", k ? "+" : "", nm);
    }
    if (!b[0]) snprintf(b, n, "(sin texturas)");
}
static void pb_res_empieza(uint32_t addr, uint32_t w, uint32_t h) {
    if (pb_res_on < 0) { const char *e = getenv("PB_RESUMEN"); pb_res_on = e ? 1 : 0; pb_res_fot = e ? atol(e) : 0; }
    if (!pb_res_on || pb_res_activo == 2) return;
    extern unsigned port_display_frames(void);
    unsigned fr = port_display_frames();
    if (!pb_res_activo && (long)fr >= pb_res_fot) { pb_res_activo = 1; pb_res_fot = fr; printf("[resumen] === fotograma %u ===\n", fr); }
    else if (pb_res_activo == 1 && (long)fr > pb_res_fot) { pb_res_activo = 2; printf("[resumen] === fin: %u escenas ===\n", pb_res_nesc); return; }
    if (pb_res_activo != 1) return;
    pb_nres = 0; pb_res_draws = pb_res_idx = 0; pb_res_addr = addr; pb_res_w = w; pb_res_h = h; pb_res_nesc++;
}
static void pb_res_draw(uint32_t fg, uint32_t vg, uint32_t n) {
    if (pb_res_activo != 1) return;
    pb_res_draws++; pb_res_idx += n;
    for (int k = 0; k < pb_nres; k++) if (pb_res[k].f == fg && pb_res[k].v == vg) { pb_res[k].n++; pb_res[k].idx += n; return; }
    if (pb_nres < 96) { pb_res[pb_nres].f = fg; pb_res[pb_nres].v = vg; pb_res[pb_nres].n = 1; pb_res[pb_nres].idx = n; pb_nres++; }
}
static void pb_res_termina(void) {
    if (pb_res_activo != 1) return;
    printf("[resumen] escena %u 0x%08X %ux%u: %u dibujos, %u indices, %d programas\n", pb_res_nesc, pb_res_addr, pb_res_w, pb_res_h,
           pb_res_draws, pb_res_idx, pb_nres);
    for (int a = 0; a < pb_nres; a++) for (int b = a + 1; b < pb_nres; b++)
        if (pb_res[b].n > pb_res[a].n) { PbRes t = pb_res[a]; pb_res[a] = pb_res[b]; pb_res[b] = t; }
    for (int k = 0; k < pb_nres; k++) {
        char nf[200], nv[200]; pb_res_nombre(pb_res[k].f, nf, sizeof(nf));
        nv[0] = 0;
        { uint32_t g = pb_res[k].v; uint32_t np = g ? gxp_nparams(g) : 0, base = g ? gxp_params(g) : 0; size_t q = 0;
          for (uint32_t i = 0; i < np && i < 64 && q + 2 < sizeof(nv); i++) if (p_cat(base + 16 * i) == 1) {
              char nm[48]; p_name_str(base + 16 * i, nm, sizeof(nm)); q += (size_t)snprintf(nv + q, sizeof(nv) - q, "%s%s", q ? "," : "", nm);
              if (q > 60) break; } }
        printf("[resumen]    %4u dib %7u idx  f=0x%08X v=0x%08X  tex: %s | vu: %s\n", pb_res[k].n, pb_res[k].idx, pb_res[k].f, pb_res[k].v, nf, nv);
    }
}
#define PB_RES_EMPIEZA(a, w, h) pb_res_empieza(a, w, h)
#define PB_RES_DRAW(f, v, n) pb_res_draw(f, v, n)
#define PB_RES_TERMINA() pb_res_termina()
#else
#define PB_RES_EMPIEZA(a, w, h) ((void)0)
#define PB_RES_DRAW(f, v, n) ((void)0)
#define PB_RES_TERMINA() ((void)0)
#endif
"""
    ancla = 'void vita_hle_sceGxmBeginScene(void) {'
    vb = "    Surf *s = surf_find(arg(2));\n    c->in_scene = 1;\n"
    nb = "    Surf *s = surf_find(arg(2));\n    PB_RES_EMPIEZA(s ? s->data : 0, s ? s->w : 0, s ? s->h : 0);\n    c->in_scene = 1;\n"
    ve = "void vita_hle_sceGxmEndScene(void) {\n    Ctx *c = ctx_find(r0);\n    if (!c) { r0 = GXM_ERR_INVALID_VALUE; return; }\n"
    ne = ve + "    PB_RES_TERMINA();\n"
    vd = "    uint32_t ptype = prim >> 26;                   /* 0 tri, 1 lines, 2 points, 3 strip, 4 fan */\n"
    nd = "    PB_RES_DRAW(f->gxp, v->gxp, icount);\n" + vd
    if ancla in s12 and vb in s12 and ve in s12 and vd in s12:
        s12 = s12.replace(ancla, defs + ancla, 1).replace(vb, nb, 1).replace(ve, ne, 1).replace(vd, nd, 1)
        cambios += 1
        open(f12, 'w', encoding='utf-8').write(s12)
    else:
        print('AVISO: parche 12 no aplicado (no encontré los anclajes)')

# 13. Sin público (opción): [android] publico=0 o PB_PUBLICO=0. El público son figuras planas
#     (fragmento con el sampler CrowdSpriteTextureSampler, ~47 dibujos en la escena principal) que
#     leen un atlas que el juego dibuja cada fotograma con ~40 espectadores 3D con esqueleto (escena de
#     256x256 y otra de 512x1024 sobre la misma superficie, ~70 000 índices). Se saltan las figuras y,
#     apuntando la superficie del atlas que leían, también los dibujos de esas escenas.
f13 = os.path.join(raiz, 'host', 'gxm.c')
s13 = open(f13, encoding='utf-8').read()
if 'pb_sin_publico' not in s13:
    defs = r"""
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (sin público, [android] publico=0) */
static int pb_sin_publico(Ctx *c, FProg *f) {
    static int on = -1;
    static uint32_t atlas[8]; static int natlas;
    static struct { uint32_t gxp; int8_t es; } cache[512];
    if (on < 0) {
        extern int launcher_ini_int(const char *section, const char *key, int def);
        const char *e = getenv("PB_PUBLICO");
        on = e && *e ? (*e == '0') : !launcher_ini_int("android", "publico", 0);   /* por defecto sin público */
        if (on) printf("[publico] sin público (android publico=0)\n");
    }
    if (!on) return 0;
    uint32_t t = gpu_target_addr();
    for (int i = 0; i < natlas; i++) if (atlas[i] == t) return 1;   /* escena del atlas del público */
    uint32_t h = (f->gxp >> 4) & 511;
    if (cache[h].gxp != f->gxp) {
        cache[h].gxp = f->gxp;
        cache[h].es = gxp_find(f->gxp, "CrowdSpriteTextureSampler") ? 1 : 0;
    }
    if (!cache[h].es) return 0;
    uint32_t p = gxp_find(f->gxp, "CrowdSpriteTextureSampler");
    const uint8_t *tw = p ? frag_tex_words(c, (int)p_res(p)) : NULL;
    if (tw) {
        GpuTexture tx; memset(&tx, 0, sizeof(tx)); tex_from_words(tw, &tx);
        int k; for (k = 0; k < natlas && atlas[k] != tx.data; k++) {}
        if (k == natlas && natlas < 8 && tx.data) { atlas[natlas++] = tx.data; printf("[publico] atlas en 0x%08X: sus escenas no se dibujan\n", tx.data); }
    }
    return 1;
}
#endif
"""
    ancla = 'static void do_draw(Ctx *c, uint32_t vprog, const uint32_t streams[16], uint32_t prim, uint32_t itype,'
    vd = "    PB_RES_DRAW(f->gxp, v->gxp, icount);\n"
    nd = vd + "#ifdef __ANDROID__\n    if (pb_sin_publico(c, f)) { g_skipped++; return; }\n#endif\n"
    if ancla in s13 and vd in s13:
        s13 = s13.replace(ancla, defs + ancla, 1).replace(vd, nd, 1)
        cambios += 1
        open(f13, 'w', encoding='utf-8').write(s13)
    else:
        print('AVISO: parche 13 no aplicado')

# 14. Vértices sin copia intermedia. prepara_crudo copiaba los vértices del juego a g_cbuf y luego
#     gpu_draw_v3k_crudo los copiaba otra vez al búfer de la GPU (en el acto). Si están seguidos en la
#     memoria del juego (rango, por instancia o sin paso: casi todos), se pasa el puntero directo y se
#     copian una sola vez.
f14 = os.path.join(raiz, 'host', 'gxm.c')
s14 = open(f14, encoding='utf-8').read()
v14 = """        uint8_t *dst = g_cbuf[b];
        if (!stride) memcpy(dst, base, need[b]);"""
n14 = """        uint8_t *dst = g_cbuf[b];
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (vértices sin copia intermedia) */
        if (!stride) { bdata[b] = base; bbytes[b] = need[b]; continue; }
        if (lay->inst[b]) { bdata[b] = base; bbytes[b] = (ninst - 1) * stride + need[b]; continue; }
        if (rango) { bdata[b] = base + (size_t)lo * stride; bbytes[b] = (span - 1) * stride + need[b]; continue; }
#endif
        if (!stride) memcpy(dst, base, need[b]);"""
if v14 in s14 and 'sin copia intermedia' not in s14:
    s14 = s14.replace(v14, n14, 1); cambios += 1
    open(f14, 'w', encoding='utf-8').write(s14)

# 15. Texturas estables: el hash del contenido de cada textura se calculaba en cada escena (cientos de
#     texturas del estadio, leídas enteras cada fotograma: ~90 ms/s del hilo de dibujo). Si una textura
#     lleva 60 comprobaciones seguidas sin cambiar, se vuelve a comprobar cada 8 fotogramas (un cambio
#     de verdad se ve como mucho ~0,25 s tarde; las que cambian se siguen comprobando siempre).
#     PB_TEX_SIEMPRE=1 lo quita.
f15 = os.path.join(raiz, 'host', 'gpu_vk.c')
s15 = open(f15, encoding='utf-8').read()
if 'pb_estable' not in s15:
    v1 = "    int next;                               /* cadena de la tabla g_tex_bucket (índice+1) */\n"
    n1 = v1 + "    uint32_t pb_estable, pb_chk;            /* ANDROID: comprobaciones seguidas sin cambiar, fotograma de la última */\n"
    v2 = "    if (x && x->scene == g_stat_scenes) return x;\n"
    n2 = v2 + """#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (texturas estables) */
    {
        static int siempre = -1;
        if (siempre < 0) { const char *e = getenv("PB_TEX_SIEMPRE"); siempre = e && *e == '1'; }
        extern unsigned port_display_frames(void);
        if (!siempre && x && x->pb_estable >= 60 && port_display_frames() - x->pb_chk < 8) { x->scene = g_stat_scenes; return x; }
    }
#endif
"""
    v3 = "    uint32_t hash = content_hash(t);\n"
    n3 = v3 + """#ifdef __ANDROID__
    if (x) {
        extern unsigned port_display_frames(void);
        if (x->hash == hash) { if (x->pb_estable < 1000000u) x->pb_estable++; } else x->pb_estable = 0;
        x->pb_chk = port_display_frames();
    }
#endif
"""
    if v1 in s15 and v2 in s15 and v3 in s15:
        s15 = s15.replace(v1, n1, 1).replace(v2, n2, 1).replace(v3, n3, 1)
        cambios += 1
        open(f15, 'w', encoding='utf-8').write(s15)
    else:
        print('AVISO: parche 15 no aplicado')

# 16. Tirones: cuenta los fotogramas de más de 36 ms y el peor de cada intervalo de 5 s (línea
#     "[lentos]" del registro; las medias de FPS los esconden).
f16 = os.path.join(raiz, 'host', 'hle_display.c')
s16 = open(f16, encoding='utf-8').read()
v16 = """        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - t0.QuadPart) / (double)f0.QuadPart;
        if (dt >= 5.0) {"""
n16 = """        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - t0.QuadPart) / (double)f0.QuadPart;
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (tirones) */
        static LARGE_INTEGER pb_ult; static unsigned pb_lentos, pb_muy; static double pb_peor;
        if (pb_ult.QuadPart) {
            double ft = (double)(now.QuadPart - pb_ult.QuadPart) * 1000.0 / (double)f0.QuadPart;
            if (ft > 36.0) pb_lentos++;
            if (ft > 50.0) pb_muy++;
            if (ft > pb_peor) pb_peor = ft;
        }
        pb_ult = now;
        if (dt >= 5.0) {
            printf("[lentos] %u fotogramas > 36 ms, %u > 50 ms, peor %.0f ms (de %u)\\n", pb_lentos, pb_muy, pb_peor, g_frames - n0);
            pb_lentos = pb_muy = 0; pb_peor = 0;
        }
#endif
        if (dt >= 5.0) {"""
if v16 in s16 and '(tirones)' not in s16:
    s16 = s16.replace(v16, n16, 1); cambios += 1
    open(f16, 'w', encoding='utf-8').write(s16)

# 17. Esperas falsas por textura: antes de leer una textura se esperaba a las escenas en vuelo que
#     escriben en su memoria, calculando el tamaño como ancho x alto x 8 bytes (mucho más que la textura
#     real): chocaba con superficies vecinas y el hilo de dibujo esperaba a la GPU sin motivo (~250 ms/s
#     con 16 escenas en vuelo). Se usa el tamaño real de los datos.
f17 = os.path.join(raiz, 'host', 'gpu_vk.c')
s17 = open(f17, encoding='utf-8').read()
v17 = """    pend_overlap(t->data, t->width * t->height * 8u * (t->type == GPU_TEX_CUBE ? 6u : 1u));
    uint32_t tbytes = t->type == GPU_TEX_CUBE ? cube_face_bytes(t) * 6u : tex_data_size(t);"""
n17 = """#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (esperas falsas por textura) */
    uint32_t tbytes = t->type == GPU_TEX_CUBE ? cube_face_bytes(t) * 6u : tex_data_size(t);
    pend_overlap(t->data, tbytes ? tbytes : t->width * t->height * 8u);
#else
    pend_overlap(t->data, t->width * t->height * 8u * (t->type == GPU_TEX_CUBE ? 6u : 1u));
    uint32_t tbytes = t->type == GPU_TEX_CUBE ? cube_face_bytes(t) * 6u : tex_data_size(t);
#endif"""
if v17 in s17 and 'esperas falsas por textura' not in s17:
    s17 = s17.replace(v17, n17, 1); cambios += 1
    open(f17, 'w', encoding='utf-8').write(s17)

# 18. Traza (pruebas): PB_OVERLAP_TRAZA=1 dice qué textura espera a qué escena en pend_overlap.
f18 = os.path.join(raiz, 'host', 'gpu_vk.c')
s18 = open(f18, encoding='utf-8').read()
v18 = "        if (t && !t->resident && a < t->addr + t->gstride * t->h * g_tfmt_bpp[t->tf] && t->addr < a + n) CTXC(&g_ctx[k], 4);\n"
n18 = """        if (t && !t->resident && a < t->addr + t->gstride * t->h * g_tfmt_bpp[t->tf] && t->addr < a + n) {
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (traza de esperas por textura) */
            { static int tr = -1; static unsigned nt; if (tr < 0) tr = getenv("PB_OVERLAP_TRAZA") ? 1 : 0;
              if (tr && nt++ < 60) printf("[overlap] textura 0x%08X (%u B) espera a la escena 0x%08X %ux%u tf %d\\n", a, n, t->addr, t->w, t->h, t->tf); }
#endif
            CTXC(&g_ctx[k], 4);
        }
"""
if v18 in s18 and 'traza de esperas por textura' not in s18:
    s18 = s18.replace(v18, n18, 1); cambios += 1
    open(f18, 'w', encoding='utf-8').write(s18)

# 19. Texturas que pinta la GPU en el mismo fotograma (los cubemaps de luz de cada jugador, 8x8x6,
#     dentro de una superficie de 64x512): para leerlas se esperaba a que la GPU acabara esa escena y
#     volviera a memoria (~220 ms/s de espera en el hilo de dibujo). Si la textura ya existe, se usa la
#     del fotograma anterior (33 ms de retraso en la luz del jugador). Las nuevas siguen esperando.
#     PB_ESPERA_TEXTURAS=1 vuelve a esperar siempre.
f19 = os.path.join(raiz, 'host', 'gpu_vk.c')
s19 = open(f19, encoding='utf-8').read()
v19 = """#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (esperas falsas por textura) */
    uint32_t tbytes = t->type == GPU_TEX_CUBE ? cube_face_bytes(t) * 6u : tex_data_size(t);
"""
n19 = v19 + """    if (x && x->img) {   /* ANDROID: parches/aplicar.py (textura del fotograma anterior si la GPU aún la pinta) */
        static int espera = -1;
        if (espera < 0) { const char *e = getenv("PB_ESPERA_TEXTURAS"); espera = e && *e == '1'; }
        uint32_t a = t->data, n = tbytes ? tbytes : t->width * t->height * 8u;
        for (int k = 0; !espera && k < g_nctx; k++) {
            Target *g = g_ctx[k].busy ? g_ctx[k].tgt : NULL;
            if (g && !g->resident && a < g->addr + g->gstride * g->h * g_tfmt_bpp[g->tf] && g->addr < a + n) { x->scene = g_stat_scenes; return x; }
        }
    }
"""
if v19 in s19 and 'textura del fotograma anterior' not in s19:
    s19 = s19.replace(v19, n19, 1); cambios += 1
    open(f19, 'w', encoding='utf-8').write(s19)

# 20. Superficies que solo usa la GPU: si una superficie de al menos 128x128 se ha servido como textura
#     desde la imagen de la GPU (target_texture) y nunca se ha leído por el camino de memoria
#     (pend_overlap), deja de volver a la memoria del juego. Era sobre todo la de 960x544 de las sombras
#     de los jugadores (PlayerShadowsTarget): 2 MB por fotograma, ~10 % del hilo de dibujo en memcpy.
#     El mapa de exposición (32x32) y los cubemaps de luz (se leen de memoria) siguen igual.
#     PB_FB_MEMORIA=1 lo quita (igual que el parche 7).
f20 = os.path.join(raiz, 'host', 'gpu_vk.c')
s20 = open(f20, encoding='utf-8').read()
if 'pb_gpu' not in s20:
    reps = [
        ("    uint32_t gstride;                                         /* stride guest de su última escena */\n} Target;",
         "    uint32_t gstride;                                         /* stride guest de su última escena */\n    uint8_t pb_gpu, pb_cpu;                                   /* ANDROID: usada como textura desde la GPU / leída de memoria */\n} Target;"),
        ("#define PB_FB_SOLO_GPU(t) pb_fb_solo_gpu((t)->addr)",
         "#define PB_FB_SOLO_GPU(t) (pb_fb_solo_gpu((t)->addr) || (pb_fb_solo_gpu(0xFFFFFFFFu) >= 0 && (t)->pb_gpu && !(t)->pb_cpu && (t)->w * (t)->h >= 16384u && !getenv(\"PB_FB_MEMORIA\")))"),
        ("        if (!g->resident && lineal_mem) { if (g->pend) CTXC(&g_ctx[g->pend - 1], 0); return NULL; }\n",
         "        if (!g->resident && lineal_mem) { if (g->pend) CTXC(&g_ctx[g->pend - 1], 0); return NULL; }\n#ifdef __ANDROID__\n        g->pb_gpu = 1;   /* ANDROID: parches/aplicar.py (superficies que solo usa la GPU) */\n#endif\n"),
        ("        if (t && !t->resident && a < t->addr + t->gstride * t->h * g_tfmt_bpp[t->tf] && t->addr < a + n) {\n",
         "        if (t && !t->resident && a < t->addr + t->gstride * t->h * g_tfmt_bpp[t->tf] && t->addr < a + n) {\n#ifdef __ANDROID__\n            t->pb_cpu = 1;\n#endif\n"),
    ]
    ok = all(v in s20 for v, n in reps)
    if ok:
        for v, n in reps: s20 = s20.replace(v, n, 1)
        cambios += 1
        open(f20, 'w', encoding='utf-8').write(s20)
    else:
        print('AVISO: parche 20 no aplicado:', [v[:50] for v, n in reps if v not in s20])


# 21. Caché de vértices entre fotogramas. Cada dibujo copiaba sus vértices al búfer de la escena (~10 %
#     del hilo de dibujo en memcpy), aunque casi todos son fijos (estadio; jugadores: el esqueleto va en
#     la GPU). Ahora cada trozo (puntero, bytes) se guarda en un búfer persistente de 64 MB:
#       - si sigue igual (hash) se reutiliza; tras 8 comprobaciones iguales solo se mira cada 8 fotogramas;
#       - si cambia, se escribe en sitio nuevo (nunca encima de lo que la GPU puede estar leyendo);
#       - si cambia 3 veces se marca dinámico y va por el camino de siempre;
#       - si el búfer se llena, se esperan las escenas en vuelo y se empieza de cero.
#     PB_SIN_CACHE_VERTICES=1 lo quita.
f21 = os.path.join(raiz, 'host', 'gpu_vk.c')
s21 = open(f21, encoding='utf-8').read()
if 'vc_busca' not in s21:
    defs = r"""
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (caché de vértices entre fotogramas) */
#define VC_SIZE (64u << 20)
#define VC_ENT  16384
typedef struct { const uint8_t *p; uint32_t n, off, hash, estable, chk, gen, cambios; uint8_t dinamico; } VcEnt;
static Buffer g_vc;
static uint32_t g_vc_off, g_vc_gen = 1;
static int g_vc_lleno;
/* Al empezar una escena (antes de ningún dibujo): si se llenó, esperar las escenas en vuelo y empezar de cero. */
static void vc_inicio_escena(void) { if (g_vc_lleno) { ctx_complete_all(); g_vc_gen++; g_vc_off = 0; g_vc_lleno = 0; } }
static int g_vc_on = -1;
static VcEnt g_vce[VC_ENT];
unsigned long g_vc_si, g_vc_no, g_vc_din;
static uint32_t vc_hash(const uint8_t *p, uint32_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return (uint32_t)(h ^ (h >> 32));
}
static int vc_copia(VcEnt *e, const uint8_t *p, uint32_t n) {
    uint32_t off = (g_vc_off + 15u) & ~15u;
    if (off + n > VC_SIZE) { g_vc_lleno = 1; return 0; }   /* lleno: se vacía al empezar la escena siguiente */
    memcpy((uint8_t *)g_vc.ptr + off, p, n);
    e->off = off; e->gen = g_vc_gen; g_vc_off = off + n;
    return 1;
}
static int vc_busca(const uint8_t *p, uint32_t n, VkBuffer *buf, VkDeviceSize *off) {
    if (g_vc_on < 0) {
        const char *ev = getenv("PB_SIN_CACHE_VERTICES");
        g_vc_on = !(ev && *ev == '1') && buffer_new(&g_vc, VC_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        printf("[vcache] caché de vértices: %s\n", g_vc_on ? "sí" : "no");
    }
    if (!g_vc_on || n < 256) return 0;   /* trozos pequeños: copiar es más barato */
    { static unsigned long nn; if (++nn % 60000 == 0) printf("[vcache] aciertos %lu, copias %lu, dinamicos %lu, usado %u KB\n", g_vc_si, g_vc_no, g_vc_din, g_vc_off >> 10); }
    extern unsigned port_display_frames(void);
    uint32_t fr = port_display_frames();
    uint32_t h = (uint32_t)((((uintptr_t)p >> 4) ^ n * 2654435761u) * 0x9E3779B1u) & (VC_ENT - 1);
    VcEnt *e = NULL, *libre = NULL;
    for (int k = 0; k < 8; k++) {
        VcEnt *c = &g_vce[(h + (uint32_t)k) & (VC_ENT - 1)];
        if (c->p == p && c->n == n) { e = c; break; }
        if (!c->p && !libre) libre = c;
    }
    if (!e) {
        if (!libre) libre = &g_vce[h];   /* sustituir */
        e = libre; memset(e, 0, sizeof(*e)); e->p = p; e->n = n;
        e->hash = vc_hash(p, n); e->chk = fr;
        if (!vc_copia(e, p, n)) { e->p = NULL; return 0; }
        g_vc_no++;
        *buf = g_vc.buf; *off = e->off; return 1;
    }
    if (e->dinamico) { g_vc_din++; return 0; }
    if (e->gen == g_vc_gen && e->estable >= 8 && fr - e->chk < 8) { g_vc_si++; *buf = g_vc.buf; *off = e->off; return 1; }
    uint32_t x = vc_hash(p, n);
    e->chk = fr;
    if (x == e->hash && e->gen == g_vc_gen) { e->estable++; g_vc_si++; *buf = g_vc.buf; *off = e->off; return 1; }
    if (x != e->hash) { e->hash = x; e->estable = 0; if (++e->cambios >= 3) { e->dinamico = 1; g_vc_din++; return 0; } }
    if (!vc_copia(e, p, n)) return 0;
    g_vc_no++;
    *buf = g_vc.buf; *off = e->off; return 1;
}
#endif
"""
    ancla = "static void draw_v3k_x(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, uint32_t uint_mask, const float *verts, uint32_t nv,\n                       const uint32_t *idx"
    v2 = """        for (uint32_t b = 0; b < lay->nbind; b++) {
            memcpy((uint8_t *)g_vb.ptr + o, bdata[b], bbytes[b]);
            bbufs[b] = g_vb.buf; boffs[b] = o;
            o += (bbytes[b] + 15u) & ~15u;
        }"""
    n2 = """        for (uint32_t b = 0; b < lay->nbind; b++) {
#ifdef __ANDROID__
            if (vc_busca(bdata[b], bbytes[b], &bbufs[b], &boffs[b])) { o += (bbytes[b] + 15u) & ~15u; continue; }
#endif
            memcpy((uint8_t *)g_vb.ptr + o, bdata[b], bbytes[b]);
            bbufs[b] = g_vb.buf; boffs[b] = o;
            o += (bbytes[b] + 15u) & ~15u;
        }"""
    # la definición de draw_v3k_x: la segunda aparición (la primera es el prototipo de gpu_draw_v3k_crudo)
    i = s21.find(ancla)
    j = s21.find(ancla, i + 1)
    k = j if j >= 0 else i
    v3 = """    EnterCriticalSection(&g_cs);
    if (!white_texture()) { LeaveCriticalSection(&g_cs); return; }
    int tf = tfmt_from_gxm(gxm_format);
"""
    n3 = """    EnterCriticalSection(&g_cs);
    if (!white_texture()) { LeaveCriticalSection(&g_cs); return; }
#ifdef __ANDROID__
    vc_inicio_escena();
#endif
    int tf = tfmt_from_gxm(gxm_format);
"""
    if k >= 0 and v2 in s21 and v3 in s21:
        s21 = s21[:k] + defs + s21[k:]
        s21 = s21.replace(v2, n2, 1)
        # vc_inicio_escena se usa en gpu_scene_begin (antes de draw_v3k_x): prototipo arriba
        s21 = s21.replace(v3, n3, 1)
        s21 = s21.replace("""#define PB_FB_SOLO_GPU(t) 0
#endif
""", """#define PB_FB_SOLO_GPU(t) 0
#endif
#ifdef __ANDROID__
static void vc_inicio_escena(void);
#endif
""", 1)
        cambios += 1
        open(f21, 'w', encoding='utf-8').write(s21)
    else:
        print('AVISO: parche 21 no aplicado', k, v2 in s21)

# 22. ADPF: el limitador de FPS informa cada fotograma del trabajo real (desde que despertó hasta que
#     va a dormir) a adpf_android.c, que se lo pasa al sistema (objetivo 33,3 ms).
f22 = os.path.join(raiz, 'host', 'hle_display.c')
s22 = open(f22, encoding='utf-8').read()
v22a = """        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = now.QuadPart / f, plazo = g_qpc_last.QuadPart / f + target;"""
n22a = """        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (ADPF) */
        static LARGE_INTEGER pb_desp;
        if (pb_desp.QuadPart) { extern void pb_adpf_trabajo(long long ns); pb_adpf_trabajo((long long)((double)(now.QuadPart - pb_desp.QuadPart) * 1e9 / f)); }
#endif
        double t = now.QuadPart / f, plazo = g_qpc_last.QuadPart / f + target;"""
v22b = """        } else {
            g_qpc_last.QuadPart = (LONGLONG)(plazo * f);
        }
    }
"""
n22b = """        } else {
            g_qpc_last.QuadPart = (LONGLONG)(plazo * f);
        }
#ifdef __ANDROID__
        QueryPerformanceCounter(&pb_desp);
#endif
    }
"""
if v22a in s22 and v22b in s22 and '(ADPF)' not in s22:
    s22 = s22.replace(v22a, n22a, 1).replace(v22b, n22b, 1); cambios += 1
    open(f22, 'w', encoding='utf-8').write(s22)
elif '(ADPF)' not in s22:
    print('AVISO: parche 22 no aplicado', v22a in s22, v22b in s22)

# 23. Hilo GPU. El hilo de dibujo del juego («Display Engine») hacía todo: código del juego, traducir
#     cada dibujo y además grabar las órdenes de Vulkan (texturas, descriptores, driver de Adreno), ~24 ms
#     de cada 33 y más en las jugadas pesadas (bajadas a 20 FPS). Ahora los dibujos (gpu_draw_v3k y
#     gpu_draw_v3k_crudo) van a una cola con copia de sus datos y otro hilo («GPU Hilo») graba Vulkan en
#     paralelo. Empezar/terminar escena, presentar y sincronizar vacían antes la cola y se hacen en el
#     acto (al terminar la escena todo está grabado: el juego ya puede reutilizar su memoria, como con la
#     GPU de verdad). Los vértices se resuelven en el hilo de dibujo con la caché del parche 21 (los que
#     no están en caché se copian a la cola). [android] hilo_gpu=0 o PB_HILO_GPU=0 lo quita.
f23 = os.path.join(raiz, 'host', 'gpu_vk.c')
s23 = open(f23, encoding='utf-8').read()
if 'gpu_scene_begin_impl' not in s23:
    renombres = r"""
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (hilo GPU): las funciones públicas pasan a *_impl */
#define gpu_scene_begin gpu_scene_begin_impl
#define gpu_scene_depth gpu_scene_depth_impl
#define gpu_scene_end gpu_scene_end_impl
#define gpu_draw_v3k_crudo gpu_draw_v3k_crudo_impl
#define gpu_draw_v3k gpu_draw_v3k_impl
#define gpu_draw gpu_draw_impl
#define gpu_draw_prog gpu_draw_prog_impl
#define gpu_present gpu_present_impl
#define gpu_sync_all gpu_sync_all_impl
#define gpu_sync_all_m gpu_sync_all_m_impl
void gpu_scene_depth(uint32_t depth_addr);
void gpu_scene_begin(uint32_t color_addr, uint32_t width, uint32_t height, uint32_t stride, int load_ds, uint32_t gxm_format,
                     uint32_t surf_type, uint32_t rw, uint32_t rh);
void gpu_draw(const GpuDraw *d, const GpuVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni);
void gpu_scene_end(void);
void gpu_draw_prog(const GpuDraw *d, int vsm, int fsm, uint32_t nattr, const float *verts, uint32_t nv,
                   const uint32_t *idx, uint32_t ni, const uint32_t *mem, uint32_t memwords, uint32_t vsa, uint32_t fsa,
                   const GpuTexture *const tex[16]);
void gpu_draw_v3k(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, uint32_t uint_mask, const float *verts, uint32_t nv,
                  const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes, const void *finfo,
                  uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data, const void *fdata, uint32_t fbytes_data,
                  const float viewport[4], const GpuTexture *const tex[16]);
void gpu_draw_v3k_crudo(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, const GpuVLayout *lay,
                        const uint8_t *const bdata[GPU_VLAY_MAXB], const uint32_t bbytes[GPU_VLAY_MAXB], uint32_t ninst,
                        uint32_t nv, const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes,
                        const void *finfo, uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data,
                        const void *fdata, uint32_t fbytes_data, const float viewport[4], const GpuTexture *const tex[16]);
void gpu_present(uint32_t addr, uint32_t width, uint32_t height, uint32_t stride);
void gpu_sync_all(void);
void gpu_sync_all_m(int motivo);
/* El hilo GPU pasa los búferes de vértices ya resueltos (caché) a draw_v3k_x. */
static int g_pre_activo;
static VkBuffer g_pre_buf[GPU_VLAY_MAXB];
static VkDeviceSize g_pre_off[GPU_VLAY_MAXB];
#endif
"""
    ancla = '#include "gpu.h"\n'
    v_loop = "            if (vc_busca(bdata[b], bbytes[b], &bbufs[b], &boffs[b])) { o += (bbytes[b] + 15u) & ~15u; continue; }\n"
    n_loop = """            if (g_pre_activo) {   /* hilo GPU: la caché ya se consultó en el hilo de dibujo */
                if (g_pre_buf[b]) { bbufs[b] = g_pre_buf[b]; boffs[b] = g_pre_off[b]; o += (bbytes[b] + 15u) & ~15u; continue; }
            } else if (vc_busca(bdata[b], bbytes[b], &bbufs[b], &boffs[b])) { o += (bbytes[b] + 15u) & ~15u; continue; }
"""
    final = r"""
#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (hilo GPU): cola de órdenes y funciones públicas */
#undef gpu_scene_begin
#undef gpu_scene_depth
#undef gpu_scene_end
#undef gpu_draw_v3k_crudo
#undef gpu_draw_v3k
#undef gpu_draw
#undef gpu_draw_prog
#undef gpu_present
#undef gpu_sync_all
#undef gpu_sync_all_m
#include <pthread.h>
#define CQ_SIZE (48u << 20)
enum { CQ_SALTO = 1, CQ_CRUDO = 2, CQ_V3K = 3 };
typedef struct {
    uint32_t tipo, bytes;
    GpuDraw d;
    int vsm, fsm;
    uint32_t nloc, uint_mask, ninst, nv, ni;
    uint32_t vinfo_bytes, finfo_bytes, vbytes_data, fbytes_data, verts_bytes;
    GpuVLayout lay;
    VkBuffer pbuf[GPU_VLAY_MAXB];
    VkDeviceSize poff[GPU_VLAY_MAXB];
    uint32_t bbytes[GPU_VLAY_MAXB], o_vb[GPU_VLAY_MAXB];
    uint32_t o_idx, o_vinfo, o_finfo, o_vdata, o_fdata, o_verts;
    float viewport[4];
    uint32_t texmask;
    GpuTexture tex[16];
} CqDraw;
static uint8_t *g_cq;
static size_t g_cq_head, g_cq_tail;
static pthread_mutex_t g_cq_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cq_cw = PTHREAD_COND_INITIALIZER, g_cq_cp = PTHREAD_COND_INITIALIZER;
static int g_cq_on = -1, g_cq_w_espera, g_cq_p_espera;
unsigned long g_cq_dibujos, g_cq_esperas;

static void cq_ejecuta(CqDraw *r) {
    const uint8_t *base = (const uint8_t *)r;
    const GpuTexture *tp[16];
    for (int u = 0; u < 16; u++) tp[u] = (r->texmask >> u & 1) ? &r->tex[u] : NULL;
    if (r->tipo == CQ_CRUDO) {
        const uint8_t *bd[GPU_VLAY_MAXB];
        for (uint32_t b = 0; b < GPU_VLAY_MAXB; b++) {
            g_pre_buf[b] = r->pbuf[b]; g_pre_off[b] = r->poff[b];
            bd[b] = r->pbuf[b] ? NULL : base + r->o_vb[b];
        }
        g_pre_activo = 1;
        gpu_draw_v3k_crudo_impl(&r->d, r->vsm, r->fsm, r->nloc, &r->lay, bd, r->bbytes, r->ninst, r->nv,
                                (const uint32_t *)(base + r->o_idx), r->ni, base + r->o_vinfo, r->vinfo_bytes,
                                base + r->o_finfo, r->finfo_bytes, base + r->o_vdata, r->vbytes_data,
                                base + r->o_fdata, r->fbytes_data, r->viewport, tp);
        g_pre_activo = 0;
    } else if (r->tipo == CQ_V3K) {
        gpu_draw_v3k_impl(&r->d, r->vsm, r->fsm, r->nloc, r->uint_mask, (const float *)(base + r->o_verts), r->nv,
                          (const uint32_t *)(base + r->o_idx), r->ni, base + r->o_vinfo, r->vinfo_bytes,
                          base + r->o_finfo, r->finfo_bytes, base + r->o_vdata, r->vbytes_data,
                          base + r->o_fdata, r->fbytes_data, r->viewport, tp);
    }
}
static void *cq_hilo(void *x) {
    (void)x;
    pthread_setname_np(pthread_self(), "GPU Hilo");
    { extern void pb_afinidad_nombre(const char *); pb_afinidad_nombre("GPU Hilo"); }
    { extern void pb_adpf_registra(int, pid_t); pb_adpf_registra(2, gettid()); }
    for (;;) {
        pthread_mutex_lock(&g_cq_mx);
        while (g_cq_head == g_cq_tail) {
            if (g_cq_p_espera) pthread_cond_signal(&g_cq_cp);
            g_cq_w_espera = 1; pthread_cond_wait(&g_cq_cw, &g_cq_mx); g_cq_w_espera = 0;
        }
        size_t t = g_cq_tail;
        pthread_mutex_unlock(&g_cq_mx);
        CqDraw *r = (CqDraw *)(g_cq + t);
        size_t sig = r->tipo == CQ_SALTO ? 0 : t + r->bytes;
        if (r->tipo != CQ_SALTO) cq_ejecuta(r);
        pthread_mutex_lock(&g_cq_mx);
        g_cq_tail = sig;
        if (g_cq_p_espera) pthread_cond_signal(&g_cq_cp);
        pthread_mutex_unlock(&g_cq_mx);
    }
    return NULL;
}
static int cq_activo(void) {
    if (g_cq_on < 0) {
        const char *e = getenv("PB_HILO_GPU");
        g_cq_on = e && *e ? *e != '0' : launcher_ini_int("android", "hilo_gpu", 1);
        if (g_cq_on) {
            g_cq = (uint8_t *)malloc(CQ_SIZE);
            pthread_t th;
            if (!g_cq || pthread_create(&th, NULL, cq_hilo, NULL) != 0) { free(g_cq); g_cq = NULL; g_cq_on = 0; }
            else pthread_detach(th);
        }
        printf("[hilo_gpu] %s\n", g_cq_on ? "sí: los dibujos se graban en otro hilo" : "no");
    }
    return g_cq_on;
}
double g_cq_espera_s;   /* tiempo que el hilo de dibujo espera a la cola (trazador de fotogramas) */
static void cq_vacia(void) {
    if (g_cq_on <= 0 || g_pb_soy_gpu) return;   /* en el propio hilo GPU: nunca esperarse a sí mismo */
    if (g_cq_head == g_cq_tail) return;
    LARGE_INTEGER qa, qb, qf; QueryPerformanceCounter(&qa);
    pthread_mutex_lock(&g_cq_mx);
    while (g_cq_head != g_cq_tail) { g_cq_p_espera = 1; g_cq_esperas++; pthread_cond_wait(&g_cq_cp, &g_cq_mx); g_cq_p_espera = 0; }
    pthread_mutex_unlock(&g_cq_mx);
    QueryPerformanceCounter(&qb); QueryPerformanceFrequency(&qf);
    g_cq_espera_s += (double)(qb.QuadPart - qa.QuadPart) / (double)qf.QuadPart;
}
/* Reserva R bytes contiguos en la cola (espera si está llena). */
static uint8_t *cq_reserva(size_t R) {
    pthread_mutex_lock(&g_cq_mx);
    for (;;) {
        size_t h = g_cq_head, t = g_cq_tail;
        if (h >= t) {
            if (CQ_SIZE - h >= R + 64) { pthread_mutex_unlock(&g_cq_mx); return g_cq + h; }
            if (t > R) { ((CqDraw *)(g_cq + h))->tipo = CQ_SALTO; g_cq_head = 0; if (g_cq_w_espera) pthread_cond_signal(&g_cq_cw); continue; }
        } else if (t - h > R + 64) { pthread_mutex_unlock(&g_cq_mx); return g_cq + h; }
        g_cq_p_espera = 1; g_cq_esperas++; pthread_cond_wait(&g_cq_cp, &g_cq_mx); g_cq_p_espera = 0;
    }
}
static void cq_publica(uint8_t *p, size_t R) {
    pthread_mutex_lock(&g_cq_mx);
    g_cq_head = (size_t)(p - g_cq) + R;
    g_cq_dibujos++;
    if (g_cq_w_espera) pthread_cond_signal(&g_cq_cw);
    pthread_mutex_unlock(&g_cq_mx);
}
#define CQ_AL(x) (((x) + 15u) & ~(size_t)15u)
static uint8_t *cq_nuevo(int tipo, const GpuDraw *d, int vsm, int fsm, uint32_t nloc, const uint32_t *idx, uint32_t ni,
                         const void *vinfo, uint32_t vib, const void *finfo, uint32_t fib, const void *vdata, uint32_t vdb,
                         const void *fdata, uint32_t fdb, const float viewport[4], const GpuTexture *const tex[16],
                         size_t extra, size_t *R_out) {
    size_t o = CQ_AL(sizeof(CqDraw));
    size_t o_idx = o; o += CQ_AL((size_t)ni * 4u);
    size_t o_vi = o; o += CQ_AL(vib);
    size_t o_fi = o; o += CQ_AL(fib);
    size_t o_vd = o; o += CQ_AL(vdb);
    size_t o_fd = o; o += CQ_AL(fdb);
    size_t o_ex = o; o += CQ_AL(extra);
    uint8_t *p = cq_reserva(o);
    CqDraw *r = (CqDraw *)p;
    r->tipo = (uint32_t)tipo; r->bytes = (uint32_t)o;
    r->d = *d; r->vsm = vsm; r->fsm = fsm; r->nloc = nloc; r->ni = ni;
    r->vinfo_bytes = vib; r->finfo_bytes = fib; r->vbytes_data = vdb; r->fbytes_data = fdb;
    r->o_idx = (uint32_t)o_idx; r->o_vinfo = (uint32_t)o_vi; r->o_finfo = (uint32_t)o_fi;
    r->o_vdata = (uint32_t)o_vd; r->o_fdata = (uint32_t)o_fd; r->o_verts = (uint32_t)o_ex;
    if (ni) memcpy(p + o_idx, idx, (size_t)ni * 4u);
    if (vib) memcpy(p + o_vi, vinfo, vib);
    if (fib) memcpy(p + o_fi, finfo, fib);
    if (vdb && vdata) memcpy(p + o_vd, vdata, vdb);
    if (fdb && fdata) memcpy(p + o_fd, fdata, fdb);
    memcpy(r->viewport, viewport, sizeof(r->viewport));
    r->texmask = 0;
    for (int u = 0; u < 16; u++) if (tex && tex[u]) { r->texmask |= 1u << u; r->tex[u] = *tex[u]; }
    *R_out = o;
    return p;
}

void gpu_scene_depth(uint32_t a) { cq_vacia(); gpu_scene_depth_impl(a); }
void gpu_scene_begin(uint32_t color_addr, uint32_t width, uint32_t height, uint32_t stride, int load_ds, uint32_t gxm_format,
                     uint32_t surf_type, uint32_t rw, uint32_t rh) {
    cq_vacia(); gpu_scene_begin_impl(color_addr, width, height, stride, load_ds, gxm_format, surf_type, rw, rh);
}
void gpu_scene_end(void) { cq_vacia(); gpu_scene_end_impl(); }
void gpu_draw(const GpuDraw *d, const GpuVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni) { cq_vacia(); gpu_draw_impl(d, v, nv, idx, ni); }
void gpu_draw_prog(const GpuDraw *d, int vsm, int fsm, uint32_t nattr, const float *verts, uint32_t nv,
                   const uint32_t *idx, uint32_t ni, const uint32_t *mem, uint32_t memwords, uint32_t vsa, uint32_t fsa,
                   const GpuTexture *const tex[16]) {
    cq_vacia(); gpu_draw_prog_impl(d, vsm, fsm, nattr, verts, nv, idx, ni, mem, memwords, vsa, fsa, tex);
}
void gpu_present(uint32_t addr, uint32_t width, uint32_t height, uint32_t stride) { cq_vacia(); gpu_present_impl(addr, width, height, stride); }
void gpu_sync_all(void) { cq_vacia(); gpu_sync_all_impl(); }
void gpu_sync_all_m(int motivo) { cq_vacia(); gpu_sync_all_m_impl(motivo); }

void gpu_draw_v3k(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, uint32_t uint_mask, const float *verts, uint32_t nv,
                  const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes, const void *finfo,
                  uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data, const void *fdata, uint32_t fbytes_data,
                  const float viewport[4], const GpuTexture *const tex[16]) {
    if (g_pb_soy_gpu || !cq_activo() || !g_ok || !(pb_async_activo() ? g_pb_en_escena : g_cur != NULL)) {
        cq_vacia();
        gpu_draw_v3k_impl(d, vsm, fsm, nloc, uint_mask, verts, nv, idx, ni, vinfo, vinfo_bytes, finfo, finfo_bytes, vdata,
                          vbytes_data, fdata, fbytes_data, viewport, tex);
        return;
    }
    size_t vb = (size_t)nv * 16u * (nloc ? nloc : 1u), R;
    uint8_t *p = cq_nuevo(CQ_V3K, d, vsm, fsm, nloc, idx, ni, vinfo, vinfo_bytes, finfo, finfo_bytes, vdata, vbytes_data,
                          fdata, fbytes_data, viewport, tex, vb, &R);
    CqDraw *r = (CqDraw *)p;
    r->uint_mask = uint_mask; r->nv = nv; r->verts_bytes = (uint32_t)vb;
    if (vb && verts) memcpy(p + r->o_verts, verts, vb);
    cq_publica(p, R);
}

void gpu_draw_v3k_crudo(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, const GpuVLayout *lay,
                        const uint8_t *const bdata[GPU_VLAY_MAXB], const uint32_t bbytes[GPU_VLAY_MAXB], uint32_t ninst,
                        uint32_t nv, const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes,
                        const void *finfo, uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data,
                        const void *fdata, uint32_t fbytes_data, const float viewport[4], const GpuTexture *const tex[16]) {
    if (g_pb_soy_gpu || !cq_activo() || !g_ok || !(pb_async_activo() ? g_pb_en_escena : g_cur != NULL) || !lay) {
        cq_vacia();
        gpu_draw_v3k_crudo_impl(d, vsm, fsm, nloc, lay, bdata, bbytes, ninst, nv, idx, ni, vinfo, vinfo_bytes, finfo,
                                finfo_bytes, vdata, vbytes_data, fdata, fbytes_data, viewport, tex);
        return;
    }
    /* vértices: la caché (en este hilo); lo que no está en caché se copia a la cola */
    VkBuffer pb[GPU_VLAY_MAXB]; VkDeviceSize po[GPU_VLAY_MAXB]; size_t extra = 0;
    for (uint32_t b = 0; b < GPU_VLAY_MAXB; b++) {
        pb[b] = VK_NULL_HANDLE; po[b] = 0;
        if (b >= lay->nbind) continue;
        if (!vc_busca(bdata[b], bbytes[b], &pb[b], &po[b])) { pb[b] = VK_NULL_HANDLE; extra += CQ_AL(bbytes[b]); }
    }
    size_t R;
    uint8_t *p = cq_nuevo(CQ_CRUDO, d, vsm, fsm, nloc, idx, ni, vinfo, vinfo_bytes, finfo, finfo_bytes, vdata, vbytes_data,
                          fdata, fbytes_data, viewport, tex, extra, &R);
    CqDraw *r = (CqDraw *)p;
    r->lay = *lay; r->ninst = ninst; r->nv = nv;
    size_t o = r->o_verts;
    for (uint32_t b = 0; b < GPU_VLAY_MAXB; b++) {
        r->pbuf[b] = pb[b]; r->poff[b] = po[b]; r->bbytes[b] = b < lay->nbind ? bbytes[b] : 0; r->o_vb[b] = 0;
        if (b < lay->nbind && !pb[b]) { r->o_vb[b] = (uint32_t)o; memcpy(p + o, bdata[b], bbytes[b]); o += CQ_AL(bbytes[b]); }
    }
    cq_publica(p, R);
}
#endif
"""
    if ancla in s23 and v_loop in s23:
        s23 = s23.replace(ancla, ancla + renombres, 1).replace(v_loop, n_loop, 1) + final
        cambios += 1
        open(f23, 'w', encoding='utf-8').write(s23)
    else:
        print('AVISO: parche 23 no aplicado', ancla in s23, v_loop in s23)

# 24. Traza de sceGxmFinish: cuántas veces lo llama el juego, cuánto se espera y desde dónde (línea
#     "[finish]" cada 5 s, junto a "[lentos]").
f24 = os.path.join(raiz, 'host', 'gxm.c')
s24 = open(f24, encoding='utf-8').read()
v24 = "void vita_hle_sceGxmFinish(void) { gpu_sync_all_m(7); r0 = GXM_OK; }\n"
n24 = """#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (traza de sceGxmFinish) */
unsigned g_pb_finish_n; double g_pb_finish_ms, g_pb_finish_max; uint32_t g_pb_finish_lr;
void vita_hle_sceGxmFinish(void) {
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    uint32_t quien = lr;
    gpu_sync_all_m(7);
    QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    g_pb_finish_n++; g_pb_finish_ms += ms; if (ms > g_pb_finish_max) { g_pb_finish_max = ms; g_pb_finish_lr = quien; }
    r0 = GXM_OK;
}
#else
void vita_hle_sceGxmFinish(void) { gpu_sync_all_m(7); r0 = GXM_OK; }
#endif
"""
if v24 in s24 and 'traza de sceGxmFinish' not in s24:
    s24 = s24.replace(v24, n24, 1); cambios += 1
    open(f24, 'w', encoding='utf-8').write(s24)
f24b = os.path.join(raiz, 'host', 'hle_display.c')
s24b = open(f24b, encoding='utf-8').read()
v24b = """            pb_lentos = pb_muy = 0; pb_peor = 0;"""
n24b = """            { extern unsigned g_pb_finish_n; extern double g_pb_finish_ms, g_pb_finish_max; extern uint32_t g_pb_finish_lr;
              printf("[finish] %u llamadas, %.0f ms en total, la peor %.1f ms (desde 0x%08X)\\n", g_pb_finish_n, g_pb_finish_ms, g_pb_finish_max, g_pb_finish_lr);
              g_pb_finish_n = 0; g_pb_finish_ms = 0; g_pb_finish_max = 0; }
            pb_lentos = pb_muy = 0; pb_peor = 0;"""
if v24b in s24b and '[finish]' not in s24b:
    s24b = s24b.replace(v24b, n24b, 1); cambios += 1
    open(f24b, 'w', encoding='utf-8').write(s24b)

# 25. Trazador de fotogramas lentos: para cada fotograma de más de 38 ms escribe el tiempo de CPU de los
#     hilos principal, «Display Engine» y «GPU Hilo» en ese fotograma, y cuánto esperó el hilo de dibujo a
#     la cola del hilo GPU y a la GPU (línea "[lento]", como mucho 80 por partida).
f25 = os.path.join(raiz, 'host', 'hle_display.c')
s25 = open(f25, encoding='utf-8').read()
v25 = """            if (ft > 36.0) pb_lentos++;"""
n25 = """            {   /* ANDROID: parches/aplicar.py (trazador de fotogramas lentos) */
                extern double pb_cpu_hilo(int quien); extern double g_cq_espera_s; extern double g_prof_wait;
                static double c0[3], q0, w0; static unsigned nlento;
                double c[3]; for (int k = 0; k < 3; k++) c[k] = pb_cpu_hilo(k);
                if (ft > 38.0 && c0[1] > 0 && g_frames > 3300 && nlento++ < 80)
                    printf("[lento] %.1f ms: CPU principal %.1f, dibujo %.1f, hilo GPU %.1f | dibujo espera cola %.1f, GPU %.1f ms\\n", ft,
                           (c[0] - c0[0]) * 1000.0, (c[1] - c0[1]) * 1000.0, (c[2] - c0[2]) * 1000.0,
                           (g_cq_espera_s - q0) * 1000.0, (g_prof_wait - w0) * 1000.0);
                for (int k = 0; k < 3; k++) c0[k] = c[k];
                q0 = g_cq_espera_s; w0 = g_prof_wait;
            }
            if (ft > 36.0) pb_lentos++;"""
if v25 in s25 and 'trazador de fotogramas lentos' not in s25:
    s25 = s25.replace(v25, n25, 1); cambios += 1
    open(f25, 'w', encoding='utf-8').write(s25)

# 26. Hilo GPU totalmente asíncrono. Con el parche 23 el hilo de dibujo vaciaba la cola al empezar y al
#     terminar cada escena y al presentar: esperaba 3-12 ms por fotograma al hilo GPU y además hacía él
#     el envío a la GPU (gpu_scene_end ~6 %). Ahora empezar/terminar escena, profundidad y presentar
#     también van a la cola, en orden. El hilo de dibujo solo espera si va más de un fotograma por
#     delante (tope de retraso para los controles), en sceGxmFinish y cuando la CPU necesita la memoria.
#     Lo que el hilo de dibujo consulta durante la escena (superficie actual, F16, raw16) sale de una copia
#     propia. PB_GPU_ASINCRONO=0 vuelve al parche 23.
f26 = os.path.join(raiz, 'host', 'gpu_vk.c')
s26 = open(f26, encoding='utf-8').read()
if 'CQ_BEGIN' not in s26:
    reps = []
    # a) la caché de vértices no se reinicia en el hilo GPU cuando la cola es asíncrona (lo hace el de dibujo)
    reps.append(("""#ifdef __ANDROID__
    vc_inicio_escena();
#endif
""", """#ifdef __ANDROID__
    if (!pb_async_activo()) vc_inicio_escena();
#endif
"""))
    reps.append(("""#ifdef __ANDROID__
static void vc_inicio_escena(void);
#endif
""", """#ifdef __ANDROID__
static void vc_inicio_escena(void);
static int pb_async_activo(void);
#endif
"""))
    # b) tipos nuevos de orden
    reps.append(("enum { CQ_SALTO = 1, CQ_CRUDO = 2, CQ_V3K = 3 };",
                 "enum { CQ_SALTO = 1, CQ_CRUDO = 2, CQ_V3K = 3, CQ_BEGIN = 4, CQ_END = 5, CQ_DEPTH = 6, CQ_PRESENT = 7, CQ_WANTRAW = 8 };\n"
                 "typedef struct { uint32_t tipo, bytes; uint32_t a[9]; } CqSimple;\n"
                 "static volatile unsigned g_cq_pres_enc, g_cq_pres_hecho;   /* presentaciones encoladas / hechas */"))
    # c) ejecutar las órdenes nuevas en el hilo GPU
    reps.append(("""        if (r->tipo != CQ_SALTO) cq_ejecuta(r);""",
"""        if (r->tipo == CQ_CRUDO || r->tipo == CQ_V3K) cq_ejecuta(r);
        else if (r->tipo != CQ_SALTO) {
            const CqSimple *q = (const CqSimple *)r;
            switch (q->tipo) {
            case CQ_BEGIN: gpu_scene_begin_impl(q->a[0], q->a[1], q->a[2], q->a[3], (int)q->a[4], q->a[5], q->a[6], q->a[7], q->a[8]); break;
            case CQ_END: gpu_scene_end_impl(); break;
            case CQ_DEPTH: gpu_scene_depth_impl(q->a[0]); break;
            case CQ_PRESENT: gpu_present_impl(q->a[0], q->a[1], q->a[2], q->a[3]); __atomic_add_fetch(&g_cq_pres_hecho, 1, __ATOMIC_RELEASE); break;
            case CQ_WANTRAW: gpu_target_want_raw16(); break;
            }
        }"""))
    # d) funciones públicas asíncronas
    v_pub = """void gpu_scene_depth(uint32_t a) { cq_vacia(); gpu_scene_depth_impl(a); }
void gpu_scene_begin(uint32_t color_addr, uint32_t width, uint32_t height, uint32_t stride, int load_ds, uint32_t gxm_format,
                     uint32_t surf_type, uint32_t rw, uint32_t rh) {
    cq_vacia(); gpu_scene_begin_impl(color_addr, width, height, stride, load_ds, gxm_format, surf_type, rw, rh);
}
void gpu_scene_end(void) { cq_vacia(); gpu_scene_end_impl(); }"""
    n_pub = """/* Copia del estado de la superficie actual para el hilo de dibujo (el hilo GPU puede ir por detrás). */
static int g_pb_async = -1;
static int pb_async_activo(void) {
    if (g_pb_async < 0) { const char *e = getenv("PB_GPU_ASINCRONO"); g_pb_async = !(e && *e == '0'); }   /* activado (el bloqueo era gpu_module con g_cs: parche 28) */
    return g_pb_async && g_cq_on > 0 && !g_pb_soy_gpu;
}
typedef struct { uint32_t addr; uint8_t want, raw; } PbSup;
static PbSup g_pb_sup[64]; static int g_pb_nsup;
static PbSup *g_pb_cur; static uint32_t g_pb_cur_addr; static int g_pb_cur_tf = -1, g_pb_en_escena;
static void cq_simple(int tipo, const uint32_t *a, int n) {
    CqSimple *q = (CqSimple *)cq_reserva(sizeof(CqSimple));
    q->tipo = (uint32_t)tipo; q->bytes = (uint32_t)CQ_AL(sizeof(CqSimple));
    for (int i = 0; i < 9; i++) q->a[i] = i < n ? a[i] : 0;
    cq_publica((uint8_t *)q, CQ_AL(sizeof(CqSimple)));
}
void gpu_scene_depth(uint32_t a) {
    if (cq_activo() && pb_async_activo()) { cq_simple(CQ_DEPTH, &a, 1); return; }
    cq_vacia(); gpu_scene_depth_impl(a);
}
void gpu_scene_begin(uint32_t color_addr, uint32_t width, uint32_t height, uint32_t stride, int load_ds, uint32_t gxm_format,
                     uint32_t surf_type, uint32_t rw, uint32_t rh) {
    if (!(cq_activo() && pb_async_activo())) {
        cq_vacia(); gpu_scene_begin_impl(color_addr, width, height, stride, load_ds, gxm_format, surf_type, rw, rh);
        return;
    }
    if (g_vc_lleno) {   /* caché de vértices llena: vaciar con todo parado */
        cq_vacia();
        EnterCriticalSection(&g_cs); vc_inicio_escena(); LeaveCriticalSection(&g_cs);
    }
    int k; for (k = 0; k < g_pb_nsup && g_pb_sup[k].addr != color_addr; k++) {}
    if (k == g_pb_nsup) { if (g_pb_nsup < 64) g_pb_nsup++; else k = 63; g_pb_sup[k].addr = color_addr; g_pb_sup[k].want = g_pb_sup[k].raw = 0; }
    g_pb_cur = &g_pb_sup[k]; g_pb_cur_addr = color_addr; g_pb_cur_tf = tfmt_from_gxm(gxm_format); g_pb_en_escena = 1;
    if (g_pb_cur->want && g_pb_cur_tf == 2) g_pb_cur->raw = 1;
    uint32_t a[9] = { color_addr, width, height, stride, (uint32_t)load_ds, gxm_format, surf_type, rw, rh };
    cq_simple(CQ_BEGIN, a, 9);
}
void gpu_scene_end(void) {
    if (cq_activo() && pb_async_activo()) { g_pb_en_escena = 0; cq_simple(CQ_END, NULL, 0); return; }
    cq_vacia(); gpu_scene_end_impl();
}
/* Consultas del hilo de dibujo durante la escena: de la copia propia si la cola es asíncrona. */
uint32_t pb_target_addr(void) { return g_pb_en_escena ? g_pb_cur_addr : 0; }
int pb_target_raw16(void) { return g_pb_en_escena && g_pb_cur && g_pb_cur->raw; }
int pb_target_f16(void) { return g_pb_en_escena && g_pb_cur_tf == 2; }
void pb_target_want_raw16(void) {
    if (g_pb_en_escena && g_pb_cur && g_pb_cur_tf == 2) g_pb_cur->want = 1;
    cq_simple(CQ_WANTRAW, NULL, 0);
}"""
    reps.append((v_pub, n_pub))
    v_pres = "void gpu_present(uint32_t addr, uint32_t width, uint32_t height, uint32_t stride) { cq_vacia(); gpu_present_impl(addr, width, height, stride); }"
    n_pres = """void gpu_present(uint32_t addr, uint32_t width, uint32_t height, uint32_t stride) {
    if (!(cq_activo() && pb_async_activo())) { cq_vacia(); gpu_present_impl(addr, width, height, stride); return; }
    /* como mucho un fotograma por delante del hilo GPU (retraso de los controles) */
    while (g_cq_pres_enc - __atomic_load_n(&g_cq_pres_hecho, __ATOMIC_ACQUIRE) >= 1) {
        pthread_mutex_lock(&g_cq_mx);
        if (g_cq_head != g_cq_tail) { g_cq_p_espera = 1; g_cq_esperas++; pthread_cond_wait(&g_cq_cp, &g_cq_mx); g_cq_p_espera = 0; }
        pthread_mutex_unlock(&g_cq_mx);
        if (g_cq_head == g_cq_tail) break;
    }
    uint32_t a[4] = { addr, width, height, stride };
    g_cq_pres_enc++;
    cq_simple(CQ_PRESENT, a, 4);
}"""
    reps.append((v_pres, n_pres))
    # e) las consultas públicas usan la copia del hilo de dibujo cuando la cola es asíncrona
    reps.append(("int gpu_target_raw16(void) { return g_cur && g_cur->raw16; }",
                 "int gpu_target_raw16(void) {\n#ifdef __ANDROID__\n    extern int pb_target_raw16(void); if (pb_async_activo() && !pb_en_hilo_gpu()) return pb_target_raw16();\n#endif\n    return g_cur && g_cur->raw16;\n}"))
    reps.append(("int gpu_target_f16(void) { return g_cur && g_cur->tf == 2; }",
                 "int gpu_target_f16(void) {\n#ifdef __ANDROID__\n    extern int pb_target_f16(void); if (pb_async_activo() && !pb_en_hilo_gpu()) return pb_target_f16();\n#endif\n    return g_cur && g_cur->tf == 2;\n}"))
    reps.append(("void gpu_target_want_raw16(void) { if (g_cur && g_cur->tf == 2) g_cur->want_raw16 = 1; }",
                 "void gpu_target_want_raw16(void) {\n#ifdef __ANDROID__\n    extern void pb_target_want_raw16(void); if (pb_async_activo() && !pb_en_hilo_gpu()) { pb_target_want_raw16(); return; }\n#endif\n    if (g_cur && g_cur->tf == 2) g_cur->want_raw16 = 1;\n}"))
    reps.append(("uint32_t gpu_target_addr(void) { return g_cur ? g_cur->addr : 0; }",
                 "uint32_t gpu_target_addr(void) {\n#ifdef __ANDROID__\n    extern uint32_t pb_target_addr(void); if (pb_async_activo() && !pb_en_hilo_gpu()) return pb_target_addr();\n#endif\n    return g_cur ? g_cur->addr : 0;\n}"))
    # f) saber si estamos en el hilo GPU (las consultas internas del hilo GPU usan g_cur)
    reps.append(("""static int g_pre_activo;""", """static int g_pre_activo;
static __thread int g_pb_soy_gpu;
static int pb_en_hilo_gpu(void) { return g_pb_soy_gpu; }"""))
    reps.append(("""    pthread_setname_np(pthread_self(), "GPU Hilo");""", """    pthread_setname_np(pthread_self(), "GPU Hilo");
    g_pb_soy_gpu = 1;"""))
    falta = [v[:60] for v, n in reps if v not in s26]
    if falta:
        print('AVISO: parche 26 no aplicado, faltan:', falta)
    else:
        for v, n in reps: s26 = s26.replace(v, n, 1)
        cambios += 1
        open(f26, 'w', encoding='utf-8').write(s26)

# 27. Resolución interna solo en la imagen principal. Con x2/x3 se agrandaban TODAS las superficies
#     (mapa de sombras 2048x1024 -> 4096x2048, máscara de sombras de jugadores, texturas que pinta el
#     juego...): a x2 la GPU pasaba de 231 a 374 MHz y el teléfono se calentaba mucho. Ahora en Android
#     solo los búferes de pantalla (donde se pinta la escena que se ve) usan la resolución elegida; el
#     resto va a x1. PB_RES_TODAS=1 vuelve a escalarlas todas.
f27 = os.path.join(raiz, 'host', 'gpu_vk.c')
s27 = open(f27, encoding='utf-8').read()
if 'pb_res_para' not in s27:
    reps = [
        ("    uint8_t pb_gpu, pb_cpu;", "    uint8_t pb_gpu, pb_cpu, pb_res;"),
        ("        t->sw = w * g_res; t->sh = h * g_res;",
         "        { uint32_t rf = pb_res_para(addr); t->pb_res = (uint8_t)rf; t->sw = w * rf; t->sh = h * rf; }   /* ANDROID: parches/aplicar.py */"),
        ("                       &t->img, &t->mem, &t->view)) { t->img = VK_NULL_HANDLE; return NULL; }\n        if (g_res > 1) {",
         "                       &t->img, &t->mem, &t->view)) { t->img = VK_NULL_HANDLE; return NULL; }\n        if (t->pb_res > 1) {"),
        ("    VkRect2D sc = { { 0, 0 }, { t->rw * g_res, t->rh * g_res } };",
         "    VkRect2D sc = { { 0, 0 }, { t->rw * t->pb_res, t->rh * t->pb_res } };"),
        ("d->dbias_factor * (float)g_res : d->dbias_factor);",
         "d->dbias_factor * (float)(g_cur ? g_cur->pb_res : g_res) : d->dbias_factor);"),
        ("    const float rs = (float)g_res;", "    const float rs = (float)(g_cur ? g_cur->pb_res : g_res);"),
        ("uint32_t gpu_res_scale(void) { return g_res; }",
         """uint32_t gpu_res_scale(void) { return g_cur && g_cur->pb_res ? g_cur->pb_res : g_res; }
/* ANDROID: resolución de una superficie nueva: la elegida solo en los búferes de pantalla. */
static uint32_t pb_res_para(uint32_t addr) {
    static int todas = -1;
    if (todas < 0) { const char *e = getenv("PB_RES_TODAS"); todas = e && *e == '1'; }
    extern int port_display_is_framebuf(uint32_t addr);
    return (todas || port_display_is_framebuf(addr)) ? g_res : 1u;
}"""),
    ]
    falta = [v[:50] for v, n in reps if v not in s27]
    if falta: print('AVISO: parche 27 no aplicado', falta)
    else:
        for v, n in reps: s27 = s27.replace(v, n, 1)
        cambios += 1
        open(f27, 'w', encoding='utf-8').write(s27)

# 28. gpu_module con su propio candado. gpu_scene_begin toma g_cs y lo suelta gpu_scene_end (la escena
#     entera); con el hilo GPU asíncrono quien empieza la escena es el hilo GPU, y si el hilo de dibujo
#     creaba un shader a mitad de escena (gpu_module -> g_cs) se bloqueaban los dos (el juego no
#     arrancaba). vkCreateShaderModule se puede llamar desde cualquier hilo: basta un candado propio
#     para g_nmods.
f28 = os.path.join(raiz, 'host', 'gpu_vk.c')
s28 = open(f28, encoding='utf-8').read()
v28 = """    EnterCriticalSection(&g_cs);
    int id = 0;
    if (vkCreateShaderModule(g_dev, &smi, NULL, &g_mods[g_nmods]) == VK_SUCCESS) id = (int)g_nmods++;
    LeaveCriticalSection(&g_cs);"""
n28 = """#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (gpu_module con su propio candado) */
    static pthread_mutex_t mx_mods = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mx_mods);
    int id = 0;
    if (g_nmods < MAX_MODS && vkCreateShaderModule(g_dev, &smi, NULL, &g_mods[g_nmods]) == VK_SUCCESS) id = (int)g_nmods++;
    pthread_mutex_unlock(&mx_mods);
#else
    EnterCriticalSection(&g_cs);
    int id = 0;
    if (vkCreateShaderModule(g_dev, &smi, NULL, &g_mods[g_nmods]) == VK_SUCCESS) id = (int)g_nmods++;
    LeaveCriticalSection(&g_cs);
#endif"""
if v28 in s28 and 'propio candado' not in s28:
    s28 = s28.replace(v28, n28, 1); cambios += 1
    if '#include <pthread.h>' not in s28.split('static int g_pre_activo;')[0]:
        s28 = s28.replace('#include <windows.h>\n', '#include <windows.h>\n#include <pthread.h>\n', 1)
    open(f28, 'w', encoding='utf-8').write(s28)
# 29. Profundidad/stencil sin guardar al acabar la escena (2026-11-09). Todas las pasadas guardaban la
#     profundidad; casi ninguna superficie la vuelve a usar (solo ForceLoad y los mapas de sombras, que
#     se muestrean). Una superficie cuya profundidad nadie ha cargado ni muestreado usa una pasada con
#     storeOp DONT_CARE. PB_DS_GUARDAR=1 vuelve atrás. Ganancia medida pequeña (GPU 76 -> 74 %).
# 30. Filtro anisotrópico 4x en vez de 16x (PB_ANISO=n; 1 = sin). A x2: trabajo de GPU -17 %.
# 31. Pase final: ampliando menos de 1,5x se usa bilineal en vez del bicúbico de 9 lecturas
#     (PB_BICUBICO=1 lo deja). Sin ganancia medible; se deja porque no cambia la imagen a x2.
f29 = os.path.join(raiz, 'host', 'gpu_vk.c')
s29 = open(f29, encoding='utf-8').read()
if 'g_passf_nd' not in s29:
    pares29 = [
    ("static VkRenderPass g_pass_raw, g_pass_raw_load;",
     "static VkRenderPass g_pass_raw, g_pass_raw_load;\nstatic VkRenderPass g_passf_nd[NTFMT], g_pass_raw_nd;   /* ANDROID p29: profundidad/stencil sin guardar al acabar */"),
    ("""    atts[0].format = g_tfmt_vk[0];
    g_pass = g_passf[0];
""", """    atts[0].format = g_tfmt_vk[0];
    g_pass = g_passf[0];
    /* ANDROID p29: la misma pasada sin guardar profundidad/stencil al terminar (en una GPU por
     * mosaicos guardarla cuesta escribir toda la superficie; casi ninguna escena la vuelve a usar). */
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    for (int i = 0; i < NTFMT; i++) {
        atts[0].format = g_tfmt_vk[i];
        CHECK(vkCreateRenderPass(g_dev, &rpi, NULL, &g_passf_nd[i]));
    }
    atts[0].format = VK_FORMAT_R16G16B16A16_UINT;
    CHECK(vkCreateRenderPass(g_dev, &rpi, NULL, &g_pass_raw_nd));
    atts[0].format = g_tfmt_vk[0];
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
"""),
    ("""    if (t->raw16) rp.renderPass = (load_ds && t->ds_ready) ? g_pass_raw_load : g_pass_raw;
    else rp.renderPass = (load_ds && t->ds_ready) ? g_passf_load[tf] : g_passf[tf];
    t->ds_ready = 1;
""", """    {   /* ANDROID p29: PB_DS_GUARDAR=1 guarda siempre la profundidad (como antes) */
        static int guardar = -1;
        if (guardar < 0) { const char *e = getenv("PB_DS_GUARDAR"); guardar = e && *e == '1'; }
        if (load_ds) t->pb_ds_uso = 1;
        int nd = !guardar && !t->pb_ds_uso && !t->dtx;        /* nadie la ha cargado ni muestreado nunca */
        int carga = load_ds && t->ds_ready && t->pb_ds_ok;    /* solo se carga si la última escena la guardó */
        if (t->raw16) rp.renderPass = carga ? g_pass_raw_load : nd ? g_pass_raw_nd : g_pass_raw;
        else rp.renderPass = carga ? g_passf_load[tf] : nd ? g_passf_nd[tf] : g_passf[tf];
        t->pb_ds_ok = !nd;
    }
    t->ds_ready = 1;
"""),
    ("uint8_t pb_gpu, pb_cpu, pb_res; ", "uint8_t pb_gpu, pb_cpu, pb_res, pb_ds_uso, pb_ds_ok; "),
    ("""        g_aniso = props.limits.maxSamplerAnisotropy < 16.0f ? props.limits.maxSamplerAnisotropy : 16.0f;
    }""", """        g_aniso = props.limits.maxSamplerAnisotropy < 16.0f ? props.limits.maxSamplerAnisotropy : 16.0f;
#ifdef __ANDROID__   /* p30: filtro anisotrópico moderado en el móvil (16x cuesta mucha GPU). PB_ANISO=n lo cambia; 1 = sin */
        { const char *e = getenv("PB_ANISO"); float a = e ? (float)atof(e) : 4.0f; if (a < 1.0f) a = 1.0f; if (a < g_aniso) g_aniso = a; }
#endif
    }"""),
    ("""    float pc[8] = { 1.0f / sw, 1.0f / sh, sw, sh, g_pp_fxaa, g_pp_sharp, (float)g_pp_out.w / sw, 0 };""",
     """    float pc[8] = { 1.0f / sw, 1.0f / sh, sw, sh, g_pp_fxaa, g_pp_sharp, (float)g_pp_out.w / sw, 0 };
#ifdef __ANDROID__   /* p31: ampliando menos de 1,5x (resolución x2) el bicúbico de 9 lecturas no se nota: bilineal. PB_BICUBICO=1 lo deja */
    { static int bic = -1; if (bic < 0) { const char *e = getenv("PB_BICUBICO"); bic = e && *e == '1'; }
      if (!bic && pc[6] > 1.0f && pc[6] < 1.5f) pc[6] = 1.0f; }
#endif"""),
    ]
    if all(a in s29 for a, b in pares29):
        for a, b in pares29: s29 = s29.replace(a, b, 1)
        cambios += 1
        open(f29, 'w', encoding='utf-8').write(s29)
    else:
        print('AVISO: parches 29-31 no aplicados (el fuente ha cambiado)')
# 32. Palancas analógicas del mando en pantalla (2026-11-10): MandoEnPantalla.java manda la posición
#     exacta (nativoStick, stubs_android.c) y input.c la usa en vez de las teclas mientras se toca.
f32 = os.path.join(raiz, 'host', 'input.c')
s32 = open(f32, encoding='utf-8').read()
v32 = '    rx = keys_axis(k, 6, 7, rx); ry = keys_axis(k, 4, 5, ry);\n'
n32 = '    rx = keys_axis(k, 6, 7, rx); ry = keys_axis(k, 4, 5, ry);\n#ifdef __ANDROID__   /* ANDROID: parches/aplicar.py (palancas analógicas del mando en pantalla) */\n    { extern volatile int pb_tactil_eje[4];\n      int ex = pb_tactil_eje[0], ey = pb_tactil_eje[1]; if (ex >= 0 && ey >= 0) { lx = (uint8_t)ex; ly = (uint8_t)ey; }\n      ex = pb_tactil_eje[2]; ey = pb_tactil_eje[3];     if (ex >= 0 && ey >= 0) { rx = (uint8_t)ex; ry = (uint8_t)ey; } }\n#endif\n'
if v32 in s32 and 'pb_tactil_eje' not in s32:
    open(f32, 'w', encoding='utf-8').write(s32.replace(v32, n32, 1)); cambios += 1
# 33. Ampliar sin estirar y toque bien medido (2026-11-10). [video] zoom (100-125 %, PB_ZOOM): la imagen
#     crece igual a lo ancho y a lo alto y se recorta lo que no cabe. Además el toque se traduce sobre el
#     rectángulo real de la imagen (antes sobre toda la pantalla: desviado por las franjas laterales).
pares33g = [('    uint32_t ww = g_swap_ext.width, wh = g_swap_ext.height, dw = ww, dh = ww * h / w;\n    if (dh > wh) { dh = wh; dw = wh * w / h; }\n    int32_t ox = (int32_t)(ww - dw) / 2, oy = (int32_t)(wh - dh) / 2;\n', '    uint32_t ww = g_swap_ext.width, wh = g_swap_ext.height, dw = ww, dh = ww * h / w;\n    if (dh > wh) { dh = wh; dw = wh * w / h; }\n#ifdef __ANDROID__   /* ANDROID p33: ampliar sin estirar ([video] zoom, 100-125 %): la imagen crece igual a lo ancho y a lo\n                      * alto y lo que no cabe se recorta por arriba y por abajo (menos franja negra a los lados) */\n    { static int zoom = -1;\n      if (zoom < 0) { extern int launcher_ini_int(const char *, const char *, int); const char *e = getenv("PB_ZOOM");\n                      zoom = e ? atoi(e) : launcher_ini_int("video", "zoom", 100); if (zoom < 100) zoom = 100; if (zoom > 125) zoom = 125; }\n      dw = dw * (uint32_t)zoom / 100u; dh = dh * (uint32_t)zoom / 100u; }\n#endif\n    uint32_t vw = dw < ww ? dw : ww, vh = dh < wh ? dh : wh;         /* parte de la imagen que cabe en la ventana */\n    int32_t rx = (int32_t)(dw - vw) / 2, ry = (int32_t)(dh - vh) / 2;   /* recorte (en píxeles de destino) */\n    int32_t ox = (int32_t)(ww - vw) / 2, oy = (int32_t)(wh - vh) / 2;\n    g_pres_rect[0] = ox - rx; g_pres_rect[1] = oy - ry; g_pres_rect[2] = (int32_t)dw; g_pres_rect[3] = (int32_t)dh;\n'), ('    bl.srcOffsets[1].x = (int32_t)(post ? dw : pw); bl.srcOffsets[1].y = (int32_t)(post ? dh : ph); bl.srcOffsets[1].z = 1;\n    bl.dstOffsets[0].x = ox; bl.dstOffsets[0].y = oy;\n    bl.dstOffsets[1].x = ox + (int32_t)dw; bl.dstOffsets[1].y = oy + (int32_t)dh; bl.dstOffsets[1].z = 1;\n', '    if (post) {\n        bl.srcOffsets[0].x = rx; bl.srcOffsets[0].y = ry;\n        bl.srcOffsets[1].x = rx + (int32_t)vw; bl.srcOffsets[1].y = ry + (int32_t)vh;\n    } else {   /* sin pase final el origen va a su tamaño: el mismo recorte en sus píxeles */\n        bl.srcOffsets[0].x = (int32_t)((uint64_t)rx * pw / dw); bl.srcOffsets[0].y = (int32_t)((uint64_t)ry * ph / dh);\n        bl.srcOffsets[1].x = (int32_t)((uint64_t)(rx + (int32_t)vw) * pw / dw); bl.srcOffsets[1].y = (int32_t)((uint64_t)(ry + (int32_t)vh) * ph / dh);\n    }\n    bl.srcOffsets[1].z = 1;\n    bl.dstOffsets[0].x = ox; bl.dstOffsets[0].y = oy;\n    bl.dstOffsets[1].x = ox + (int32_t)vw; bl.dstOffsets[1].y = oy + (int32_t)vh; bl.dstOffsets[1].z = 1;\n'), ('static VkRenderPass g_pass_raw, g_pass_raw_load;\n', 'static VkRenderPass g_pass_raw, g_pass_raw_load;\n/* p33: dónde queda la imagen del juego dentro de la ventana (x, y, ancho, alto; puede salirse si hay zoom) */\nstatic volatile int32_t g_pres_rect[4];\nint gpu_ventana_a_panel(int mx, int my, int *px, int *py) {   /* punto de la ventana -> panel táctil de la Vita (1920x1088) */\n    int32_t x0 = g_pres_rect[0], y0 = g_pres_rect[1], w = g_pres_rect[2], h = g_pres_rect[3];\n    if (w <= 0 || h <= 0) return 0;\n    int x = (int)((int64_t)(mx - x0) * 1920 / w), y = (int)((int64_t)(my - y0) * 1088 / h);\n    *px = x < 0 ? 0 : x > 1919 ? 1919 : x; *py = y < 0 ? 0 : y > 1087 ? 1087 : y;\n    return 1;\n}\n')]
pares33i = [('        g_mouse_x = mx * 1920 / ww;\n        g_mouse_y = my * 1088 / wh;\n', '        g_mouse_x = mx * 1920 / ww;\n        g_mouse_y = my * 1088 / wh;\n#ifdef __ANDROID__   /* ANDROID p33: el toque se mide sobre la imagen del juego, no sobre toda la pantalla (franjas, zoom) */\n        { extern int gpu_ventana_a_panel(int mx, int my, int *px, int *py); int tx, ty;\n          if (gpu_ventana_a_panel(mx, my, &tx, &ty)) { g_mouse_x = tx; g_mouse_y = ty; } }\n#endif\n')]
for f33, pares33, marca33 in ((os.path.join(raiz, 'host', 'gpu_vk.c'), pares33g, 'g_pres_rect'), (os.path.join(raiz, 'host', 'input.c'), pares33i, 'gpu_ventana_a_panel')):
    s33 = open(f33, encoding='utf-8').read()
    if marca33 not in s33 and all(a33 in s33 for a33, b33 in pares33):
        for a33, b33 in pares33: s33 = s33.replace(a33, b33, 1)
        open(f33, 'w', encoding='utf-8').write(s33); cambios += 1
    elif marca33 not in s33: print('AVISO: parche 33 no aplicado en', f33)
# 34. Traza de palancas (PB_PALANCA_TRAZA=1): cada cambio de las palancas que ve el juego, en log.txt.
f34 = os.path.join(raiz, 'host', 'input.c')
s34 = open(f34, encoding='utf-8').read()
v34 = '      ex = pb_tactil_eje[2]; ey = pb_tactil_eje[3];     if (ex >= 0 && ey >= 0) { rx = (uint8_t)ex; ry = (uint8_t)ey; } }\n#endif\n'
n34 = '      ex = pb_tactil_eje[2]; ey = pb_tactil_eje[3];     if (ex >= 0 && ey >= 0) { rx = (uint8_t)ex; ry = (uint8_t)ey; }\n      /* PB_PALANCA_TRAZA=1: escribe cada cambio de las palancas que ve el juego (p34) */\n      static int tr = -1; static uint32_t ult; if (tr < 0) tr = getenv("PB_PALANCA_TRAZA") ? 1 : 0;\n      uint32_t ahora = (uint32_t)lx | ((uint32_t)ly << 8) | ((uint32_t)rx << 16) | ((uint32_t)ry << 24);\n      if (tr && ahora != ult) { printf("[palanca] izq %u,%u der %u,%u (tactil %d)\\n", lx, ly, rx, ry, pb_tactil_eje[0]); fflush(stdout); ult = ahora; } }\n#endif\n'
if v34 in s34 and 'PB_PALANCA_TRAZA' not in s34:
    open(f34, 'w', encoding='utf-8').write(s34.replace(v34, n34, 1)); cambios += 1
# 35. Cubemaps de luz negros (2026-11-13): el parche 19 devuelve la textura del fotograma anterior sin pasar
#     por pend_overlap, que era quien marcaba la superficie como «la lee la CPU» (pb_cpu). La superficie de
#     los cubemaps (64x512) quedaba entonces como «solo GPU» (parche 20) y no volvía a la memoria del juego:
#     la luz ambiente de jugadores y estadio era 0 y todo lo que no da el sol salía negro (caras en RTTS,
#     estadio de lejos en los cambios de entrada). Ahora ese camino también marca pb_cpu.
f35 = os.path.join(raiz, 'host', 'gpu_vk.c')
s35 = open(f35, encoding='utf-8').read()
v35 = 'if (g && !g->resident && a < g->addr + g->gstride * g->h * g_tfmt_bpp[g->tf] && g->addr < a + n) { x->scene = g_stat_scenes; return x; }'
n35 = 'if (g && !g->resident && a < g->addr + g->gstride * g->h * g_tfmt_bpp[g->tf] && g->addr < a + n) { g->pb_cpu = 1; x->scene = g_stat_scenes; return x; }   /* p35: la lee la CPU */'
if v35 in s35:
    open(f35, 'w', encoding='utf-8').write(s35.replace(v35, n35, 1)); cambios += 1
elif n35 not in s35: print('AVISO: parche 35 no aplicado')
# 36. «Solo GPU» únicamente para superficies que se redibujan en cada fotograma (2026-11-13). El parche 20
#     dejaba de copiar a la memoria del juego cualquier superficie servida como textura desde la GPU. Pero
#     hay superficies que se componen pocas veces y luego el juego lee de su memoria por otro camino: la cara
#     del jugador creado (RTTS) salía negra. Ahora hace falta además que la superficie se haya dibujado en
#     60 fotogramas seguidos (sombras de jugadores, etc.); las que se dibujan de vez en cuando vuelven a la
#     memoria como en el PC.
f36 = os.path.join(raiz, 'host', 'gpu_vk.c')
s36 = open(f36, encoding='utf-8').read()
if 'pb_fot_seguidos' not in s36:
    a1 = '} Target;\n'
    b1 = """#ifdef __ANDROID__
    uint32_t pb_ufr; uint16_t pb_nfr;                         /* ANDROID p36: último fotograma dibujado / fotogramas seguidos */
#endif
} Target;
#ifdef __ANDROID__
/* ANDROID p36: true si la superficie lleva 60 fotogramas seguidos dibujándose (se llama al acabar sus escenas) */
static int pb_fot_seguidos(Target *t) {
    extern unsigned port_display_frames(void);
    unsigned f = port_display_frames();
    if (f != t->pb_ufr) { t->pb_nfr = (f == t->pb_ufr + 1u) ? (uint16_t)(t->pb_nfr < 60000 ? t->pb_nfr + 1 : t->pb_nfr) : 0; t->pb_ufr = f; }
    return t->pb_nfr >= 60;
}
#endif
"""
    a2 = '(t)->w * (t)->h >= 16384u && !getenv("PB_FB_MEMORIA")'
    b2 = '(t)->w * (t)->h >= 16384u && pb_fot_seguidos(t) && !getenv("PB_FB_MEMORIA")'
    if s36.count(a1) == 1 and a2 in s36:
        s36 = s36.replace(a1, b1, 1).replace(a2, b2, 1)
        open(f36, 'w', encoding='utf-8').write(s36); cambios += 1
    else: print('AVISO: parche 36 no aplicado')
# 37. Volver desde otra app (2026-11-13): al pasar a segundo plano Android destruye la ventana y el juego
#     seguía presentando en la superficie vieja («BufferQueue has been abandoned»): se oía pero la pantalla
#     quedaba negra. Ahora en segundo plano no se presenta y al volver (o si la superficie se pierde) se
#     crean una superficie y una swapchain nuevas; si la ventana nueva aún no está lista se reintenta cada
#     250 ms. arranque_android.c avisa con gpu_android_ciclo().
f37 = os.path.join(raiz, 'host', 'gpu_vk.c')
s37 = open(f37, encoding='utf-8').read()
if 'gpu_android_ciclo' not in s37:
    a1 = 'int gpu_has_window(void) { return g_ok && g_win != NULL; }\n'
    b1 = a1 + """#ifdef __ANDROID__   /* ANDROID p37: superficie nueva al volver desde otra app */
static volatile int g_pb_fondo;          /* 0 normal, 1 en segundo plano, 2 hay que crear superficie nueva */
static volatile uint32_t g_pb_fondo_t;   /* SDL_GetTicks del aviso / del último intento */
void gpu_android_ciclo(int estado) { g_pb_fondo_t = SDL_GetTicks(); g_pb_fondo = estado; }
static int pb_superficie_nueva(void) {
    uint32_t ahora = SDL_GetTicks();
    if (ahora - g_pb_fondo_t < 250u) return 0;        /* da tiempo a que Android entregue la ventana nueva */
    g_pb_fondo_t = ahora;
    vkDeviceWaitIdle(g_dev);
    if (g_swap) { vkDestroySwapchainKHR(g_dev, g_swap, NULL); g_swap = VK_NULL_HANDLE; }
    if (g_surface) { vkDestroySurfaceKHR(g_inst, g_surface, NULL); g_surface = VK_NULL_HANDLE; }
    if (!SDL_Vulkan_CreateSurface(g_win, g_inst, &g_surface)) {
        printf("[gpu] superficie nueva: SDL_Vulkan_CreateSurface falló (%s), se reintenta\\n", SDL_GetError()); fflush(stdout);
        g_surface = VK_NULL_HANDLE; return 0;
    }
    int ok = swapchain_create();
    printf("[gpu] superficie nueva al volver a la app: %s\\n", ok ? "lista" : "falló, se reintenta"); fflush(stdout);
    if (ok) g_pb_fondo = 0;
    return ok;
}
#endif
"""
    a2 = '    pump_events();\n    if (!g_swap && !swapchain_create()) { LeaveCriticalSection(&g_cs); return; }\n'
    b2 = """    pump_events();
#ifdef __ANDROID__   /* ANDROID p37: en segundo plano no se presenta; al volver, superficie nueva */
    if (g_pb_fondo == 1) { LeaveCriticalSection(&g_cs); return; }
    if (g_pb_fondo == 2 && !pb_superficie_nueva()) { LeaveCriticalSection(&g_cs); return; }
    if (!g_surface) { LeaveCriticalSection(&g_cs); return; }
#endif
    if (!g_swap && !swapchain_create()) { LeaveCriticalSection(&g_cs); return; }
"""
    a3 = '    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, g_sem_acq, VK_NULL_HANDLE, &idx);\n'
    b3 = a3 + """#ifdef __ANDROID__   /* ANDROID p37: superficie perdida (ventana destruida): se crea otra */
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_TIMEOUT && r != VK_NOT_READY) {
        static uint32_t nerr; if (nerr++ % 60 == 0) { printf("[gpu] adquirir imagen: %d, superficie nueva\\n", (int)r); fflush(stdout); }
        if (g_pb_fondo != 1) { g_pb_fondo_t = SDL_GetTicks(); g_pb_fondo = 2; }
        LeaveCriticalSection(&g_cs); return;
    }
#endif
"""
    if all(a in s37 for a in (a1, a2, a3)):
        s37 = s37.replace(a1, b1, 1).replace(a2, b2, 1).replace(a3, b3, 1)
        open(f37, 'w', encoding='utf-8').write(s37); cambios += 1
    else: print('AVISO: parche 37 no aplicado', [a[:40] for a in (a1, a2, a3) if a not in s37])
s37 = open(f37, encoding='utf-8').read()
a0 = 'X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)\n'
if 'X(vkDestroySurfaceKHR)' not in s37 and a0 in s37:   # p37: vkDestroySurfaceKHR se carga como las demás
    open(f37, 'w', encoding='utf-8').write(s37.replace(a0, a0[:-1] + ' X(vkDestroySurfaceKHR)\n', 1)); cambios += 1
# 38. Videomarcador del estadio a la resolución elegida (2026-11-13). Con el parche 27 el videomarcador (ficha del
#     jugador) salía muy borroso a x2. Buscado por descarte con PB_RES_TAMANOS y capturas: es la superficie que el
#     juego declara de 16x16 (pinta más grande de lo que declara). Las superficies con los tamaños de PB_RES_TAMANOS
#     (por defecto "16x16") siguen la resolución elegida; PB_TRAZA_SUP=1 lista cada superficie creada.
f38 = os.path.join(raiz, 'host', 'gpu_vk.c')
s38 = open(f38, encoding='utf-8').read()
if 'pb_res_tamano' not in s38:
    a1 = '{ uint32_t rf = pb_res_para(addr); t->pb_res'
    b1 = '{ uint32_t rf = pb_res_tamano(w, h) ? g_res : pb_res_para(addr); if (getenv("PB_TRAZA_SUP")) { printf("[sup] 0x%08X %ux%u tf %d res %u\\n", addr, w, h, tf, rf); fflush(stdout); } t->pb_res'
    a2 = 'static uint32_t pb_res_para(uint32_t addr) {'
    b2 = """/* ANDROID p38: superficies que siguen la resolución elegida aunque no sean pantalla (videomarcador) */
static int pb_res_tamano(uint32_t w, uint32_t h) {
    static uint32_t tw[8], th[8]; static int n = -1;
    if (n < 0) {
        const char *e = getenv("PB_RES_TAMANOS"); if (!e) e = "16x16";
        n = 0;
        while (*e && n < 8) { unsigned a = 0, b = 0; if (sscanf(e, "%ux%u", &a, &b) == 2) { tw[n] = a; th[n] = b; n++; }
                              e = strchr(e, ','); if (!e) break; e++; }
    }
    for (int i = 0; i < n; i++) if (tw[i] == w && th[i] == h) return 1;
    return 0;
}
""" + a2
    if a1 in s38 and a2 in s38:
        s38 = s38.replace(a1, b1, 1).replace(a2, b2, 1)
        open(f38, 'w', encoding='utf-8').write(s38); cambios += 1
    else: print('AVISO: parche 38 no aplicado')
# 39. Resolución x1,5 (2026-11-13): la escala de cada superficie se guarda en mitades (pb_res2: 2 = x1, 3 = x1,5,
#     4 = x2) y se usa como decimal en el viewport, el recorte, el sesgo de profundidad y res_multiplier de los
#     shaders. [video] media=1 (o PB_RES15=1) pide x1,5; la pantalla de opciones lo ofrece entre x1 y x2.
f39 = os.path.join(raiz, 'host', 'gpu_vk.c')
s39 = open(f39, encoding='utf-8').read()
if 'g_res2' not in s39:
    pares39 = [
        ('static uint32_t g_res = 1;\n', 'static uint32_t g_res = 1;\nstatic uint32_t g_res2 = 2;   /* ANDROID p39: escala de la pantalla en mitades (3 = x1,5) */\n'),
        ('uint8_t pb_gpu, pb_cpu, pb_res, pb_ds_uso, pb_ds_ok;', 'uint8_t pb_gpu, pb_cpu, pb_res, pb_ds_uso, pb_ds_ok, pb_res2;'),
        ('t->pb_res = (uint8_t)rf; t->sw = w * rf; t->sh = h * rf; }',
         '{ uint32_t r2 = rf > 1 ? g_res2 : 2u; t->pb_res2 = (uint8_t)r2; t->pb_res = (uint8_t)((r2 + 1) / 2); t->sw = (w * r2 + 1) / 2; t->sh = (h * r2 + 1) / 2; } }'),
        ('        if (t->pb_res > 1) {\n            if (!image_new_fmt(w, h,', '        if (t->pb_res2 > 2) {\n            if (!image_new_fmt(w, h,'),
        ('(float)(g_cur ? g_cur->pb_res : g_res) : d->dbias_factor);', '(g_cur ? g_cur->pb_res2 * 0.5f : g_res2 * 0.5f) : d->dbias_factor);'),
        ('{ t->rw * t->pb_res, t->rh * t->pb_res }', '{ (t->rw * t->pb_res2 + 1) / 2, (t->rh * t->pb_res2 + 1) / 2 }'),
        ('const float rs = (float)(g_cur ? g_cur->pb_res : g_res);', 'const float rs = g_cur ? g_cur->pb_res2 * 0.5f : g_res2 * 0.5f;'),
        ('uint32_t gpu_res_scale(void) { return g_cur && g_cur->pb_res ? g_cur->pb_res : g_res; }\n',
         'uint32_t gpu_res_scale(void) { return g_cur && g_cur->pb_res ? g_cur->pb_res : g_res; }\n'
         'float gpu_res_scalef(void) { return g_cur && g_cur->pb_res2 ? g_cur->pb_res2 * 0.5f : g_res2 * 0.5f; }   /* ANDROID p39 */\n'),
        ('        g_res = r < 1 ? 1u : r > 4 ? 4u : (uint32_t)r;\n',
         '        g_res = r < 1 ? 1u : r > 4 ? 4u : (uint32_t)r;\n'
         '        g_res2 = g_res * 2u;   /* ANDROID p39: x1,5 con [video] media=1 o PB_RES15=1 */\n'
         '        { const char *m = getenv("PB_RES15"); int med = m ? atoi(m) : launcher_ini_int("video", "media", 0);\n'
         '          if (med && g_res <= 2) { g_res = 2u; g_res2 = 3u; printf("[gpu] resolución interna x1,5\\n"); } }\n'),
    ]
    falta = [a[:50] for a, b in pares39 if a not in s39]
    if falta: print('AVISO: parche 39 no aplicado', falta)
    else:
        for a, b in pares39: s39 = s39.replace(a, b, 1)
        open(f39, 'w', encoding='utf-8').write(s39); cambios += 1
for f39b, a39b, b39b in ((os.path.join(raiz, 'host', 'gxm.c'), '(float)gpu_res_scale() };', 'gpu_res_scalef() };   /* ANDROID p39 */'),
                         (os.path.join(raiz, 'host', 'gpu.h'), 'uint32_t gpu_res_scale(void);', 'uint32_t gpu_res_scale(void);\nfloat gpu_res_scalef(void);   /* ANDROID p39: escala decimal (x1,5) */')):
    s39b = open(f39b, encoding='utf-8').read()
    if 'gpu_res_scalef' not in s39b:
        if a39b in s39b: open(f39b, 'w', encoding='utf-8').write(s39b.replace(a39b, b39b, 1)); cambios += 1
        else: print('AVISO: parche 39 no aplicado en', f39b)
# 39b. El log y arranque.txt dicen x1,5 (antes salía «x1,5» y luego «x2», el entero de apoyo).
s39c = open(f39, encoding='utf-8').read()
a39c = ('printf("[gpu] resolución interna x1,5\\n"); } }\n        printf("[gpu] resolución interna x%u\\n", g_res);\n'
        '        { extern void arranque(const char *fmt, ...); arranque("gpu: tarjeta elegida %s; resolución interna x%u", props.deviceName, g_res); }\n')
b39c = (' } }\n        { const char *rt = g_res2 == 3u ? "1,5" : g_res == 1 ? "1" : g_res == 2 ? "2" : g_res == 3 ? "3" : "4";   /* ANDROID p39b */\n'
        '          printf("[gpu] resolución interna x%s\\n", rt);\n'
        '          { extern void arranque(const char *fmt, ...); arranque("gpu: tarjeta elegida %s; resolución interna x%s", props.deviceName, rt); } }\n')
if 'p39b' not in s39c:
    if a39c in s39c: open(f39, 'w', encoding='utf-8').write(s39c.replace(a39c, b39c, 1)); cambios += 1
    else: print('AVISO: parche 39b no aplicado')
# 40. Resumen de escenas por tiempo (pruebas): PB_RESUMEN=t<segundos> resume el primer fotograma tras esos segundos
#     desde la primera escena (para cazar una toma concreta sin saber su número de fotograma).
f40 = os.path.join(raiz, 'host', 'gxm.c')
s40 = open(f40, encoding='utf-8').read()
a40 = 'pb_res_on = e ? 1 : 0; pb_res_fot = e ? atol(e) : 0; }'
b40 = ('pb_res_on = e ? 1 : 0; pb_res_fot = e ? atol(e) : 0;\n'
       '        if (e && *e == \'t\') { pb_res_seg = atol(e + 1); pb_res_fot = 0x7FFFFFFF; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); pb_res_t0 = ts.tv_sec; } }   /* ANDROID p40 */\n'
       '    if (pb_res_seg && pb_res_activo == 2 && pb_res_rep < 12) { pb_res_activo = 0; pb_res_seg += 2; pb_res_rep++; pb_res_fot = 0x7FFFFFFF; }\n'
       '    if (pb_res_seg && !pb_res_activo) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); if (ts.tv_sec - pb_res_t0 >= pb_res_seg) pb_res_fot = 0; }')
a40b = 'static int pb_res_on = -1, pb_res_activo;'
b40b = '#include <time.h>\nstatic long pb_res_seg, pb_res_t0; static int pb_res_rep;   /* ANDROID p40 */\nstatic int pb_res_on = -1, pb_res_activo;'
if 'p40' not in s40:
    if a40 in s40 and a40b in s40: open(f40, 'w', encoding='utf-8').write(s40.replace(a40, b40, 1).replace(a40b, b40b, 1)); cambios += 1
    else: print('AVISO: parche 40 no aplicado')
# 41. Traza de funciones de la GPU (depthClamp, etc.) en el log.
s41 = open(f40.replace('gxm.c', 'gpu_vk.c'), encoding='utf-8').read()
a41 = '    if (have.depthClamp && !env_cached("MLB15_SIN_DEPTHCLAMP")) { want.depthClamp = VK_TRUE; g_depth_clamp = VK_TRUE; }\n'
b41 = a41 + ('    printf("[gpu] funciones: depthClamp %d depthBiasClamp %d fillModeNonSolid %d shaderClipDistance %d   /* ANDROID p41 */\\n",\n'
             '           (int)have.depthClamp, (int)have.depthBiasClamp, (int)have.fillModeNonSolid, (int)have.shaderClipDistance);\n')
if 'p41' not in s41:
    if a41 in s41: open(f40.replace('gxm.c', 'gpu_vk.c'), 'w', encoding='utf-8').write(s41.replace(a41, b41, 1)); cambios += 1
    else: print('AVISO: parche 41 no aplicado')
# 42. Estadio y cielo negros en las tomas lejanas (2026-11-13). Con los búferes del parche 8 (vértices 16 MB por escena)
#     la presentación con los dos equipos formados en las líneas llenaba el de vértices («escena llena 51179», máx.
#     16383 KB) y se descartaba todo lo que venía después en la escena: gradas, estadio y cielo. Vuelven a 32 MB
#     (como en el PC) y los índices a 6 MB (se midieron 2,7 MB con el límite en 3).
f42 = os.path.join(raiz, 'host', 'gpu_vk.c')
s42 = open(f42, encoding='utf-8').read()
a42 = '#define VB_SIZE   (16u << 20)   /* ANDROID: parches/aplicar.py */'
b42 = '#define VB_SIZE   (32u << 20)   /* ANDROID: parches/aplicar.py (p42: 16 MB se llenaba en las tomas lejanas) */'
a42b = '#ifdef __ANDROID__\n#define IB_SIZE   (3u << 20)\n'
b42b = '#ifdef __ANDROID__\n#define IB_SIZE   (6u << 20)   /* p42 */\n'
if 'p42' not in s42:
    if a42 in s42 and a42b in s42: open(f42, 'w', encoding='utf-8').write(s42.replace(a42, b42, 1).replace(a42b, b42b, 1)); cambios += 1
    else: print('AVISO: parche 42 no aplicado')
# 43. Público «de cartón» (2026-11-13): [android] publico=2 (o PB_PUBLICO=2). Se dibujan las figuras planas de las
#     gradas, pero el atlas del que salen (~40 espectadores 3D con esqueleto, ~70 000 índices) solo se vuelve a
#     dibujar cada PB_PUBLICO_CADA fotogramas (10 por defecto: 3 veces por segundo); entre medias se reutiliza.
#     publico=0 sin público (por defecto), 1 completo, 2 cartón.
f43 = os.path.join(raiz, 'host', 'gxm.c')
s43 = open(f43, encoding='utf-8').read()
a43 = r'''    static int on = -1;
    static uint32_t atlas[8]; static int natlas;
    static struct { uint32_t gxp; int8_t es; } cache[512];
    if (on < 0) {
        extern int launcher_ini_int(const char *section, const char *key, int def);
        const char *e = getenv("PB_PUBLICO");
        on = e && *e ? (*e == '0') : !launcher_ini_int("android", "publico", 0);   /* por defecto sin público */
        if (on) printf("[publico] sin público (android publico=0)\n");
    }
    if (!on) return 0;
    uint32_t t = gpu_target_addr();
    for (int i = 0; i < natlas; i++) if (atlas[i] == t) return 1;   /* escena del atlas del público */'''
b43 = r'''    static int on = -1, cada = 10;   /* ANDROID p43: on = 1 sin público, 2 cartón, 0 completo */
    static uint32_t atlas[8]; static int natlas;
    static struct { uint32_t gxp; int8_t es; } cache[512];
    if (on < 0) {
        extern int launcher_ini_int(const char *section, const char *key, int def);
        const char *e = getenv("PB_PUBLICO");
        int v = e && *e ? atoi(e) : launcher_ini_int("android", "publico", 0);   /* por defecto sin público */
        on = v == 2 ? 2 : v == 0 ? 1 : 0;
        const char *ec = getenv("PB_PUBLICO_CADA"); if (ec) cada = atoi(ec); if (cada < 1) cada = 1;
        if (on == 1) printf("[publico] sin público (android publico=0)\n");
        if (on == 2) printf("[publico] de cartón: el atlas se dibuja cada %d fotogramas\n", cada);
    }
    if (!on) return 0;
    uint32_t t = gpu_target_addr();
    for (int i = 0; i < natlas; i++) if (atlas[i] == t) {   /* escena del atlas del público */
        if (on == 1) return 1;
        extern unsigned port_display_frames(void);
        static unsigned ult = ~0u, act = ~0u;
        unsigned fr = port_display_frames();
        if (fr != act) { act = fr; if (ult == ~0u || fr - ult >= (unsigned)cada) ult = fr; }
        return fr != ult;
    }'''
a43b = '''        if (k == natlas && natlas < 8 && tx.data) { atlas[natlas++] = tx.data; printf("[publico] atlas en 0x%08X: sus escenas no se dibujan\\n", tx.data); }
    }
    return 1;'''
b43b = '''        if (k == natlas && natlas < 8 && tx.data) { atlas[natlas++] = tx.data; printf("[publico] atlas en 0x%08X\\n", tx.data); }
    }
    return on == 1;'''
if 'p43' not in s43:
    if a43 in s43 and a43b in s43: open(f43, 'w', encoding='utf-8').write(s43.replace(a43, b43, 1).replace(a43b, b43b, 1)); cambios += 1
    else: print('AVISO: parche 43 no aplicado', a43 in s43, a43b in s43)
# 44. Medida (2026-11-14): tiempo en psarc_read (leer y descomprimir del WAD) para cruzarlo con los tirones
#     visibles (línea "[pantalla]" de compat_win.c, que lo imprime cada 5 s).
f44 = os.path.join(raiz, 'host', 'psarc.c')
s44 = open(f44, encoding='utf-8').read()
a44 = 'int64_t psarc_read(int a, int entry, uint64_t off, void *dst, uint64_t len) {\n'
b44 = ('/* ANDROID p44: medida del tiempo de lectura (compat_win.c lo imprime con [pantalla]) */\n'
       '#include <time.h>\n'
       'volatile double pb_psarc_ms, pb_psarc_max; volatile unsigned pb_psarc_n; volatile unsigned long long pb_psarc_bytes;\n'
       'static int64_t psarc_read_real(int a, int entry, uint64_t off, void *dst, uint64_t len);\n'
       'int64_t psarc_read(int a, int entry, uint64_t off, void *dst, uint64_t len) {\n'
       '    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);\n'
       '    int64_t r = psarc_read_real(a, entry, off, dst, len);\n'
       '    clock_gettime(CLOCK_MONOTONIC, &t1);\n'
       '    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;\n'
       '    pb_psarc_ms += ms; pb_psarc_n++; if (r > 0) pb_psarc_bytes += (unsigned long long)r; if (ms > pb_psarc_max) pb_psarc_max = ms;\n'
       '    return r;\n'
       '}\n'
       'static int64_t psarc_read_real(int a, int entry, uint64_t off, void *dst, uint64_t len) {\n')
if 'p44' not in s44:
    if a44 in s44: open(f44, 'w', encoding='utf-8').write(s44.replace(a44, b44, 1)); cambios += 1
    else: print('AVISO: parche 44 no aplicado')
# 45. sceGxmFinish ligero (2026-11-14). El juego llama a sceGxmFinish en ráfagas (10-21 veces cada ~10 s, en
#     cada lanzamiento) desde sub_81032150, que espera a la GPU para liberar recursos (fotos del videomarcador,
#     fuegos, logotipos...). Con el hilo GPU asíncrono y 10 escenas en vuelo eso costaba hasta 40 ms y los
#     tramos con «Finish» eran justo los de tirones visibles (2-3 % de fotogramas dobles). La GPU no lee la
#     memoria del juego (texturas y vértices se copian), así que basta con vaciar la cola del hilo GPU y
#     completar las escenas cuyo resultado vuelve a la memoria del juego; la escena principal (solo GPU) no
#     se espera. PB_FINISH_COMPLETO=1 vuelve a esperar todo.
f45 = os.path.join(raiz, 'host', 'gpu_vk.c')
s45 = open(f45, encoding='utf-8').read()
a45 = 'void gpu_sync_all_m(int motivo) { cq_vacia(); gpu_sync_all_m_impl(motivo); }\n'
b45 = a45 + ('void gpu_finish_ligero(void) {   /* ANDROID p45: sceGxmFinish sin esperar a las escenas que se quedan en la GPU */\n'
             '    { static int sin_cola = -1; if (sin_cola < 0) sin_cola = getenv("PB_FINISH_SIN_COLA") != NULL; if (!sin_cola) cq_vacia(); }\n'
             '    if (!g_ok) return;\n'
             '    EnterCriticalSection(&g_cs);\n'
             '    for (int k = 0; k < g_nctx; k++) {\n'
             '        Target *t = g_ctx[k].busy ? g_ctx[k].tgt : NULL;\n'
             '        if (t && !t->resident && !PB_FB_SOLO_GPU(t)) CTXC(&g_ctx[k], 7);\n'
             '    }\n'
             '    LeaveCriticalSection(&g_cs);\n'
             '}\n')
if 'gpu_finish_ligero' not in s45:
    if a45 in s45: s45 = s45.replace(a45, b45, 1); open(f45, 'w', encoding='utf-8').write(s45); cambios += 1
    else: print('AVISO: parche 45 no aplicado (gpu_vk.c)')
f45b = os.path.join(raiz, 'host', 'gxm.c')
s45b = open(f45b, encoding='utf-8').read()
a45b = '    uint32_t quien = lr;\n    gpu_sync_all_m(7);\n'
b45b = ('    uint32_t quien = lr;\n'
        '    { static int completo = -1; if (completo < 0) completo = getenv("PB_FINISH_COMPLETO") != NULL;   /* ANDROID p45 */\n'
        '      extern void gpu_finish_ligero(void);\n'
        '      if (completo) gpu_sync_all_m(7); else gpu_finish_ligero(); }\n')
if 'gpu_finish_ligero' not in s45b:
    if a45b in s45b: open(f45b, 'w', encoding='utf-8').write(s45b.replace(a45b, b45b, 1)); cambios += 1
    else: print('AVISO: parche 45 no aplicado (gxm.c)')
# 46. Uniformes por escena 3 -> 6 MB (2026-11-14): en la entrada al partido y en algunas jugadas se medían
#     3006 KB con el tope en 3 MB y se descartaban dibujos («escena llena 7160 (vértices 0)»): otra causa del
#     estadio negro, además del búfer de vértices del parche 42.
f46 = os.path.join(raiz, 'host', 'gpu_vk.c')
s46 = open(f46, encoding='utf-8').read()
a46 = '#ifdef __ANDROID__\n#define MB_SIZE   (3u << 20)\n'
b46 = '#ifdef __ANDROID__\n#define MB_SIZE   (6u << 20)   /* p46: 3 MB se llenaba */\n'
if 'p46' not in s46:
    if a46 in s46: open(f46, 'w', encoding='utf-8').write(s46.replace(a46, b46, 1)); cambios += 1
    else: print('AVISO: parche 46 no aplicado')
print(f'parches de Android: {cambios} cambios')
