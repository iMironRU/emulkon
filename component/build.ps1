# build.ps1 — сборка компоненты-драйвера Эмулкон под Windows (clang-cl, ABI MSVC) и упаковка ZIP для БПО.
#
#   .\build.ps1                       # x64 и x86, тесты без сервера, ZIP в component\dist
#   .\build.ps1 -Arch x64             # только x64
#   .\build.ps1 -Server http://127.0.0.1:8737   # плюс сценарий ККТ на живом сервере
#   .\build.ps1 -LinuxDriver <libEmulkonDriver_lin64.so>   # плюс драйвер Linux в пакете (его собирает CI на Linux)
#
# Тулчейн — clang-cl (README, «Сборка»). Сохранять в UTF-8 with BOM.

param(
    [ValidateSet("x64", "x86", "all")]
    [string]$Arch = "all",
    [string]$Server = "",
    [string]$LinuxDriver = "",
    # Выпуск: имена библиотек в пакете — только с версией. Иначе к имени добавляется начало SHA-256 библиотеки (#10).
    [switch]$Release
)

$ErrorActionPreference = "Stop"
# Окружение: локально — портативный тулчейн C:\.dev\.tools; в CI — инструменты раннера (.github/workflows).
if (Test-Path 'C:\.dev\.tools\activate.ps1') { . 'C:\.dev\.tools\activate.ps1' | Out-Null }

$src = $PSScriptRoot
$toolchains = $env:DEV_CMAKE_TOOLCHAINS
if (-not $toolchains -or -not (Test-Path $toolchains)) { throw "Нет toolchain-файлов CMake: задайте DEV_CMAKE_TOOLCHAINS (README, «Сборка»)" }

# Каталог сборки — латиницей: часть инструментов не любит не-ASCII пути.
$buildRoot = Join-Path $env:LOCALAPPDATA "emulkon-build"

function Invoke-Step {
    param([string]$What, [scriptblock]$Command)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Command 2>&1 | ForEach-Object { Write-Host "  $_" } } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "${What}: код возврата $LASTEXITCODE" }
}

$archs = if ($Arch -eq "all") { @("x64", "x86") } else { @($Arch) }
$dlls = @()
foreach ($a in $archs) {
    $bin = Join-Path $buildRoot $a
    Write-Host "== $a" -ForegroundColor Cyan
    Invoke-Step "cmake configure" { cmake -S $src -B $bin -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$toolchains\clang-cl-$a.cmake" }
    Invoke-Step "cmake build" { cmake --build $bin }
    if ($Server) { $env:EMULKON_TEST_SERVER = $Server } else { Remove-Item Env:EMULKON_TEST_SERVER -ErrorAction SilentlyContinue }
    if ($Server) { Invoke-Step "cmake reconfigure" { cmake $bin } }
    Invoke-Step "ctest" { ctest --test-dir $bin --output-on-failure }
    $dlls += Get-ChildItem $bin -Filter "EmulkonDriver_win*.dll"
}

# Драйвер Linux собирается на Linux (CI) и только упаковывается здесь вместе с DLL Windows.
if ($LinuxDriver) {
    if (-not (Test-Path -LiteralPath $LinuxDriver)) { throw "Нет драйвера Linux: $LinuxDriver" }
    $dlls += Get-Item -LiteralPath $LinuxDriver
}

# --- пакет драйвера для БПО: INFO.XML + MANIFEST.XML + DLL Windows и .so Linux ---
$version = (Get-Content -LiteralPath (Join-Path $src "..\VERSION") -TotalCount 1).Trim()
$dist = Join-Path $src "dist"
$stage = Join-Path $buildRoot "package"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage, $dist | Out-Null

$utf8 = New-Object Text.UTF8Encoding $false
$info = @"
<?xml version="1.0" encoding="UTF-8"?>
<drivers>
	<component progid="AddIn.EmulkonDriver" type="ККТ,ЭквайринговыйТерминал,ЭлектронныеВесы,ВесыСПечатьюЭтикеток,ПринтерЧеков,ПринтерЭтикеток,СканерШтрихкода,СчитывательМагнитныхКарт,ДисплейПокупателя,ТерминалСбораДанных,СчитывательRFID,УстройствоРаспознавания" name="ЭМУЛЯТОР Эмулкон $version (imiron.ru)" version="$version"/>
</drivers>
"@
# Версия — в имени файла DLL внутри пакета. Тонкий клиент 1С помнит установленные компоненты по
# имени файла (%APPDATA%\1C\1cv8\ExtCompT\registry.xml) и DLL с тем же именем повторно не ставит:
# после «Обновить драйвер» в БПО касса работала бы со старой DLL (прогон в Рознице, 2026-10-09).
# Не выпуск — ещё и начало SHA-256 библиотеки: новая сборка той же версии получает новое имя и ставится, та же
# библиотека — то же имя (#10). Выпуск (-Release, release.yml) — имя только с версией.
$packaged = @{}
foreach ($d in $dlls) {
    $tag = $version -replace '\.', '_'
    if (-not $Release) { $tag += "_" + (Get-FileHash -LiteralPath $d.FullName -Algorithm SHA256).Hash.Substring(0, 8).ToLower() }
    $packaged[$d.FullName] = $d.Name -replace '^(lib)?EmulkonDriver_', ('${1}EmulkonDriver_' + $tag + '_')
}
$components = foreach ($d in $dlls) {
    $cpu = if ($d.Name -like "*64*") { "x86_64" } else { "i386" }
    $os = if ($d.Name -like "*_lin*") { "Linux" } else { "Windows" }
    "  <component os=`"$os`" path=`"$($packaged[$d.FullName])`" type=`"native`" arch=`"$cpu`"/>"
}
$manifest = @"
<?xml version="1.0" encoding="UTF-8"?>
<bundle xmlns="http://v8.1c.ru/8.2/addin/bundle" name="EmulkonDriver">
$($components -join "`r`n")
</bundle>
"@
[IO.File]::WriteAllText((Join-Path $stage "INFO.XML"), $info, $utf8)
[IO.File]::WriteAllText((Join-Path $stage "MANIFEST.XML"), $manifest, $utf8)
foreach ($d in $dlls) { Copy-Item $d.FullName (Join-Path $stage $packaged[$d.FullName]) }

$zip = Join-Path $dist "EmulkonDriver_$version.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "Пакет драйвера: $zip" -ForegroundColor Green
