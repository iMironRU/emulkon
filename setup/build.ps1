# build.ps1 — сборка установщика Эмулкона (Windows x64, clang-cl, ABI MSVC).
#
#   .\build.ps1 [-Dist <каталог dist>]
#
# Берёт уже собранные сервер, трей и драйвер из dist (их раскладывает build.ps1 в корне),
# зашивает их в EmulkonSetup.exe и кладёт его в dist и dist\downloads. Сохранять в UTF-8 with BOM.

param(
    [string]$Dist = (Join-Path $PSScriptRoot "..\dist")
)

$ErrorActionPreference = "Stop"
# Окружение: локально — портативный тулчейн C:\.dev\.tools; в CI — инструменты раннера (.github/workflows).
if (Test-Path 'C:\.dev\.tools\activate.ps1') { . 'C:\.dev\.tools\activate.ps1' | Out-Null }

$src = $PSScriptRoot
$Dist = (Resolve-Path $Dist).Path
$version = (Get-Content -LiteralPath (Join-Path $src "..\VERSION") -TotalCount 1).Trim()
if (-not $env:DEV_CMAKE_TOOLCHAINS) { throw "Нет toolchain-файлов CMake: задайте DEV_CMAKE_TOOLCHAINS (README, «Сборка»)" }

function Invoke-Step {
    param([string]$What, [scriptblock]$Command)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Command 2>&1 | ForEach-Object { Write-Host "  $_" } } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "${What}: код возврата $LASTEXITCODE" }
}

# Полезная нагрузка — в каталог латиницей: resource compiler и не-ASCII пути не дружат.
$buildRoot = Join-Path $env:LOCALAPPDATA "emulkon-build"
$payload = Join-Path $buildRoot "setup-payload"
$bin = Join-Path $buildRoot "setup"
Remove-Item -LiteralPath $payload -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $payload | Out-Null
$files = @(
    (Join-Path $Dist "emulkon.exe"),
    (Join-Path $Dist "EmulkonTray.exe"),
    (Join-Path $Dist "EmulkonSnapIn.dll"),
    (Join-Path $Dist "downloads\EmulkonDriver_$version.zip"),
    (Join-Path $Dist "downloads\EmulkonDriver_${version}_win64.dll"),
    (Join-Path $Dist "downloads\EmulkonDriver_${version}_win32.dll"),
    (Join-Path $src "..\res\emulkon.ico")
)
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { throw "Нет файла для установщика: $f (сначала build.ps1 в корне)" }
    Copy-Item -LiteralPath $f $payload
}

Invoke-Step "cmake configure" { cmake -S $src -B $bin -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$env:DEV_CMAKE_TOOLCHAINS\clang-cl-x64.cmake" "-DPAYLOAD_DIR=$($payload -replace '\\','/')" }
# Ресурсы с полезной нагрузкой пересобираем всегда: ninja не следит за файлами из RCDATA.
Get-ChildItem -LiteralPath (Join-Path $bin "CMakeFiles\EmulkonSetup.dir") -Filter *.res -ErrorAction SilentlyContinue | Remove-Item -Force
Invoke-Step "cmake build" { cmake --build $bin }
Invoke-Step "ctest" { ctest --test-dir $bin --output-on-failure }

$exe = Join-Path $bin "EmulkonSetup.exe"
Copy-Item -LiteralPath $exe (Join-Path $Dist "EmulkonSetup.exe") -Force
Copy-Item -LiteralPath $exe (Join-Path $Dist "downloads\EmulkonSetup_$version.exe") -Force
$mb = [Math]::Round((Get-Item -LiteralPath $exe).Length / 1MB, 1)
Write-Host "Установщик: $(Join-Path $Dist 'downloads')\EmulkonSetup_$version.exe ($mb МБ)" -ForegroundColor Green
