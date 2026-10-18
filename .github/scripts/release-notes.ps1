# release-notes.ps1 — раздел версии из CHANGELOG.md для описания релиза GitHub.
#
#   .github\scripts\release-notes.ps1 -Version 0.8.0 > notes.md

param([Parameter(Mandatory = $true)][string]$Version, [switch]$ChangesOnly)  # ChangesOnly — без строки об установке (latest.json)

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
if ($ChangesOnly) { return }
""
"**Установка:** скачайте ``EmulkonSetup_$Version.exe`` и запустите (нужны права администратора). Пакет драйвера для"
"«Драйверов оборудования» БПО — ``EmulkonDriver_$Version.zip``. Проверьте файлы по ``SHA256SUMS``."
""
"> **Подписи кода пока нет.** SmartScreen может предупредить о неизвестном издателе, а антивирус (в т. ч. Microsoft"
"> Defender) — пометить установщик или забрать его в карантин. Это ложное срабатывание (эмулятор тестового"
"> оборудования, [issue #20](https://github.com/iMironRU/emulkon/issues/20)): сверьте ``SHA256SUMS``, восстановите"
"> из карантина и разрешите. Срабатывание облачное и непостоянное."
""
"**Сервер для Linux** (x86_64, systemd): ``sudo apt install ./emulkon_$Version-1_amd64.deb`` или"
"``sudo dnf install ./emulkon-$Version-1.x86_64.rpm`` — служба ``emulkon``, веб-интерфейс http://127.0.0.1:8737/."
""
"**Docker** (общий сервер для команды): ``docker run -d -p 8737:8737 -e EMULKON_DEVICE_TOKEN=… -e EMULKON_CONTROL_TOKEN=… -v emulkon-data:/data ghcr.io/imironru/emulkon:$Version``"
