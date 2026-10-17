# build.ps1 — полная сборка Эмулкона: сервер, драйвер, трей, оснастка MMC, установщик. Результат — dist\:
#
#   dist\emulkon.exe                              сервер (запуск отсюда раздаёт downloads\)
#   dist\EmulkonTray.exe                          трей
#   dist\EmulkonSnapIn.dll                        оснастка MMC «Эмулкон»
#   dist\EmulkonSetup.exe                         установщик (он же — в downloads\)
#   dist\downloads\EmulkonDriver_<версия>.zip     пакет драйвера для БПО
#   dist\downloads\EmulkonDriver_<версия>_win64.dll, _win32.dll — для самообновления драйвера
#   dist\downloads\EmulkonSetup_<версия>.exe
#
#   .\build.ps1                                   всё с тестами
#   .\build.ps1 -Server http://127.0.0.1:8738     плюс сценарий драйвера на живом сервере
#   .\build.ps1 -Release                          выпуск: имена библиотек в пакете драйвера без хеша сборки
#
# Версия — файл VERSION. Сохранять в UTF-8 with BOM.

param(
    [string]$Server = "",
    # Драйвер Linux (libEmulkonDriver_lin64.so) — собирается на Linux (CI); попадает в пакет, загрузки и установщик.
    [string]$LinuxDriver = "",
    # Выпуск (release.yml): имена библиотек в пакете драйвера — только с версией (component/build.ps1, #10).
    [switch]$Release
)

$ErrorActionPreference = "Stop"
# Окружение: локально — портативный тулчейн C:\.dev\.tools; в CI — инструменты раннера (.github/workflows).
if (Test-Path 'C:\.dev\.tools\activate.ps1') { . 'C:\.dev\.tools\activate.ps1' | Out-Null }

$root = $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root "VERSION") -TotalCount 1).Trim()
$dist = Join-Path $root "dist"
$downloads = Join-Path $dist "downloads"

function Invoke-Step {
    param([string]$What, [scriptblock]$Command)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Command 2>&1 | ForEach-Object { Write-Host "  $_" } } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "${What}: код возврата $LASTEXITCODE" }
}

Write-Host "== Эмулкон $version" -ForegroundColor Cyan
New-Item -ItemType Directory -Force $dist, $downloads | Out-Null
# Файлы прежних версий из downloads убираем: сервер раздаёт самые новые, остальное — мусор.
Get-ChildItem -LiteralPath $downloads -File | Where-Object { $_.Name -notlike "*_$version*" } | Remove-Item -Force

Write-Host "== сервер" -ForegroundColor Cyan
Push-Location (Join-Path $root "server")
try {
    Invoke-Step "go vet" { go vet ./... }
    Invoke-Step "go test" { go test ./... }
    Invoke-Step "go build" { go build -trimpath -ldflags "-s -w -X main.version=$version" -o (Join-Path $dist "emulkon.exe") ./cmd/emulkon }
} finally { Pop-Location }

Write-Host "== драйвер" -ForegroundColor Cyan
& (Join-Path $root "component\build.ps1") -Server $Server -LinuxDriver $LinuxDriver -Release:$Release
$cbuild = Join-Path $env:LOCALAPPDATA "emulkon-build"
Copy-Item -LiteralPath (Join-Path $root "component\dist\EmulkonDriver_$version.zip") $downloads -Force
Copy-Item -LiteralPath (Join-Path $cbuild "x64\EmulkonDriver_win64.dll") (Join-Path $downloads "EmulkonDriver_${version}_win64.dll") -Force
Copy-Item -LiteralPath (Join-Path $cbuild "x86\EmulkonDriver_win32.dll") (Join-Path $downloads "EmulkonDriver_${version}_win32.dll") -Force
if ($LinuxDriver) {
    # Драйвер Linux для самообновления на кассах с Linux (docs/protocol.md §1.5).
    Copy-Item -LiteralPath $LinuxDriver (Join-Path $downloads "EmulkonDriver_${version}_lin64.so") -Force
}

Write-Host "== трей" -ForegroundColor Cyan
& (Join-Path $root "tray\build.ps1")
Copy-Item -LiteralPath (Join-Path $root "tray\dist\EmulkonTray.exe") $dist -Force

Write-Host "== оснастка MMC" -ForegroundColor Cyan
& (Join-Path $root "console\build.ps1")
Copy-Item -LiteralPath (Join-Path $root "console\dist\EmulkonSnapIn.dll") $dist -Force

Write-Host "== установщик" -ForegroundColor Cyan
& (Join-Path $root "setup\build.ps1") -Dist $dist

Write-Host ""
Get-ChildItem -LiteralPath $dist -Recurse -File | ForEach-Object {
    "{0,8:N0} КБ  {1}" -f ($_.Length / 1KB), $_.FullName.Substring($dist.Length + 1)
}
Write-Host "Готово: $dist" -ForegroundColor Green
