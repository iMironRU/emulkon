# live.ps1 — живой тест оснастки: отдельный сервер с временными данными и токенами, запросы консоли к нему.
# Рабочую службу Emulkon не трогает: свой порт, свой каталог данных, адрес консоли — EMULKON_CONSOLE_URL.
#
#   .\live.ps1 -Exe <console_live.exe> [-Server <emulkon.exe>] [-Port 8799]
#
# Без -Server берёт dist\emulkon.exe, а нет его — собирает сервер во временный каталог (go из C:\.dev\.tools).
# Сохранять в UTF-8 with BOM.

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Server = (Join-Path $PSScriptRoot "..\..\dist\emulkon.exe"),
    [int]$Port = 8799
)

$ErrorActionPreference = "Stop"
$tmp = Join-Path $env:TEMP ("emulkon-console-live-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null

if (-not (Test-Path -LiteralPath $Server)) {
    if (Test-Path 'C:\.dev\.tools\activate.ps1') { . 'C:\.dev\.tools\activate.ps1' | Out-Null }  # иначе go — из PATH
    $Server = Join-Path $tmp "emulkon.exe"
    Push-Location (Join-Path $PSScriptRoot "..\..\server")
    try { go build -o $Server ./cmd/emulkon; if ($LASTEXITCODE -ne 0) { throw "go build: код $LASTEXITCODE" } } finally { Pop-Location }
}

# Токены — случайные на каждый прогон; задаются окружением: оно важнее реестра службы.
$control = "live-ctl-" + [guid]::NewGuid().ToString("N")
$device = "live-dev-" + [guid]::NewGuid().ToString("N")
$url = "http://127.0.0.1:$Port"
$proc = $null
$saved = @{}
foreach ($k in "EMULKON_CONTROL_TOKEN", "EMULKON_DEVICE_TOKEN", "EMULKON_CONSOLE_URL", "LIVE_CONTROL_TOKEN", "LIVE_DEVICE_TOKEN") {
    $saved[$k] = [Environment]::GetEnvironmentVariable($k, "Process")
}
try {
    if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) { throw "порт $Port занят" }
    $env:EMULKON_CONTROL_TOKEN = $control
    $env:EMULKON_DEVICE_TOKEN = $device
    $proc = Start-Process -FilePath $Server -ArgumentList "-addr", "127.0.0.1:$Port", "-data", "`"$tmp\data`"", "-downloads", "`"$tmp`"" `
        -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $tmp "server.err") -RedirectStandardOutput (Join-Path $tmp "server.out")
    $ready = $false
    for ($i = 0; $i -lt 50 -and -not $ready; $i++) {
        Start-Sleep -Milliseconds 200
        try { $ready = (Invoke-WebRequest "$url/api/v1/health" -UseBasicParsing -TimeoutSec 2).StatusCode -eq 200 } catch { }
    }
    if (-not $ready) { throw "сервер не поднялся: $(Get-Content (Join-Path $tmp 'server.err') -Raw)" }

    $env:EMULKON_CONTROL_TOKEN = $null
    $env:EMULKON_DEVICE_TOKEN = $null
    $env:EMULKON_CONSOLE_URL = $url
    $env:LIVE_CONTROL_TOKEN = $control
    $env:LIVE_DEVICE_TOKEN = $device
    & $Exe
    if ($LASTEXITCODE -ne 0) { throw "живой тест оснастки: код $LASTEXITCODE" }
} finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], "Process") }
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; $proc.WaitForExit(5000) | Out-Null }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
