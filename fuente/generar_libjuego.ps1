# Genera libjuego.so (el juego traducido para ARM64 Android) a partir de un eboot.elf en claro.
# Solo para el teléfono de quien tiene la copia del juego: no se reparte.
#
#   .\generar_libjuego.ps1 [-Elf ruta\eboot.elf] [-Hilos 3]
# Salida: fuente\salida\libjuego.so + eboot.elf  (copiarlos a /storage/emulated/0/ProjectBaseball/)
param([Parameter(Mandatory=$true)][string]$Elf, [string]$VitaRE = $env:VITARE, [int]$Hilos = 3, [int]$MB = 12)
if (-not $VitaRE) { throw 'indica -VitaRE <carpeta del port de PC> (o la variable VITARE)' }
$ErrorActionPreference = 'Stop'
$F = $PSScriptRoot
$A = Split-Path $F
$PCH = "$VitaRE\mlb15\recomp\herramientas"   # armrecomp, base de NIDs, cabeceras (solo lectura)
$LL = "$VitaRE\deps\llvm\bin"                  # clang + lld (LLVM 19, el que reparte el launcher)
$TOOLS = "$VitaRE\tools"                        # split_recomp.py / fix_recomp.py (solo lectura)
$libmain = "$A\app\app\build\intermediates\stripped_native_libs\debug\out\lib\arm64-v8a\libmain.so"
$tmp = "$F\gen"; $sal = "$F\salida"
New-Item -ItemType Directory -Force $tmp, "$tmp\partes", "$tmp\obj", $sal | Out-Null
if (-not (Test-Path $libmain)) { throw "falta libmain.so: compila antes la app (gradle assembleDebug)" }
$t0 = Get-Date

# 1. traducir
$c = "$tmp\juego.c"
Get-ChildItem "$tmp\partes" -File | Remove-Item -Force
& "$PCH\armrecomp.exe" emit $Elf $c 999999 "$PCH\db" | Select-String -Pattern 'overlaps|neon inline|functions|trapped' | ForEach-Object { "  armrecomp: $($_.Line.Trim())" }
if (-not (Test-Path $c)) { throw 'armrecomp no generó el C' }
"traducido: {0:N0} MB en {1:N0} s" -f ((Get-Item $c).Length / 1MB), ((Get-Date) - $t0).TotalSeconds

# 2. partir y corregir (como recompilar.c en PC)
& py -I "$TOOLS\split_recomp.py" $c "$tmp\partes" $MB | Select-Object -Last 1
Remove-Item -LiteralPath $c -Force
$partes = Get-ChildItem "$tmp\partes\*.c"
foreach ($p in $partes) { & py -I "$TOOLS\fix_recomp.py" $p.FullName $p.FullName | Out-Null }
"partes: $($partes.Count)"

# 3. compilar en paralelo
$flags = @('--target=aarch64-linux-android29', '-march=armv8.2-a', '-O2', '-fPIC', '-ffreestanding', '-nostdinc', '-fno-stack-protector',
           '-fno-math-errno', '-ffp-contract=off', '-fasynchronous-unwind-tables', '-w', '-DVITA_MEM_DIRECTA',
           '-isystem', "$PCH\include\clang", '-I', "$PCH\include\shim", '-I', "$PCH\include")
Get-ChildItem "$tmp\obj" -File | Remove-Item -Force
$fuentes = @($partes | Select-Object -ExpandProperty FullName) + @("$F\android\juego_base_android.c")
$activos = @(); $hechos = 0
foreach ($s in $fuentes) {
    while (@($activos | Where-Object { -not $_.HasExited }).Count -ge $Hilos) { Start-Sleep -Milliseconds 500 }
    $o = "$tmp\obj\" + [IO.Path]::GetFileNameWithoutExtension($s) + '.o'
    $extra = if ($s -like '*juego_base_android.c') { @('-fno-builtin', '-I', "$F\port\vitarecomp\include") } else { @() }
    $args = $flags + $extra + @('-c', "`"$s`"", '-o', "`"$o`"")
    $pr = Start-Process "$LL\clang.exe" -ArgumentList $args -NoNewWindow -PassThru -RedirectStandardError "$o.log"
    $null = $pr.Handle   # sin esto PowerShell pierde el código de salida del proceso
    $activos += $pr
}
$activos | ForEach-Object { $_.WaitForExit() }
$malos = @($activos | Where-Object { $_.ExitCode -ne 0 })
if ($malos.Count) { Get-ChildItem "$tmp\obj\*.log" | Where-Object { $_.Length -gt 0 } | Select-Object -First 2 | ForEach-Object { Get-Content $_.FullName -TotalCount 15 }; throw "falló la compilación de $($malos.Count) partes" }
"compilado en {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds

# 4. enlazar: libjuego.so necesita libmain.so (las funciones del port)
$objs = Get-ChildItem "$tmp\obj\*.o" | Select-Object -ExpandProperty FullName
& "$LL\lld.exe" -flavor gnu -shared -o "$sal\libjuego.so" -soname libjuego.so --version-script "$F\android\libjuego.map" `
    -z noexecstack -z max-page-size=16384 --hash-style=gnu --as-needed -O1 $objs $libmain
if ($LASTEXITCODE) { throw 'falló el enlace' }
Copy-Item $Elf "$sal\eboot.elf" -Force
"libjuego.so: {0:N1} MB | total {1:N0} s" -f ((Get-Item "$sal\libjuego.so").Length / 1MB), ((Get-Date) - $t0).TotalSeconds
