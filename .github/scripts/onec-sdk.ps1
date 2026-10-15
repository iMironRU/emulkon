# onec-sdk.ps1 — SDK Native API 1С для сборки драйвера в CI: заголовки распространяет 1С, в репозиторий они не
# входят и лежат в секрете ONEC_SDK_B64 (zip каталога include в base64). Пишет ONEC_SDK и HAVE_ONEC_SDK в GITHUB_ENV.
# Секрета нет (pull request из форка) — HAVE_ONEC_SDK=0: собираются сервер, трей и консоль, без драйвера.
#
# Подготовить секрет (локально, из каталога include SDK):
#   Compress-Archive <include>\* sdk.zip
#   gh secret set ONEC_SDK_B64 --body ([Convert]::ToBase64String([IO.File]::ReadAllBytes("sdk.zip")))

param([switch]$Required)

$ErrorActionPreference = "Stop"
if (-not $env:ONEC_SDK_B64) {
    if ($Required) { throw "Нет секрета ONEC_SDK_B64: без SDK Native API 1С драйвер не собрать" }
    "HAVE_ONEC_SDK=0" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
    Write-Host "::notice::Секрета ONEC_SDK_B64 нет — драйвер и установщик не собираются"
    return
}
$dir = Join-Path $env:RUNNER_TEMP "onec-sdk"
$zip = Join-Path $env:RUNNER_TEMP "onec-sdk.zip"
# Только символы base64: при записи секрета через конвейер PowerShell 5.1 в него попадают BOM и переводы строк.
$b64 = $env:ONEC_SDK_B64 -replace '[^A-Za-z0-9+/=]', ''
[IO.File]::WriteAllBytes($zip, [Convert]::FromBase64String($b64))
Expand-Archive $zip $dir -Force
Remove-Item $zip
$include = (Get-ChildItem $dir -Recurse -Filter ComponentBase.h | Select-Object -First 1).DirectoryName
if (-not $include) { throw "В секрете ONEC_SDK_B64 нет ComponentBase.h" }
"ONEC_SDK=$include" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
"HAVE_ONEC_SDK=1" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
Write-Host "SDK Native API 1С: $include"
