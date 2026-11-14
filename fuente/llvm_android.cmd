@echo off
rem clang + lld (LLVM 19.1.7) que se ejecutan EN Android ARM64, para generar libjuego.so dentro del tel??fono.
rem Necesita antes llvm_host.cmd (llvm-tblgen y clang-tblgen para Windows).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set H=%~dp0..\herramientas
set NDK=%H%\sdk\ndk\27.2.12479018
set PATH=%H%\sdk\cmake\3.22.1\bin;%PATH%
cmake -G Ninja -S "%H%\llvm_src\llvm-project-19.1.7.src\llvm" -B "%H%\llvm_android" ^
  -DCMAKE_TOOLCHAIN_FILE="%NDK%\build\cmake\android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 ^
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release ^
  -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64 ^
  -DLLVM_HOST_TRIPLE=aarch64-unknown-linux-android29 -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-unknown-linux-android29 ^
  -DLLVM_TABLEGEN="%H%\llvm_host\bin\llvm-tblgen.exe" -DCLANG_TABLEGEN="%H%\llvm_host\bin\clang-tblgen.exe" -DLLVM_NATIVE_TOOL_DIR="%H%\llvm_host\bin" ^
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_UTILS=ON -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_DOCS=OFF ^
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF ^
  -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_PLUGIN_SUPPORT=OFF -DLLVM_ENABLE_PLUGINS=OFF ^
  -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" ^
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections -s" || exit /b 1
cmake --build "%H%\llvm_android" --target clang lld || exit /b 1
echo HECHO
