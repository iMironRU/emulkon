# release-notes.ps1 — раздел версии из CHANGELOG.md для описания релиза GitHub.
#
#   .github\scripts\release-notes.ps1 -Version 0.8.0 > notes.md

param([Parameter(Mandatory = $true)][string]$Version)

$ErrorActionPreference = "Stop"
$lines = Get-Content (Join-Path $PSScriptRoot "..\..\CHANGELOG.md") -Encoding UTF8
$out = @()
$inside = $false
foreach ($l in $lines) {
    if ($l -match '^## \[') {
        if ($inside) { break }
        $inside = $l -match "^## \[$([regex]::Escape($Version))\]"
        continue
    }
    if ($inside) { $out += $l }
}
if (-not $inside -and -not $out) { throw "В CHANGELOG.md нет раздела [$Version]" }
($out -join "`n").Trim()
""
"**Установка:** скачайте ``EmulkonSetup_$Version.exe`` и запустите (нужны права администратора). Подписи кода пока нет —"
"проверьте файл по ``SHA256SUMS``. Пакет драйвера для «Драйверов оборудования» БПО — ``EmulkonDriver_$Version.zip``."
