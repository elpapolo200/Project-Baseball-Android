# Project Baseball para Android

Port nativo de **MLB 15 The Show** (PS Vita) para teléfonos Android, hermano del port de PC
[Project Baseball](https://github.com/elpapolo200/Project-Baseball). El juego de la Vita se traduce a código
nativo ARM64 y sus gráficos van por Vulkan en la GPU del teléfono (con el traductor de shaders de Vita3K).

> **Este repositorio y la APK no contienen nada del juego.** Necesitas tu propia copia de MLB 15 The Show
> (PCSA00511). Ni el código ni los datos del juego se distribuyen aquí.

**Estado:** versión de pruebas (prerelease). Probado sobre todo en un Samsung Galaxy S22 Ultra.

## Qué necesitas

- Teléfono **ARM64 con Android 10 o superior** y Vulkan 1.1.
- En la memoria interna, una carpeta llamada `ProjectBaseball` (junto a `Download`, `DCIM`...) con:

```
ProjectBaseball/
    PCSA00511/     tu carpeta del juego DESCIFRADA (la de Vita3K): VITA_COMPRESSED_WAD.arc, VITA_PATCH_WAD.arc...
    eboot.elf      el eboot del juego en claro
    libjuego.so    el juego traducido para Android, generado a partir de TU eboot
```

`libjuego.so` se genera en un PC con `fuente/generar_libjuego.ps1` y las herramientas del port de PC
(armrecomp + LLVM, repositorio [Project Baseball](https://github.com/elpapolo200/Project-Baseball)):

```powershell
.\fuente\generar_libjuego.ps1 -Elf <ruta>\eboot.elf -VitaRE <carpeta del port de PC>
```

Como lleva el código del juego, cada persona tiene que generar el suyo: **no se comparte ni se sube a ningún sitio.**

## Instalar y jugar

1. Descarga `ProjectBaseball-vX.Y.Z.apk` de [Releases](https://github.com/elpapolo200/Project-Baseball-Android/releases) e instálala.
2. Ábrela: pide el permiso **«Acceso a todos los archivos»** (para leer la carpeta `ProjectBaseball`).
3. Copia la carpeta `ProjectBaseball` al teléfono. En la pantalla de opciones el recuadro debe salir en verde.
4. Toca **JUGAR**.

**Pantalla de opciones:** idioma (español / English), resolución interna (x1, x1,5, x2), brillo, nitidez, zoom,
suavizado, sombras, público en las gradas (no / cartón / completo), botones en pantalla y multijugador.

**Controles:** botones en pantalla (se ocultan con «◐»), pantalla táctil de la Vita tocando fuera de los botones,
o un mando Bluetooth.

## Multijugador en la misma wifi (EXPERIMENTAL)

Dos teléfonos en la misma wifi, cada uno con su juego y **la misma versión de la app**:

1. Uno toca **Crear sala**; en pantalla sale su código (por ejemplo `0.8`).
2. El otro escribe ese código y toca **Unirse**.
3. Los dos entran a una **Exhibición**: el que crea la sala juega en casa.

Usa el modo de conexión directa por IP que trae el propio juego. Todavía en pruebas: el partido puede cortarse
o desincronizarse. Detalles técnicos en [docs/MULTIJUGADOR.md](docs/MULTIJUGADOR.md).

## Si algo falla

En `ProjectBaseball/datos/` quedan `arranque.txt` (qué encontró, GPU, FPS) y `log.txt` (registro completo).

## Compilar

- JDK 17, Android SDK (API 34) y NDK, Gradle 8.7. Proyecto en `app/` (`gradlew assembleDebug`).
- `fuente/port/`: el código del port compartido con el PC (se trae con `fuente/sincronizar_desde_pc.ps1`).
- `fuente/android/`: lo propio de Android y `parches/aplicar.py` (parches solo de Android sobre `fuente/port`).
- Notas de desarrollo: [docs/PLAN_ANDROID.md](docs/PLAN_ANDROID.md).

## Licencia y créditos

GPL-2.0 (ver [LICENSE](LICENSE)). Usa código de **Vita3K** (traductor de shaders, GPL-2.0), **SDL2** (zlib),
**LibAtrac9** (MIT), **glslang** (BSD/Apache), **fmt** (MIT) y **Swappy / Android Frame Pacing** (Apache 2.0).

Proyecto de aficionados sin relación con Sony Interactive Entertainment, San Diego Studio ni MLB.
*MLB 15 The Show* es marca de sus respectivos dueños.

---

## English

Native Android port of **MLB 15 The Show** (PS Vita), sibling of the PC port. **No game content is included**:
you need your own copy (PCSA00511). Put a `ProjectBaseball` folder in internal storage with your decrypted
`PCSA00511` folder, `eboot.elf` and `libjuego.so` (generated on a PC from *your* eboot with
`fuente/generar_libjuego.ps1` and the PC port tools; never share it). Install the APK from Releases, grant
"All files access", and tap PLAY. The options screen has an English mode (Idioma / Language).
Same-Wi-Fi multiplayer is **experimental**: one phone taps *Create room*, the other types the code and taps
*Join*, then both start an Exhibition. License: GPL-2.0.
