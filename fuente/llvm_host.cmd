@echo off
rem Herramientas de LLVM para Windows (llvm-tblgen, clang-tblgen) que hacen falta para compilar clang/lld para Android.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set H=%~dp0..\herramientas
set PATH=%H%\sdk\cmake\3.22.1\bin;%PATH%
cmake -G Ninja -S "%H%\llvm_src\llvm-project-19.1.7.src\llvm" -B "%H%\llvm_host" ^
  -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=AArch64 ^
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_DOCS=OFF -DLLVM_INCLUDE_UTILS=ON || exit /b 1
cmake --build "%H%\llvm_host" --target llvm-tblgen clang-tblgen || exit /b 1
echo HECHO

