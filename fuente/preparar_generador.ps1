# Mete en la app lo que hace falta para generar libjuego.so DENTRO del teléfono (PreparacionActivity):
#   app/app/libs/arm64-v8a/  libarmrecomp.so, libpartir.so, libclang.so, liblld.so  (ejecutables de Android)
#   app/app/src/main/assets/generador/  db (NIDs), include (clang, shim, vitarecomp), juego_base_android.c, libjuego.map
# Nada del juego: solo herramientas y cabeceras.
#   .\preparar_generador.ps1 [-VitaRE <carpeta del port de PC>]
# Antes: llvm_host.cmd y llvm_android.cmd (clang + lld para Android) y sincronizar_desde_pc.ps1.
param([string]$VitaRE = $env:VITARE)
$ErrorActionPreference = 'Stop'
$F = $PSScriptRoot
$A = Split-Path $F
if (-not $VitaRE) { $VitaRE = Join-Path (Split-Path $A) 'VitaRE' }
$H = "$A\herramientas"
$NDK = (Get-ChildItem "$H\sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$CC = "$NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
$LIBS = "$A\app\app\libs\arm64-v8a"
$AS = "$A\app\app\src\main\assets\generador"
New-Item -ItemType Directory -Force $LIBS | Out-Null
if (Test-Path $AS) { Remove-Item $AS -Recurse -Force }
New-Item -ItemType Directory -Force $AS | Out-Null

# 1. armrecomp y partir para Android (desde el código)
$ar = "$F\port\vitarecomp\tools\armrecomp"
$fs = 'main','container','inflate','decode','module','analyze','emit','nids' | ForEach-Object { "$ar\$_.c" }
& $CC --target=aarch64-linux-android29 -O2 -fPIE -pie -w -s -I $ar -I "$F\port\vitarecomp\include" @fs -o "$LIBS\libarmrecomp.so" -lm
if ($LASTEXITCODE) { throw 'falló armrecomp' }
& $CC --target=aarch64-linux-android29 -O2 -fPIE -pie -w -s "$F\android\movil\partir.c" -o "$LIBS\libpartir.so"
if ($LASTEXITCODE) { throw 'falló partir' }

# 2. clang y lld para Android (llvm_android.cmd)
$lb = "$H\llvm_android\bin"
$clang = @('clang-19', 'clang') | ForEach-Object { Join-Path $lb $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
$lld = @('lld') | ForEach-Object { Join-Path $lb $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $clang -or -not $lld) { throw "faltan clang/lld para Android en $lb (llvm_android.cmd)" }
Copy-Item $clang "$LIBS\libclang.so" -Force
Copy-Item $lld "$LIBS\liblld.so" -Force

# 3. datos: NIDs, cabeceras, base del juego
$PCH = "$VitaRE\mlb15\recomp\herramientas"
Copy-Item "$PCH\db" "$AS\db" -Recurse
New-Item -ItemType Directory -Force "$AS\include" | Out-Null
Copy-Item "$PCH\include\shim" "$AS\include\shim" -Recurse
Copy-Item "$F\port\vitarecomp\include\vitarecomp" "$AS\include\vitarecomp" -Recurse   # la misma versión que libmain.so
# cabeceras propias de clang: las del clang 19 compilado, sin las de x86 / otras arquitecturas
$ch = "$H\llvm_android\lib\clang\19\include"
if (-not (Test-Path $ch)) { $ch = "$PCH\include\clang" }
New-Item -ItemType Directory -Force "$AS\include\clang" | Out-Null
Get-ChildItem $ch -File | Where-Object { $_.Name -notmatch '(?i)^(amx|avx|sse|mm|x86|xmm|emm|pmm|tmm|smm|nmm|wmm|f16c|fma|bmi|lzcnt|popcnt|rtm|sha|xsave|xop|ia32|immintrin|x86gprintrin|cet|clflush|clwb|clzero|cldemote|enqcmd|gfni|hreset|invpcid|keylocker|movdir|mwaitx|pconfig|pku|prfchw|ptwrite|rdpid|rdseed|rdrand|serialize|sgx|tsxldtrk|uintr|vaes|vpclmul|waitpkg|wbnoinvd|adx|cpuid|cmpccxadd|raoint|prfchi|usermsr|altivec|htm|vec|s390|riscv|sifive|hexagon|hvx|velintrin|msa|lasx|lsx|larch|mips|ppc|power|cuda|__clang_cuda|__clang_hip|opencl|hlsl)' } | Copy-Item -Destination "$AS\include\clang"
Copy-Item "$F\android\juego_base_android.c" "$AS\juego_base_android.c"
Copy-Item "$F\android\libjuego.map" "$AS\libjuego.map"

Get-ChildItem $LIBS | ForEach-Object { "{0,-18} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB) }
"assets generador: {0:N1} MB" -f ((Get-ChildItem $AS -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
