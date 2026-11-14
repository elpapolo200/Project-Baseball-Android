cmake_minimum_required(VERSION 3.6)
project(PROJECT_BASEBALL C CXX)

# libmain.so: el port de Project Baseball (fuente\port, copia del PC) + lo propio de Android
# (fuente\android). SDLActivity carga "SDL2" y "main"; libjuego.so (el juego traducido) se carga
# después con dlopen y usa las funciones de libmain.so.
set(FUENTE ${CMAKE_SOURCE_DIR}/../../../fuente)
set(PORT ${FUENTE}/port)
set(AND ${FUENTE}/android)

# --- port (C) ---
file(GLOB HOST_C ${PORT}/host/*.c)
foreach(fuera ui_estilo opciones texturas_hd recompilar watchdog shc vk_comprobar launcher hle_dialog hle_savedialog)
    list(REMOVE_ITEM HOST_C ${PORT}/host/${fuera}.c)
endforeach()
file(GLOB RT_C ${PORT}/vitarecomp/src/*.c)
file(GLOB AT9_C ${PORT}/deps/LibAtrac9/*.c)
set(ANDROID_C ${AND}/compat_win.c ${AND}/dialogos_android.c ${AND}/launcher_android.c ${AND}/shc_android.c
              ${AND}/stubs_android.c ${AND}/arranque_android.c ${AND}/perfil_android.c ${AND}/adpf_android.c)

# --- traductor de shaders de Vita3K (C++) + SpvBuilder de glslang (Vita3K) ---
file(GLOB V3K_CPP ${PORT}/v3k/src/*.cpp ${PORT}/v3k/src/shader/*.cpp ${PORT}/v3k/src/shader/translator/*.cpp ${PORT}/v3k/src/gxm/*.cpp)
set(GL ${PORT}/deps/glslang-v3k)
set(SPV_CPP ${GL}/SPIRV/SpvBuilder.cpp ${GL}/SPIRV/SpvPostProcess.cpp ${GL}/SPIRV/InReadableOrder.cpp
            ${GL}/SPIRV/disassemble.cpp ${GL}/SPIRV/doc.cpp ${GL}/SPIRV/Logger.cpp)

add_library(main SHARED ${HOST_C} ${RT_C} ${AT9_C} ${ANDROID_C} ${V3K_CPP} ${SPV_CPP})

target_include_directories(main PRIVATE
    ${AND}/include                 # windows.h propio: antes que nada
    ${PORT}/vitarecomp/include ${PORT}/host ${PORT}/host/shaders ${PORT}/deps/LibAtrac9
    ${PORT}/v3k/include ${PORT}/v3k/src ${GL} ${GL}/SPIRV ${PORT}/deps/fmt/include
    ${CMAKE_SOURCE_DIR}/SDL/include)
target_compile_definitions(main PRIVATE VITA_CPU_REMOTA _GNU_SOURCE spv=v3kspv FMT_HEADER_ONLY NOMINMAX)
target_compile_options(main PRIVATE -O2 -w -fno-strict-aliasing
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20 -fexceptions -frtti>)
set_source_files_properties(${PORT}/host/main.c PROPERTIES COMPILE_DEFINITIONS main=port_main)
set_source_files_properties(${PORT}/host/gxm.c PROPERTIES COMPILE_OPTIONS "-include;${AND}/pre/gxm_pre.h")
# los #pragma comment(lib, "user32.lib") de los fuentes de PC no significan nada aquí
target_link_options(main PRIVATE -Wl,--no-dependent-libraries)
find_package(games-frame-pacing REQUIRED CONFIG)
target_link_libraries(main SDL2 vulkan log android m dl games-frame-pacing::swappy_static)
