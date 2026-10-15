# build.ps1 — сборка оснастки MMC Эмулкона (Windows x64, clang-cl, ABI MSVC), тесты, DLL в console\dist.
#
#   .\build.ps1            DLL, модульные тесты и живой тест против отдельного сервера (tests\live.ps1)
#   .\build.ps1 -NoLive    без живого теста
#
# Сохранять в UTF-8 with BOM.

param(
    [switch]$NoLive
)

$ErrorActionPreference = "Stop"
# Окружение: локально — портативный тулчейн C:\.dev\.tools; в CI — инструменты раннера (.github/workflows).
if (Test-Path 'C:\.dev\.tools\activate.ps1') { . 'C:\.dev\.tools\activate.ps1' | Out-Null }

$src = $PSScriptRoot
if (-not $env:DEV_CMAKE_TOOLCHAINS) { throw "Нет toolchain-файлов CMake: задайте DEV_CMAKE_TOOLCHAINS (README, «Сборка»)" }
$bin = Join-Path $env:LOCALAPPDATA "emulkon-build\console"   # латиницей: часть инструментов не любит не-ASCII пути

function Invoke-Step {
    param([string]$What, [scriptblock]$Command)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Command 2>&1 | ForEach-Object { Write-Host "  $_" } } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "${What}: код возврата $LASTEXITCODE" }
}

Invoke-Step "cmake configure" { cmake -S $src -B $bin -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$env:DEV_CMAKE_TOOLCHAINS\clang-cl-x64.cmake" }
Invoke-Step "cmake build" { cmake --build $bin }
Invoke-Step "ctest" { ctest --test-dir $bin --output-on-failure }
if (-not $NoLive) {
    Write-Host "  живой тест: отдельный сервер, рабочая служба не трогается"
    & (Join-Path $src "tests\live.ps1") -Exe (Join-Path $bin "console_live.exe")
}

$dist = Join-Path $src "dist"
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $bin "EmulkonSnapIn.dll") $dist -Force
Write-Host "Оснастка: $(Join-Path $dist 'EmulkonSnapIn.dll')" -ForegroundColor Green
