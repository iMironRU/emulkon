# Отключает «Защиту от опасных действий» 1С (предупреждение при открытии внешних обработок)
# для тестовых баз Эмулкона (строка соединения содержит «EmulkonTest») или, с -AllBases, для всех баз.
#
# Механизм платформы: параметр DisableUnsafeActionProtection в conf.cfg — регулярное выражение по
# строке соединения; совпала — предупреждений нет. Пишется в conf.cfg каждой установленной версии
# (C:\Program Files\1cv8\<версия>\bin\conf\conf.cfg). Нужны права администратора.
#
#   tests\1c\disable-unsafe-protection.ps1           # показать, что изменится, и применить (запрос UAC)
#   tests\1c\disable-unsafe-protection.ps1 -AllBases # для ВСЕХ баз (шаблон .*), не только тестовых
#   tests\1c\disable-unsafe-protection.ps1 -WhatIf   # только показать
#   tests\1c\disable-unsafe-protection.ps1 -Undo     # убрать строку, вернуть как было
#
# Копия файла до изменения — conf.cfg.bak-emulkon рядом с ним.

param([switch]$WhatIf, [switch]$Undo, [switch]$AllBases)

# .* — любая строка соединения: защита отключается во всех базах на этом компьютере.
$pattern = if ($AllBases) { ".*" } else { ".*EmulkonTest.*" }
$key = "DisableUnsafeActionProtection"
$files = Get-ChildItem "C:\Program Files\1cv8", "C:\Program Files (x86)\1cv8" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "bin\1cv8.exe") } |
    ForEach-Object { Join-Path $_.FullName "bin\conf\conf.cfg" }
if (-not $files) { Write-Host "Платформа 1С не найдена в Program Files"; exit 1 }

# План: что будет в каждом файле.
$plan = foreach ($f in $files) {
    $lines = if (Test-Path $f) { Get-Content $f -Encoding UTF8 } else { @() }
    $rest = $lines | Where-Object { $_ -notmatch "^\s*$key\s*=" }
    $old = $lines | Where-Object { $_ -match "^\s*$key\s*=" }
    if ($Undo) {
        $new = $rest
    } else {
        # Уже заданные шаблоны сохраняем, свой добавляем через «;».
        $values = @($old | ForEach-Object { ($_ -split "=", 2)[1].Trim() } | ForEach-Object { $_ -split ";" } | Where-Object { $_ })
        if ($values -notcontains $pattern) { $values += $pattern }
        $new = @($rest) + "$key=$($values -join ';')"
    }
    [pscustomobject]@{ File = $f; Old = ($old -join " | "); New = (($new | Where-Object { $_ -match "^\s*$key\s*=" }) -join " | "); Lines = $new }
}
$plan | ForEach-Object { Write-Host "$($_.File)`n   было:  $(if ($_.Old) { $_.Old } else { '(нет)' })`n   будет: $(if ($_.New) { $_.New } else { '(нет)' })" }
if ($WhatIf) { exit 0 }

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    # Перезапуск с правами администратора: Windows спросит подтверждение (UAC).
    $args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    if ($Undo) { $args += "-Undo" }
    if ($AllBases) { $args += "-AllBases" }
    $p = Start-Process powershell -Verb RunAs -ArgumentList $args -PassThru -Wait
    exit $p.ExitCode
}

foreach ($item in $plan) {
    $dir = Split-Path $item.File
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory $dir | Out-Null }
    if ((Test-Path $item.File) -and -not (Test-Path "$($item.File).bak-emulkon")) { Copy-Item $item.File "$($item.File).bak-emulkon" }
    [IO.File]::WriteAllLines($item.File, [string[]]$item.Lines, (New-Object Text.UTF8Encoding $true))
    Write-Host "записано: $($item.File)"
}
Write-Host "Готово. Перезапустите 1С."
Start-Sleep 3
