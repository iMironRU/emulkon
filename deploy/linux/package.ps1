# package.ps1 — пакеты сервера Эмулкон для Linux x86_64: emulkon_<версия>_amd64.deb и emulkon-<версия>-1.x86_64.rpm.
# Работает на Windows и Linux (go и nfpm кросс-платформенные): сервер собирается под linux/amd64 без cgo, в
# /usr/share/emulkon/driver кладутся пакет драйвера и библиотеки для самообновления из dist\downloads.
#
#   deploy\linux\package.ps1                 # после .\build.ps1 (нужны dist\downloads) → dist\linux\
#   deploy\linux\package.ps1 -NoDriver       # без драйвера: только сервер (проверка упаковки)
#
# Нужен go в PATH (под C:\.dev — из .tools); nfpm скачивается go run с зафиксированной версией.

param(
    [string]$Out = (Join-Path $PSScriptRoot "..\..\dist\linux"),
    [switch]$NoDriver
)

$ErrorActionPreference = "Stop"
# go пишет ход работы в stderr: в Windows PowerShell 5.1 это не ошибка — итог по $LASTEXITCODE.
function Invoke-Native([string]$Exe) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Exe @args 2>&1 | ForEach-Object { "$_" } } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($args[0]): код $LASTEXITCODE" }
}
$nfpm = "github.com/goreleaser/nfpm/v2/cmd/nfpm@v2.41.1"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$version = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
if (-not (Get-Command go -ErrorAction SilentlyContinue) -and (Test-Path 'C:\.dev\.tools\activate.ps1')) { . 'C:\.dev\.tools\activate.ps1' | Out-Null }

$stage = Join-Path ([IO.Path]::GetTempPath()) ("emulkon-linux-pkg-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force (Join-Path $stage "bin"), (Join-Path $stage "driver"), $Out | Out-Null
try {
    # Сервер: linux/amd64, статически (без cgo), версия — из VERSION.
    $env:GOOS = "linux"; $env:GOARCH = "amd64"; $env:CGO_ENABLED = "0"
    Push-Location (Join-Path $root "server")
    try {
        Invoke-Native go build -trimpath -ldflags "-s -w -X main.version=$version" -o (Join-Path $stage "bin/emulkon") ./cmd/emulkon
    } finally {
        Pop-Location
        Remove-Item Env:GOOS, Env:GOARCH, Env:CGO_ENABLED -ErrorAction SilentlyContinue
    }

    # Драйвер для касс: пакет для БПО и библиотеки самообновления (Windows и Linux) — как на странице /downloads.
    if (-not $NoDriver) {
        $dl = Join-Path $root "dist\downloads"
        $files = @("EmulkonDriver_$version.zip", "EmulkonDriver_${version}_win64.dll", "EmulkonDriver_${version}_win32.dll",
            "EmulkonDriver_${version}_lin64.so")
        foreach ($f in $files) {
            $p = Join-Path $dl $f
            if (-not (Test-Path $p)) { throw "нет $p — сначала .\build.ps1 -LinuxDriver <.so> (или -NoDriver)" }
            Copy-Item $p (Join-Path $stage "driver")
        }
    } else {
        Set-Content (Join-Path $stage "driver\README") "Пакет собран без драйвера (package.ps1 -NoDriver)." -Encoding utf8
    }

    # nfpm подставляет переменные не во все поля (в пути файлов — нет): готовая конфигурация — во временном каталоге.
    $config = Join-Path $stage "nfpm.yaml"
    $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "nfpm.yaml"))
    $text = $text.Replace('${EMULKON_VERSION}', $version).Replace('${EMULKON_STAGE}', $stage.Replace('\', '/'))
    [IO.File]::WriteAllText($config, $text, (New-Object Text.UTF8Encoding $false))
    Push-Location $root
    try {
        foreach ($fmt in "deb", "rpm") {
            Invoke-Native go run $nfpm package --config $config --packager $fmt --target $Out
        }
    } finally {
        Pop-Location
    }
    Get-ChildItem $Out -Filter "emulkon*" | ForEach-Object { "{0}  {1:N1} МБ" -f $_.Name, ($_.Length / 1MB) }
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
