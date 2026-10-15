# build.ps1 — сборка трея Эмулкон (Windows x64, clang-cl, ABI MSVC), тесты, exe в tray\dist.
#
#   .\build.ps1
#
# Сохранять в UTF-8 with BOM.

$ErrorActionPreference = "Stop"
# Окружение: локально — портативный тулчейн C:\.dev\.tools; в CI — инструменты раннера (.github/workflows).
if (Test-Path 'C:\.dev\.tools\activate.ps1') { . 'C:\.dev\.tools\activate.ps1' | Out-Null }

$src = $PSScriptRoot
if (-not $env:DEV_CMAKE_TOOLCHAINS) { throw "Нет toolchain-файлов CMake: задайте DEV_CMAKE_TOOLCHAINS (README, «Сборка»)" }
$bin = Join-Path $env:LOCALAPPDATA "emulkon-build\tray"   # латиницей: часть инструментов не любит не-ASCII пути

function Invoke-Step {
    param([string]$What, [scriptblock]$Command)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Command 2>&1 | ForEach-Object { Write-Host "  $_" } } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "${What}: код возврата $LASTEXITCODE" }
}

# Запущенный трей сборки держит exe — закрыть его перед сборкой. Только его: «--exit» закрывает
# любой запущенный трей, а установленный трей exe сборки не держит, и с ним остановился бы сервер.
$exe = Join-Path $bin "EmulkonTray.exe"
$running = Get-Process EmulkonTray -ErrorAction SilentlyContinue
if ($running | Where-Object { $_.Path -eq $exe }) {
    if ($running | Where-Object { $_.Path -ne $exe }) {
        Get-Process EmulkonTray | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
    } else {
        & $exe --exit
    }
    Start-Sleep -Seconds 2
}

Invoke-Step "cmake configure" { cmake -S $src -B $bin -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$env:DEV_CMAKE_TOOLCHAINS\clang-cl-x64.cmake" }
Invoke-Step "cmake build" { cmake --build $bin }
Invoke-Step "ctest" { ctest --test-dir $bin --output-on-failure }

$dist = Join-Path $src "dist"
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $exe $dist -Force
Write-Host "Трей: $(Join-Path $dist 'EmulkonTray.exe')" -ForegroundColor Green
