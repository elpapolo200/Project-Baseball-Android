# Project Baseball para Android — plan (2026-11-08)

Carpeta aparte del port de PC: `<carpeta del proyecto>`. El PC
(`VitaRE\mlb15\recomp`, `MLB15_Port`, distribuible y repo) no se toca desde aquí.

Teléfono de pruebas: **Samsung Galaxy S22 Ultra** (Snapdragon 8 Gen 1, Adreno 730, 12 GB). Por ahora
sin cable al PC: las pruebas se hacen pasando el APK y leyendo `prueba.txt`/`log.txt` que la app
escribe en su carpeta (`Android/data/com.projectbaseball.app/files`). Sin emuladores.

## Carpetas

| Carpeta | Qué |
|---|---|
| `herramientas\` | Java 17 (Temurin), Android SDK 34, build-tools 34, NDK 27.2.12479018, CMake 3.22.1, Gradle 8.7, código de SDL2 2.32.10 (~3,6 GB). Licencia del SDK aceptada por el usuario (2026-11-08) |
| `app\` | proyecto Gradle (plantilla de SDL2): `app\jni\SDL` (SDL2), `app\jni\src` (nuestro código nativo) |
| `fuente\` | pruebas y, más adelante, la copia del port adaptada |
| `apk\` | APK listos para pasar al teléfono |
| `docs\` | este plan |

Compilar el APK:

```powershell
$H='<carpeta del proyecto>\herramientas'
$env:JAVA_HOME="$H\jdk17"; $env:ANDROID_HOME="$H\sdk"; $env:GRADLE_USER_HOME="$H\gradle_home"
cd <carpeta del proyecto>\app
& "$H\gradle-8.7\bin\gradle.bat" assembleDebug --no-daemon
# -> app\app\build\outputs\apk\debug\app-debug.apk
```

## Por qué recompilación (y no ejecutar el código de la Vita)

La Vita es ARMv7 (32 bits, Thumb-2). El Snapdragon 8 Gen 1 y casi todos los móviles nuevos **ya no
ejecutan código de 32 bits**. Así que, igual que en PC, armrecomp traduce el juego a C y se compila,
ahora para ARM64 (`aarch64-linux-android`). Comprobado el 2026-11-08: una muestra de 300 funciones
traducidas compila para ARM64 **sin errores** con el clang/LLVM 19 que ya reparte el launcher.

## Sin nada del juego

Igual que en PC: `libjuego.so` se genera a partir de la copia del jugador.
1. Primera versión: el **launcher de PC** genera `libjuego.so` para Android (botón «Crear versión
   para Android»; clang + ld.lld con objetivo ARM64, ya incluidos en LLVM 19) y el jugador la copia
   al móvil junto con la carpeta `PCSA00511`.
2. Más adelante (opcional): compilar en el propio teléfono.

El APK solo lleva el port (como el exe en PC).

## Estado (2026-11-08)

- [x] Herramientas instaladas.
- [x] Código del juego traducido compila para ARM64 (muestra).
- [x] APK de prueba (`apk\ProjectBaseball-prueba-vulkan.apk`): SDL2 + Vulkan a pantalla completa, color
      cambiante, toques en pantalla, FPS y formatos de la GPU en `prueba.txt`. Compila; **sin probar en
      el teléfono**.
- [x] Inventario: de 49 fuentes (port + runtime), 17 compilan ya con el NDK; 31 solo fallan por
      `windows.h` (y `shc.c` por glslang). Lo pesado (gráficos `gxm.c`/`gpu_vk.c`, shaders v3k, audio
      `hle_ngs.c`, texturas, runtime) apenas usa Windows.
- [x] **Capa de compatibilidad** `fuente\android\include\windows.h` + `fuente\android\compat_win.c`
      (secciones críticas recursivas, variables de condición con reloj monotónico, hilos, eventos,
      semáforos, temporizador de alta resolución, relojes, atómicos, archivos con '\' → '/', búsqueda
      con comodines, `.ini` como GetPrivateProfileString, UTF-8↔UTF-16, `dlopen`, stubs GDI para la
      ventana de reserva de `hle_display.c`; `GetWriteWatch` devuelve fallo y el port no la usa).
      `fuente\android\pre\gxm_pre.h` se incluye antes de `gxm.c` (funciones usadas antes de declararse).
- [x] **40 de 43 fuentes compilan para ARM64** (`fuente\probar_ndk.ps1`): gráficos, Vulkan, kernel,
      audio, archivos, `.arc`, texturas, mando/teclado, runtime de vitarecomp. Faltan, por diseño, los
      3 de interfaz Win32: `launcher.c`, `hle_savedialog.c` (lista de partidas) y `hle_dialog.c`
      (teclado del nombre). Fuera de Android: `ui_estilo.c`, `opciones.c`, `texturas_hd.c`,
      `recompilar.c`, `watchdog.c`, `vk_comprobar.c`, `shc.c`.
- Copia de los fuentes del PC: `fuente\sincronizar_desde_pc.ps1` (sobrescribe `fuente\port`, nunca
  `fuente\android`).

## Hecho el 2026-11-08 (tarde)

- El usuario probó el APK de colores en el S22: **Vulkan + SDL2 funcionan**.
- `fuente\android\dialogos_android.c` (diálogos sin ventanas), `launcher_android.c` (carpeta pública
  `/storage/emulated/0/ProjectBaseball`: `PCSA00511/`, `eboot.elf`, `libjuego.so`, `.ini`, `datos/`;
  libjuego.so se copia a la memoria interna antes de `dlopen`; avisos con SDL_ShowSimpleMessageBox),
  `shc_android.c` (sin glslang: los shaders propios ya van en SPIR-V en `host/shaders/*.h`),
  `arranque_android.c` (SDL_main → `port_main`), `stubs_android.c` (watchdog/recompilación del PC).
- `compat_win.c`: `vulkan-1.dll` → `libvulkan.so`; la consulta de capacidades de la superficie se
  intercepta (preTransform IDENTITY + tamaño en horizontal: el S22 es vertical de fábrica, Android
  gira al componer); XInput no existe (mandos por SDL).
- Java: `ProjectBaseballActivity` (permiso «Acceso a todos los archivos», pantalla encendida) y
  `MandoEnPantalla` (botones en pantalla que mandan las teclas del port: X/C/Z/V, Q/E, Intro,
  Mayús. dcha., flechas, WASD; toques fuera de los botones = pantalla táctil de la Vita).
- **CMake con todo el port: `libmain.so` (2,4 MB) enlaza** (`-Wl,--no-dependent-libraries` por los
  `#pragma comment(lib)` del PC).
- `fuente\generar_libjuego.ps1`: armrecomp → split/fix (scripts de VitaRE\tools) → clang LLVM 19
  `aarch64-linux-android29 -march=armv8.2-a` → `ld.lld -shared` contra libmain.so (DT_NEEDED) con
  `fuente\android\libjuego.map` (solo exporta lo que pide juego_dll.c) y `juego_base_android.c`
  (TLS ELF, memoria en línea, memcpy). Salida en `fuente\salida\`.
- Instrucciones para el teléfono: `docs\LEEME_ANDROID.txt`.

- **Primer paquete completo para el teléfono** en `apk\`: `ProjectBaseball.apk` (2,6 MB),
  `ProjectBaseball\libjuego.so` (128 MB, 7 exportaciones, sus 552 importaciones existen en libmain.so)
  y `eboot.elf`. Sin probar en el teléfono. Arreglos de esta tanda: el port corre en un hilo de 16 MB de
  pila (como /STACK del exe), `-fvisibility=hidden` quitado del generador (ocultaba las entradas), dos
  importaciones por defecto (`_Xlength_error`/`_Xout_of_range`) en stubs_android.c.

## Primera prueba en el S22 (2026-11-09)

- **Menú perfecto (~38 FPS), al entrar al partido pantalla negra y se cierra**, sin ningún fallo en el
  log (se corta de golpe en `GAME_STATE_LINEUP_INTRO`, creando superficies del estadio). Memoria libre
  al arrancar: 273 MB. Dos causas:
  1. La swapchain se rehacía 1 433 veces: con preTransform IDENTITY Android devuelve `VK_SUBOPTIMAL_KHR`
     en cada acquire/present y el port lo trata como «rehacer». Ahora `compat_win.c` envuelve
     `vkGetDeviceProcAddr` → `vkAcquireNextImageKHR`/`vkQueuePresentKHR` y convierte SUBOPTIMAL en SUCCESS.
  2. Resolución interna x3 (la del PC): superficies del partido enormes → Android lo mata por memoria.
     `launcher_android.c`: `[video] resolucion` por defecto **1** en el móvil.
- Diagnóstico nuevo en `arranque.txt`: fallos con señal, PC y LR como `lib+desplazamiento`
  (`al_fallar`), y memoria del juego / libre del sistema cada vez que cambia >64 MB (`vigila_memoria`).
- Solo cambia el APK (libjuego.so sirve igual).

### Segunda prueba (2026-11-09)

- Swapchain arreglada (1 sola creación) y x1, menú a 60 FPS. Al entrar al partido sigue negro: el hilo de
  dibujo pasa ~907 ms/s en `vkCreateGraphicsPipelines` (Adreno ~7 ms por pipeline; en la NVIDIA ~0) y a
  2,3 FPS deja de presentar. Luego la memoria del juego baja (1149 → 141 MB) a la vez que la del sistema:
  compactación de app en segundo plano y cierre por Android.
- Medido en el PC a x1 con el guion del partido: GPU 550 MB dedicada + 436 compartida, RAM 1,37 GB
  (`scratchpad\gpumem.ps1`): la memoria de texturas no explica el cierre.
- Nuevo en `compat_win.c`: caché de pipelines en disco (`datos/shaders_cache.bin`, inyectada en
  `vkCreateGraphicsPipelines`, guardada cada 15 s si hay nuevas), contadores de pipelines/memoria Vulkan/
  esperas a la GPU/DEVICE_LOST. `launcher_android.c`: vigilante (8 s sin fotogramas → `ATASCO` en
  arranque.txt + pilas de todos los hilos en `datos/pilas.txt` con señal de tiempo real y
  `_Unwind_Backtrace`). `arranque_android.c`: siempre horizontal (`SDL_HINT_ORIENTATIONS`) y ciclo de vida
  (segundo plano, poca memoria) en arranque.txt.
- **Pantalla de opciones antes de jugar** (`OpcionesActivity.java`, es el icono): resolución x1-x3, FPS
  30/60, brillo, nitidez, FXAA, sombras, jugadores alta, mostrar FPS, botones en pantalla (mostrar, tamaño,
  opacidad: `[pantalla_tactil]`), comprobación de PCSA00511/eboot.elf/libjuego.so y el permiso.
  `Ini.java` lee/escribe ProjectBaseball.ini conservando el resto.

### Tercera prueba, por adb con cable (2026-11-09): ¡PARTIDO JUGABLE!

- adb: `herramientas\sdk\platform-toolsdb.exe` (en Git Bash con `MSYS_NO_PATHCONV=1` y rutas locales
  `C:/...`). Teléfono real: SM-S908U, Android 16, **7,2 GB** (modelo de 8 GB). Con PIN: hay que pedir al
  usuario que lo desbloquee. Capturas con `screencap` → `pruebas\`.
- `entorno.txt` en la carpeta del teléfono = variables de entorno del PC (p. ej. `MLB15_PAD_GUION` con
  `VitaRE	ools\guion_partido.txt` llega solo al partido). Quitarlo después de probar.
- El vigilante con `_Unwind_Backtrace` cerraba el juego (SIGILL por PAC en Android 16): ahora cadena de
  marcos x29 y 30 s de margen.
- **Causa real del cierre**: «GL mtrack» (memoria del driver) 0,8 → 3,9 GB al cargar el estadio y lmkd
  mataba el juego. Los shaders de v3k copian los uniformes al banco `sa` con un bucle de índice
  variable: Adreno manda las tablas de registros a memoria privada por hilo y por pipeline.
  `fuentendroid\parchesplicar.py` (lo llama `sincronizar_desde_pc.ps1`) desenrolla ese bucle y el
  de `outs`. Resultado: pico 1,15 GB, partido jugándose (BATTER_UP/IN_PLAY/PLAY_OVER) a ~22-25 FPS con
  límite 30. Mismo parche serviría en PC (no aplicado allí).
- Pantalla completa por defecto en el móvil (`pantalla_completa`=1): sin barras del sistema.

### Ronda de consumo 1 (2026-11-09), medida con `fuente\medir_android.py` (partido, 60 s)

| | CPU (núcleos) | GPU | FPS (límite 30) | pantalla |
|---|---|---|---|---|
| inicio | 1,95 | 63 % | 23 | 120 Hz |
| ronda 1 | 1,61 (mín. 1,51) | 58 % | 23,7 | 30 Hz |

Hecho: pantalla a la frecuencia del límite de FPS (`ProjectBaseballActivity.fijarFrecuencia`, `[android] hz`);
sondeos de semáforo/LwMutex que duermen en la variable de condición (el perfil tenía el 60 % de la CPU
en syscall/nanosleep/sched_yield); audio sin sondeo; limitador sin espera activa; QPC con `cntvct_el0`;
búferes de pantalla solo en la GPU (`PB_FB_MEMORIA=1` vuelve atrás); 8 escenas en vuelo con búferes de
16/4/4 MB; nombres de hilo de la Vita; perfilador propio `perfil_android.c` (`PB_PERFIL=inicio,dur`,
simpleperf necesita `security.perf_harden`, no se toca). Todo Android-only en `parches/aplicar.py`.
Probado: pase final y salida a 720p apenas cambian la GPU (5 puntos). Queda: el hilo «Display Engine»
(~75 % de un núcleo, ~35 ms de CPU por fotograma con ~55 escenas y ~1 150 dibujos) y la GPU (58 %).

### Ronda de consumo 2 (2026-11-09)

- La intro se deja como el original (el usuario no quiere recortes). Medido: los 22 s antes del menú son
  los 4 logos (4 s + fundidos cada uno, tabla 0x81F1592C, byte +8 = saltar, sub_81030B96); no se toca.
- Perfil del hilo de dibujo por biblioteca: port 47 %, memcpy 21 %, driver Adreno 15 %, juego 15 %.
- Hecho: do_draw con índices en rango compacto sin tabla de sellos (parche 10; ~23 -> 24,7 FPS);
  `PB_SOLO_GPU=dir,...` para probar superficies que no vuelvan a memoria (0xA4E8A160 se ve bien, sin
  ganancia clara, apagado); `[android] nucleos` / `PB_NUCLEOS` (afinidad; sin X2 no ayuda: Android ya
  pone el hilo de dibujo en los A710); `appCategory=game`/`isGame` (Game Booster de Samsung); modo
  sostenido pedido (`[android] sostenido`) pero el S22 dice «no disponible».
- `medir_android.py` ahora da MHz medios por grupo (A510/A710/X2), temperatura máxima de CPU/GPU y en
  qué núcleos corre cada hilo. OJO: tras muchas pruebas seguidas el teléfono ya está caliente (CPU
  72-82 °C) y baja frecuencias: comparar solo pruebas hechas en las mismas condiciones.
- Límite 24 o 30 FPS dan lo mismo (22,7): el ritmo lo marca la ida y vuelta hilo principal <-> hilo de
  dibujo (se esperan uno al otro). Siguiente palanca: que el hilo de dibujo haga menos por dibujo
  (draw_v3k, copias de vértices, hash de texturas por escena) o paralelizar la traducción.

### Opción 2: qué dibuja el juego (2026-11-09)

`PB_RESUMEN=<fotograma>` (parche 12) lista las escenas de un fotograma. Partido (fotograma ~6700): 7
escenas. La principal (720x408) tiene 1 144 dibujos / 377 000 índices: estadio ~700 dibujos pequeños
(Texture0+Lightmap0+StadiumCastShadowsTexture, 3 programas), jugadores ~100, 112 sprites diminutos,
público 47 figuras (CrowdSpriteTextureSampler). Antes: atlas del público 256x256 con ~40 espectadores 3D
con esqueleto (70 000 índices) y su composición 512x1024, mapa de sombras (37 dibujos) y la escena
960x544 de sombras del montículo (33).
- **Sin público por defecto** (parche 13, `[android] publico=1` o el interruptor de la pantalla de
  opciones lo devuelve): FPS 24,7 -> 26,8-28, GPU 60 -> 55 %, A710 1 834 -> ~1 600 MHz.
- Siguiente: el estadio son ~700 dibujos casi iguales cada fotograma: guardar entre fotogramas lo ya
  traducido (vértices/índices) de los que no cambian.

### Menos trabajo por dibujo (2026-11-09)

- Parche 14: vértices contiguos sin la copia intermedia a g_cbuf (se copiaban dos veces): preparación
  140 -> 100 ms/s, FPS 28 -> 29,7.
- Parche 15: texturas estables (60 comprobaciones sin cambios) solo se comprueban cada 8 fotogramas:
  «texturas» 90 -> 44 ms/s, FPS -> 30,9 (límite 60). Con límite 30: ~27-30 FPS, CPU máx. 70 °C,
  A710 1 555 MHz.
- Ahora la espera dominante es «contexto» (~170 ms/s): la GPU empieza a ser el límite.

### 30 FPS estables (2026-11-09)

- Medición nueva: frecuencia de la GPU (`/sys/class/kgsl/kgsl-3d0/clock_mhz`, iba a ~220 de 818 MHz) y
  fotogramas lentos (`[lentos]`, parche 16).
- Causa de no llegar a 30: el hilo de dibujo esperaba a la GPU. Con 8 en vuelo eran esperas de
  «contexto» (~160 ms/s); con más contextos pasaban a esperas de «textura»: los cubemaps de luz de los
  jugadores (8x8x6, `CubeGloss`) los pinta la GPU dentro de una superficie de 64x512 y la escena
  principal los lee en el mismo fotograma (`PB_OVERLAP_TRAZA=1`, parche 18).
- Parche 17: pend_overlap con el tamaño real de la textura (antes ancho x alto x 8).
- Parche 19: si esa textura ya existe y su superficie sigue en la GPU, se usa la del fotograma anterior
  (`PB_ESPERA_TEXTURAS=1` vuelve atrás).
- Parche 8: NCTX 16, 10 en vuelo por defecto (cada contexto ~200 MB entre búferes y descriptores; 16 =
  4,9 GB de PSS, 10 = 3,6 GB, 8 = 3,1 GB pero 28,3 FPS).
- Resultado con límite 30: 29,7 FPS de media, mínimo de 5 s 29,1, sin esperas de textura.

### 30 FPS fijos y más recortes (2026-11-09, tarde)

- 30 FPS fijos en Android: sin opción en OpcionesActivity, `launcher_ini_int("video","fps_max")` devuelve
  30 y la pantalla va a 30 Hz.
- Parche 20: superficies de >=128x128 servidas desde la GPU y nunca leídas por memoria no vuelven a
  memoria. Parche 21: caché de vértices (64 MB, 99,7 % de aciertos, 5,6 MB usados). memcpy del hilo de
  dibujo 16 % -> 5,5 %.
- ADPF (`adpf_android.c`, parche 22): el S22 responde «PowerHintSessions are not supported»; se queda
  para otros móviles. Quitar `appCategory=game` no cambia la GPU (se deja puesto).
- Estado: ~29,5 FPS de media, pero 20-40 % de fotogramas de 36-50 ms (tirones). Medido con el teléfono
  muy caliente tras horas de pruebas (batería 43-44 °C): repetir en frío. La GPU va a ~220-240 MHz.
- Ideas siguientes: repartir la traducción de dibujos en dos hilos (más estable, algo más de consumo),
  menos llamadas a Vulkan por dibujo (descriptores/binds repetidos), ritmo de fotogramas más suave.

### Hilo GPU y vsync (2026-11-09, noche)

- Parche 23: hilo «GPU Hilo» que graba Vulkan desde una cola (dibujos con copia de sus datos; empezar y
  terminar escena y presentar vacían la cola). Parche 26 (asíncrono del todo) se BLOQUEABA al arrancar:
  desactivado por defecto (`PB_GPU_ASINCRONO=1` para depurarlo). Volcado de pilas a petición: crear
  `datos/volcar` (vigilante).
- Vsync: `compat_win.c` deja solo FIFO y la cadena en el mínimo del driver (4 imágenes); el limitador del
  port va a 32 como red. Con FIFO la cuenta de `[lentos]` (intervalos en la CPU) ya no mide lo que se ve.
- Trazador de fotogramas lentos (parche 25, `[lento]`): en el partido el hilo de dibujo gasta 25-40 ms de
  CPU en los fotogramas lentos; el principal ~9 y el hilo GPU ~15.
- Probado sin resultado: afinidad por papel (`nucleos_criticos/resto`), prioridad -10 de los críticos,
  ADPF (no soportado en el S22), quitar `appCategory=game`.
- Medido al final: 30,0 FPS de media, mínimo de 5 s 29,8, con 150 presentaciones cada 5 s.

### x2 más ligero (2026-11-09)

- Parche 27: la resolución interna solo se aplica a los búferes de pantalla (`pb_res_para`, por
  superficie: `Target.pb_res`, scissor, viewport, sesgo, `gpu_res_scale`); sombras, máscaras y RTT van a
  x1 (`PB_RES_TODAS=1` lo quita). x2: GPU 374 -> 296 MHz con la misma imagen, memoria 4,0 -> 3,7 GB.
- A x2 el FXAA se apaga solo (launcher_ini_int de `suavizado`): GPU 76 -> 69 %.
- Opciones del móvil: solo x1 / x2 (x3 fuera; `resolucion` se limita a 2).
- x1 sigue siendo lo recomendado: GPU ~220 MHz frente a ~285 a x2.

### 30 FPS ESTABLES (2026-11-09, noche)

- El bloqueo del hilo GPU asíncrono: gpu_scene_begin toma g_cs y lo suelta gpu_scene_end (toda la
  escena); con el hilo GPU empezando la escena, gpu_module (shader nuevo a mitad de escena en el hilo de
  dibujo) esperaba g_cs para siempre. Parche 28: gpu_module con candado propio. Asíncrono activado por
  defecto (parche 26, `PB_GPU_ASINCRONO=0` lo quita). Hilo de dibujo 17-23 ms por fotograma, sin esperas.
- Medida buena = compositor: `dumpsys SurfaceFlinger --timestats -enable -clear` / `-dump` (histograma
  present2present de la capa SurfaceView). Partido, 40 s:
  - **x1: 1 203 fotogramas, los 1 203 a 33 ms (30,3 FPS) — 30 estables.**
  - x2: 1 194 fotogramas, 1 185 a 33 ms, 8 a 66 ms, 1 a 102 ms.
- Retraso de presentación (desired2present) 40-110 ms con FIFO y 4 imágenes: vigilar si se nota al batear.

### Retraso de botones, guardado (2026-11-09, noche)

- **Swappy** (Android Frame Pacing 2.1.3, Maven `androidx.games:games-frame-pacing`, Prefab,
  `swappy_static`; obliga a `c++_shared` y `android.useAndroidX=true`). Ganchos en `compat_win.c`:
  vkCreateDevice (añade VK_GOOGLE_display_timing; pedir las extensiones en dos pasos, una venía vacía y
  daba -7), vkGetDeviceQueue, vkCreateSwapchainKHR (init + 33,3 ms + sin auto-intervalo ni auto-pipeline;
  con auto bajaba a 24 Hz), vkQueuePresentKHR -> SwappyVk_queuePresent, vkDestroySwapchainKHR.
  `[android] swappy=0` lo quita. Medido x1, 40 s: 1 232 fotogramas, 1 230 a 33 ms, 2 a 66 ms; retraso
  (desired2present) ~15 ms (antes 40-110 ms con FIFO). `fuente/medir_pantalla.sh` mide esto.
- Guardado: la ruta salía doble (GetModuleFileNameA con «/»; vfs recorta por «\»): ahora `dir\ProjectBaseball`
  y al arrancar se mueven las partidas de ProjectBaseball/ProjectBaseball/datos a datos. Lista táctil de
  partidas: `ProjectBaseballActivity.elegirPartida` (AlertDialog) llamada por JNI desde
  dialogos_android.c (título, subtítulo y fecha de cada ranura; cancelar). Sin probar a mano.

### x2 más estable (2026-11-09/10)

- Medido en el compositor, 120 s de partido a x2: de 56 fotogramas perdidos (66 ms) a 21-39 según la
  prueba. Todavía NO es estable. El teléfono estuvo caliente (batería 38-42 °C, `thermal_pwrlevel` 10 =
  GPU limitada a 285 MHz buena parte del tiempo).
- Parche 29: profundidad/stencil con storeOp DONT_CARE si nadie la carga ni la muestrea (GPU 76 -> 74 %).
  Parche 30: anisotrópico 4x en vez de 16x (`PB_ANISO`): trabajo de GPU -17 % a x2. Parche 31: pase final
  bilineal ampliando <1,5x (sin ganancia medible).
- La GPU va al ~66 % de media: los fotogramas perdidos son picos, no falta de potencia media. En
  `pruebas\log_lim_def.txt` (traza `[lento]` ampliada a >44 ms, hasta 600): picos del hilo GPU (25-40 ms
  de CPU, el hilo de dibujo espera la cola; sospecha: pipelines nuevas al cambiar de cámara), esperas en
  `sceGxmFinish` (185 llamadas en 2 min, 3,1 s, peor 39 ms) y algún pico del hilo principal (50 ms).
- Sin efecto: `MLB15_FPS=40` (limitador), `MLB15_NITIDEZ=0`, `MLB15_POSPROCESO=0` (-8 puntos de GPU pero
  quita brillo/nitidez). `pruebas\compilar_instalar.ps1` compila e instala.

### Mando en pantalla nuevo (2026-11-10)

- `MandoEnPantalla.java` rehecho: faltaba la palanca DERECHA (la Vita tiene dos). Ahora: cruceta de una
  pieza (deslizable, diagonales), △○✕□ dibujados (no texto) con rodar de uno a otro, L/R, START/SELECT
  abajo en el centro, dos palancas semiflotantes, animación al pulsar, vibración
  (`[pantalla_tactil] vibracion=0` la quita) y atenuado a los 3,5 s sin tocar.
- Palancas analógicas: `nativoStick` (stubs_android.c, `pb_tactil_eje`) + parche 32 en input.c; sin la
  función nativa caen a W/A/S/D e I/J/K/L. START/SELECT en las esquinas de abajo (en el centro tapaban las pestañas GAME MODES/OPTIONS del menú).
  Probado por adb con toques simulados (capturas en `pruebas\mando*.png`): START entra al menú, la palanca
  izquierda mueve la selección, ✕ y la palanca se iluminan al pulsar. Sin probar: la palanca derecha
  dentro del juego, varios dedos a la vez y un partido.

### Ampliar la imagen (2026-11-10)

- El juego es 960x544 (1,76) y la pantalla del S22 Ultra 2316x1080 (2,14): a toda la altura quedan ~205 px
  negros a cada lado. Parche 33: `[video] zoom` (100-125 %, `PB_ZOOM`, deslizador «Ampliar imagen» en
  opciones) amplía sin estirar y recorta arriba y abajo. Medido en el menú (`pruebas\zoom_comparacion.png`):
  al 106 % ya corta media línea del título MAIN MENU y parte de las pestañas; al 112 % las corta casi
  enteras. Se deja en 100 % por defecto.
- El toque se traduce ahora sobre el rectángulo real de la imagen (`gpu_ventana_a_panel`; antes sobre toda
  la pantalla, desviado por las franjas). Sin confirmar: un toque simulado sobre la pestaña OPTIONS no hizo
  nada ni antes ni después (puede que esa pestaña no sea táctil).
- Para llenar la pantalla de verdad haría falta panorámico real: dibujar el 3D más ancho (proyección y
  superficie 720x408 del juego) dejando el HUD en 16:9. Sin empezar.

### Temperatura en pantalla (2026-11-10)

- `TemperaturaEnPantalla.java`: temperatura de la batería (ACTION_BATTERY_CHANGED, leída cada 2 s) bajo los
  FPS; amarillo desde 40 °C o estado térmico «moderado» de Android, rojo desde 44 °C o «grave».
  `[video] mostrar_temp` (1 por defecto) e interruptor «Mostrar temperatura del teléfono» en opciones.
  Probado: marca 39,5 con `dumpsys battery` en 39,6. Los sensores de CPU/GPU no son legibles para una app.

### Puntería del lanzamiento «a saltos» (2026-11-11, cambio hecho, falta probar lanzando)

- Queja: al apuntar el lanzamiento con la palanca izquierda solo hay unas 3 posiciones fijas.
- Descartado el mando: `PB_PALANCA_TRAZA=1` (parche 34, línea `[palanca]` en log.txt) con un empuje gradual
  simulado da 137, 164, 191, 215 en X y 86 en Y: el port entrega al juego la palanca analógica completa
  (la función nativa se usa; no cae a teclas).
- Descartado también: `vcvt` de punto fijo de VFP sin traducir (no aparece ninguno en `pseudo\eboot.asm`).
- En PC con mando la puntería va suave (lo confirma el usuario), así que no es la traducción del juego.
  Causa probable: la palanca en pantalla llegaba a tope con ~108 px (~7 mm de pulgar) y sin curva. Cambiado
  en `MandoEnPantalla.muevePalanca`: tope a 1,5 radios (~200 px; `[pantalla_tactil] recorrido`, en %), zona
  muerta 0,07 y curva t^1,6. Medido en diez pasos de 20 px: 133, 141, 152, 164, 178, 194, 212, 231, 252.
  Falta que el usuario lo pruebe lanzando.
- (Hipótesis anterior) Queda dentro del juego o de su traducción. Siguiente: comprobar si en PC con mando pasa igual; localizar
  quién lee el pad durante el lanzamiento (`MLB15_CTRL_TRACE`) y seguir cómo convierte lx/ly a decimal.

### Caras y estadio en negro (2026-11-13)

El usuario vio en negro la cara del jugador creado (RTTS) y el estadio de lejos en los cambios de entrada.
Reproducido en el S22 con la carrera `RTTS_010` (práctica de bateo; guion
`@MUSIC.SKX+6:start,+9:start,+5:derecha,+2:derecha,+2:derecha,+3:cruz,+4:abajo,+2:cruz,+12:cruz,+8:cruz,+8:cruz`,
capturas con `fuente\capturas_partido.py` + `hoja_capturas.py`). Con `PB_FB_MEMORIA=1` se veía bien: el parche 20
(«superficies solo en la GPU») dejaba sin copiar a la memoria del juego superficies que se componen pocas veces
(la cara del jugador creado) y que el juego luego lee de memoria.
- Parche 36: «solo GPU» únicamente si la superficie se ha dibujado en 60 fotogramas seguidos. Verificado: la cara
  de Joe Random se ve, 29-30 FPS.
- Parche 35: el camino del parche 19 (textura del fotograma anterior) también marca `pb_cpu`. No era la causa de la
  cara, pero es correcto.
- Sin verificar: el estadio negro de lejos (no se reprodujo en AT&T Park de día; el usuario lo vio en el de los Reds).
- APK con el logo nuevo de Project Baseball (icono adaptable).

### Palanca más rápida y mando reordenado (2026-11-13)

- La palanca izquierda se sentía lenta al moverse: venía del cambio del 11-nov (tope a 1,5 radios y curva t^1,6).
  Ahora tope a 1 radio (`[pantalla_tactil] recorrido` 100 por defecto) y curva t^1,2. Medido con `PB_PALANCA_TRAZA=1`
  y empujes de 20-140 px: 134, 150, 165, 191, 206, 229, 255 (antes 133 ... 212 a 140 px).
- A la derecha: la palanca derecha arriba (ay 0,70) y △○✕□ abajo (centro del rombo ay 0,30), al revés que antes.
  Comprobado en captura del menú.

### Volver desde otra app, videomarcador y x1,5 (2026-11-13)

- **Pantalla negra al volver desde otra app** (se oía pero no se veía): al pasar a segundo plano Android destruye la
  ventana y el juego seguía presentando en la superficie vieja (logcat: «BufferQueue has been abandoned»,
  `dequeueBuffer failed: No such device`). Parche 37: en segundo plano no se presenta y al volver
  (`SDL_APP_DIDENTERFOREGROUND`, avisado desde `arranque_android.c` con `gpu_android_ciclo`) o si adquirir imagen
  falla se crean superficie y swapchain nuevas (reintento cada 250 ms). Verificado: inicio del sistema y vuelta,
  la imagen vuelve, 29 FPS, Swappy se reactiva y la pantalla sigue a 30 Hz.
- **Videomarcador del estadio muy borroso a x2**: el parche 27 (resolución solo en la pantalla) dejaba a x1 la
  superficie donde se pinta la ficha del jugador. Encontrada por descarte (`PB_RES_TAMANOS`, traza
  `PB_TRAZA_SUP=1` con cada superficie creada, 25 capturas seguidas para descartar que fuera una animación): el
  juego la declara de **16x16**. Parche 38: las superficies de `PB_RES_TAMANOS` (por defecto `16x16`) siguen la
  resolución elegida. Verificado a x2: se ven la cara y el texto. Coste nulo (superficie diminuta).
- **Opción x1,5** (parche 39 + pantalla de opciones x1 / x1,5 / x2): la escala de cada superficie va en mitades
  (`pb_res2`: 2 = x1, 3 = x1,5, 4 = x2) y se usa como decimal en viewport, recorte, sesgo de profundidad y
  `res_multiplier`. `[video] media=1` (o `PB_RES15=1`). **Verificado en el S22 (`PB_RES15=1`)**: encuadre
  correcto en todo el partido, videomarcador nítido, 29-30 FPS, 37 °C, GPU ~65 %. Parche 39b: el log y `arranque.txt`
  dicen «x1,5» (antes salía «x1,5» y luego «x2»). También probado eligiendo x1,5 desde el ini
  (`resolucion=1`, `media=1`): log «x1,5», 30 FPS.
- **Estadio y cielo negros en las tomas lejanas (ARREGLADO, parche 42)**: al entrar al partido (presentación con los
  dos equipos formados en las líneas) todo lo que no era césped salía negro unos segundos; también en otras tomas
  lejanas. En el PC no pasa. Descartados: optimizaciones de Android (todas desactivadas), público, depth clamp (el
  Adreno 730 lo tiene; traza `[gpu] funciones`, parche 41). Con `PB_RESUMEN=t<seg>` (parche 40: resumen de escenas
  por tiempo, uno cada 2 s) se vio que el juego sí dibuja estadio y cielo con los mismos shaders que en la toma de
  bateo; quitando programas con `MLB15_V3K_SALTA` salió el culpable (fragmento `0x83A44518`, jugadores con
  esqueleto, 164 dibujos), pero no por el shader: el log decía **«escena llena 51179, vértices máx 16383 KB»**. El
  búfer de vértices por escena del parche 8 (16 MB) se llenaba y se descartaba todo lo que venía detrás (gradas,
  estadio, cielo). Ahora 32 MB (como el PC) e índices 6 MB (se midieron 2,7 MB con límite 3). Verificado: toma de
  entrada con estadio, gradas y cielo, 0 dibujos descartados, 28-30 FPS.
- **Público «de cartón» (parche 43 + opciones No / Cartón / Completo)**: las gradas ya son figuras planas, pero el
  atlas del que salen (~40 espectadores 3D con esqueleto, ~70 000 índices) se redibujaba cada fotograma. Con
  `[android] publico=2` (o `PB_PUBLICO=2`) se dibujan las figuras y el atlas solo cada `PB_PUBLICO_CADA` fotogramas
  (10: 3 veces por segundo). `publico`: 0 sin (por defecto), 1 completo, 2 cartón. Verificado: público visible en
  las gradas, 29-30 FPS; traducir dibujos 280 ms/s con público completo, 232 cartón, 236 sin público (el cartón
  cuesta en CPU lo mismo que no tener público). `fuente\medir_partido.py` mide GPU %, temperatura, FPS y ms/s (el %
  de GPU solo no compara bien: Android cambia la frecuencia y `gpuclk` no se puede leer sin root).
- Herramientas: `fuente\capturas_partido.py` (capturas periódicas por adb, `GUION=` y variables),
  `fuente\hoja_capturas.py` (hoja de miniaturas), `pruebas\_tira.py` (recorte del marcador en serie).

- **Opciones en español e inglés** (2026-11-14): selector «Idioma / Language» arriba en la pantalla de opciones ([android] idioma = es por defecto / en); todos los textos de OpcionesActivity con `t(es, en)`, más el aviso de permiso y «Cancelar» de ProjectBaseballActivity. El multijugador lleva la etiqueta EXPERIMENTAL y un aviso. Verificado en el S22 en los dos idiomas.

### 30 FPS más estables y el estadio negro otra vez (2026-11-14)

- **Lo que se ve en pantalla** (`[pantalla]` en compat_win.c: tiempo entre presentaciones de Swappy, cada 5 s):
  casi siempre 30 FPS exactos. Los «fotogramas lentos» del hilo del juego (`[lentos]`, ~23 % > 36 ms) los absorben
  el hilo GPU y Swappy; los tirones visibles reales eran el **2,4 %**, en ráfagas cada ~10 s (cada lanzamiento).
  SurfaceFlinger `--latency` no da datos de esta capa en Android 14.
- Descartados (medidos con `fuente\medir_tirones.py`): modo sostenido apagado (2,78 % y 67 °C: se queda
  encendido), resolución x1 (2,19 %: no es la GPU), lecturas del WAD (parche 44, medida: sin relación), ADPF (la
  sesión **no está disponible** en este teléfono; ahora con 3 hilos y objetivo 25 ms por si en otro sí).
  `fuente\medir_nucleos.py`: los hilos van en los núcleos grandes pero a ~1 GHz (modo sostenido).
- **Causa: `sceGxmFinish`** en ráfagas (10-21 cada lanzamiento) desde `sub_81032150` (el juego espera a la GPU
  para liberar recursos: fotos del videomarcador, fuegos, logotipos...), hasta 40 ms cada una. **Parche 45**:
  Finish ligero (vacía la cola del hilo GPU y completa solo las escenas que vuelven a la memoria del juego; la
  pantalla, solo GPU, no se espera). `PB_FINISH_COMPLETO=1` vuelve atrás. Tirones **2,41 % -> 1,07 %**.
- **Estadio negro de vuelta**: el parche 8 se comprobaba buscando «16u» y, tras el 42 (32 MB), se volvía a aplicar
  cada vez que se corría `aplicar.py` sobre la copia ya parcheada (pasó desde el parche 43): `#define` anidados y
  otra vez 16 MB en Android. Arreglada la comprobación; copia resincronizada y aplicar dos veces da 0 cambios.
  Además los **uniformes por escena** (3 MB) también se llenaban («escena llena 7160 (vértices 0)», máx. 3153 KB):
  **parche 46**, 6 MB.
- **Resultado** (x1,5, público de cartón, partido con guion 3 min): **0 escenas llenas, tirones visibles 0,77 %**,
  56 °C, toma de entrada con estadio y cielo. Memoria del juego ~1,06 GB.

### Generar libjuego.so dentro del teléfono (2026-11-14, en curso)

Para que la APK sirva sin PC (y sin repartir nada del juego): la app prepara `libjuego.so` la primera vez, como el
launcher de PC.
- `PreparacionActivity` (pantalla «Preparando el juego», ES/EN, barra de progreso): copia `assets/generador` a
  `files/generador`, `libarmrecomp.so emit eboot.elf` -> `juego.c` (~480 MB), `libpartir.so` (trozos de 12 MB; es
  la parte de partir/corregir de `recompilar.c` sacada a `fuente/android/movil/partir.c`), `libclang.so` en
  paralelo (núcleos-1, ~1,3 GB de memoria libre por clang, máx. 6; borra cada trozo al compilarlo), `liblld.so`
  contra `libmain.so` de la app. Deja `libjuego.so` + `libjuego.txt` (huella: tamaño y CRC del eboot + versionCode)
  en `ProjectBaseball/`. Pico ~1,1 GB en `files/gen`, se borra al acabar.
- `OpcionesActivity`: si hay `eboot.elf` y falta `libjuego.so` (o la huella no coincide) -> «el juego se preparará al
  tocar JUGAR» y JUGAR abre la preparación. Una `libjuego.so` puesta a mano sin huella se respeta.
- Herramientas como `lib*.so` en `app/libs/arm64-v8a` (Android solo deja ejecutar lo que viene en la APK;
  `extractNativeLibs="true"`). `fuente/preparar_generador.ps1` las compila/copia: armrecomp y partir con el NDK,
  clang y lld de LLVM 19.1.7 para Android (`fuente/llvm_host.cmd` = tblgen para Windows,
  `fuente/llvm_android.cmd` = clang+lld ARM64, solo AArch64, libc++ estática).
- **Medido en el S22 por adb** (`fuente/android/movil/preparar_prueba.sh`): armrecomp 5 s (C 417 MB), partir 3 s
  (35 trozos), clang 6 a la vez 1 494 s, lld 2 s: **25 min**, libjuego.so 134 MB, batería 42,7 °C. Esa libjuego.so
  juega igual que la del PC (30 FPS, 0,86 % de tirones, 0 trampas). clang usa 400-520 MB por trozo (el cálculo de
  hilos va con 700 MB). clang 85,5 MB y lld 45,3 MB; **APK con el generador: 54,5 MB**.
- **Probado desde la app (de principio a fin)**: sin libjuego.so, opciones -> «○ el juego se preparará al tocar
  JUGAR» -> JUGAR -> «Preparando el juego» (4 clang a la vez: el cálculo da 4 con ~3 GB libres) -> **«Listo,
  preparado en 26 min 5 s»**, libjuego.so + libjuego.txt en ProjectBaseball/. Partido con esa libjuego.so: 30 FPS,
  0,90 % de tirones, 0 trampas. Opciones: «✓ juego preparado».
- Respaldos en el teléfono: `libjuego_pc.so` (la de siempre) y `libjuego_movil.so` (la de la prueba por adb).
- Pendiente: sacar el eboot de la carpeta de Vita3K (eboot.bin) sin PC; acortar los 25 min (¿-O1 en trozos fríos?).

## Siguiente paso inmediato (antes de esto)

1. Versiones Android de los diálogos (`fuente\android\dialogos_android.c`): misma lógica que
   `hle_dialog.c`/`hle_savedialog.c` en su modo sin ventana (cargar = ranura más reciente, guardar =
   la que pide el juego, teclado = texto inicial). Más adelante, interfaz táctil propia.
2. `fuente\android\launcher_android.c`: las funciones `launcher_*`/`arranque` que usa el port, con
   rutas de Android (carpeta de la app; carpeta del juego `PCSA00511` copiada en
   `Android/data/com.projectbaseball.app/files/PCSA00511`; `ProjectBaseball.ini` en la misma carpeta).
3. Shaders propios precompilados a SPIR-V (`host\shaders\*.vert/.frag`) en vez de compilarlos con
   glslang 16 en el teléfono (`shc.c` fuera).
4. CMake de la app con todo (`libmain.so` = port + SDL2 + v3k + LibAtrac9) y primer arranque con un
   `libjuego.so` generado en el PC.
5. Generador de `libjuego.so`: armrecomp + `clang --target=aarch64-linux-android29` + `ld.lld -shared`
   con un `juego_base.c` para ELF.

## Fases

1. **Capa de compatibilidad** (`fuente\android\windows.h` propio): el subconjunto de Win32 que usa el
   port sobre pthreads/POSIX (CRITICAL_SECTION, CONDITION_VARIABLE, eventos, hilos, Sleep,
   QueryPerformanceCounter, Interlocked*, TLS, archivos básicos). Los fuentes del port quedan casi
   iguales a los de PC (fácil traer arreglos). Lo que es solo de PC (launcher Win32, opciones GDI+,
   texturas HD, recompilación, watchdog con SuspendThread) se sustituye por versiones Android.
2. **`libjuego.so`**: el C de armrecomp con `--target=aarch64-linux-android29 -fPIC`, `juego_base.c`
   para ELF (TLS nativa `__thread`, sin el directorio TLS de Windows) y enlace con `ld.lld -shared`.
   Las funciones del port que usa el juego se le pasan con una tabla (o el port se carga con
   `RTLD_GLOBAL`). minSdk 29 (Android 10): TLS ELF nativa (con 26 el NDK usa TLS emulada, lentísima
   para los registros de la CPU, que se leen en cada instrucción).
3. **Primera imagen en el teléfono**: menú del juego con Vulkan (SDL2 ventana Android, superficie,
   swapchain con la rotación del teléfono).
4. **Partido jugable**: pantalla táctil (la Vita tiene táctil: directo), mando Bluetooth (SDL2),
   audio (SDL2/AAudio), partidas guardadas en la carpeta de la app, carpeta del juego elegida por el
   usuario.
5. **Ajuste móvil**: resolución x1-x2, 30/60 FPS, temperatura. GPU Adreno: formatos (BC no existe en
   móvil, el port ya decodifica en la CPU), memoria compartida (ventaja: sin copias por PCIe).
6. **APK distribuible** y release en GitHub.

## Notas técnicas

- Rendimiento esperado: el hilo de dibujo usa ~0,6 de núcleo en el portátil (Core 7 240H). Un
  Cortex-X2 del 8 Gen 1 es más lento por núcleo; objetivo realista 30 FPS seguro, 60 a x1.
- `-march=native` no aplica: para Android se compila para `armv8.2-a` (el 8 Gen 1 es ARMv9, pero
  armv8.2 cubre cualquier móvil de 64 bits razonable).
- NEON escrito en C (fase 3 del plan de rendimiento) en ARM64 vuelve a ser NEON nativo.
