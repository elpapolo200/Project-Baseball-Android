# Copia a esta carpeta los fuentes del port de PC (solo lectura del lado de PC).
# Se puede volver a ejecutar para traer arreglos del PC: sobrescribe la copia de fuente\port,
# nunca toca fuente\android (lo propio de Android).
#
#   .\sincronizar_desde_pc.ps1
param([string]$PC = $env:VITARE)
$ErrorActionPreference = 'Stop'
if (-not $PC) { throw 'indica -PC <carpeta del port de PC> (o la variable VITARE)' }
$R = "$PC\mlb15\recomp"
$D = Join-Path $PSScriptRoot 'port'
function Espejo($o, $d, [string[]]$filtros) {
    New-Item -ItemType Directory -Force $d | Out-Null
    $a = @($o, $d) + $filtros + @('/MIR', '/NJH', '/NJS', '/NFL', '/NDL', '/NP', '/XD', 'rt')
    & robocopy @a | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy falló ($LASTEXITCODE): $o" }
}
Espejo "$R\host" "$D\host" @('*.c', '*.h', '*.vert', '*.frag', '*.comp', '*.glsl', '*.inc')
Espejo "$R\dist" "$D\dist" @('*.c', '*.h', '*.def', '*.txt')
Espejo "$R\v3k\src" "$D\v3k\src" @('*.c', '*.h', '*.cpp', '*.hpp', '*.inc')
Espejo "$R\v3k\include" "$D\v3k\include" @('*.h', '*.hpp', '*.inc')
Espejo "$PC\vitarecomp\include" "$D\vitarecomp\include" @('*.h')
Espejo "$PC\vitarecomp\src" "$D\vitarecomp\src" @('*.c', '*.h')
Espejo "$PC\vitarecomp\tools\armrecomp" "$D\vitarecomp\tools\armrecomp" @('*.c', '*.h', 'CMakeLists.txt')
Espejo "$PC\deps\LibAtrac9\C\src" "$D\deps\LibAtrac9" @('*.c', '*.h')
Espejo "$PC\deps\glslang-v3k" "$D\deps\glslang-v3k" @('*.*')
Espejo "$PC\deps\fmt" "$D\deps\fmt" @('*.*')
& py -I "$PSScriptRoot\android\parches\aplicar.py" $D   # parches solo de Android (v3k sin bucles, etc.)
"copiado en $D"
Get-ChildItem $D -Directory | ForEach-Object { "{0,-12} {1,6} archivos" -f $_.Name, (Get-ChildItem $_.FullName -Recurse -File).Count }
