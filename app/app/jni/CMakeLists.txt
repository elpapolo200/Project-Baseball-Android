cmake_minimum_required(VERSION 3.6)

project(GAME)

# Sin la carpeta de quien compila dentro de las .so (__FILE__ de asserts y mensajes): rutas relativas a la raíz
get_filename_component(RAIZ_PB "${CMAKE_SOURCE_DIR}/../../.." ABSOLUTE)
add_compile_options(-ffile-prefix-map=${RAIZ_PB}/= -fmacro-prefix-map=${RAIZ_PB}/=)

# armeabi-v7a requires cpufeatures library
# include(AndroidNdkModules)
# android_ndk_import_module_cpufeatures()


# SDL sources are in a subfolder named "SDL"
add_subdirectory(SDL)

# Compilation of companion libraries
#add_subdirectory(SDL_image)
#add_subdirectory(SDL_mixer)
#add_subdirectory(SDL_ttf)

# Your game and its CMakeLists.txt are in a subfolder named "src"
add_subdirectory(src)

