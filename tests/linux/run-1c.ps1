# run-1c.ps1 — прогон ТестЭмулкона в 1С под Linux (ВМ new-vm.ps1) с этого компьютера; аналог tests\1c\run.ps1.
#
#   tests\linux\run-1c.ps1 -Screen     # только туннель и экран ВМ: http://localhost:6080/vnc.html (человек — лицензия)
#   tests\linux\run-1c.ps1             # прогон: отчёт — tests\linux\build\report-<база>.txt
#
# Туннель SSH: localhost:6080 этого компьютера → noVNC в ВМ; localhost:8737 в ВМ → сервер Эмулкона этого
# компьютера — «Кассы в сети» не нужны, сервер в сеть не открывается. «Оператор у кассы» (карта VISA на терминале,
# 0,777 кг на весах) — как на Windows, отсюда через Control API.

param(
    [string]$Base = "/home/emulkon/bases/EmulkonTest_UNF/db-data",
    [string]$User = "Администратор",
    [string]$Platform = "8.5.1.1529",
    [string]$Workstation = "EMULKON-1C-LINUX",
    [string]$Server = "http://127.0.0.1:8737",
    [string]$Driver = "",
    [string]$Epf = (Join-Path $PSScriptRoot "..\1c\build\ТестЭмулкона.epf"),
    [int]$TimeoutSec = 600,
    [switch]$Screen
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$vm = Join-Path $PSScriptRoot "vm.ps1"
$key = Join-Path $HOME ".ssh\emulkon_vm_ed25519"
$pidFile = Join-Path $env:TEMP "emulkon-vm-tunnel.pid"

# Туннель: один процесс ssh на всё время работы; жив — не трогаем.
$tunnel = $null
if (Test-Path $pidFile) { $tunnel = Get-Process -Id ([int](Get-Content $pidFile)) -ErrorAction SilentlyContinue | Where-Object Name -eq "ssh" }
if (-not $tunnel) {
    $tunnel = Start-Process ssh.exe -WindowStyle Hidden -PassThru -ArgumentList "-i", $key, "-o",
        "UserKnownHostsFile=$(Join-Path $HOME '.ssh\emulkon_vm_known_hosts')", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=30", "-N", "-L", "6080:127.0.0.1:6080", "-R", "8737:127.0.0.1:8737", "emulkon@emulkon-linux.mshome.net"
    Set-Content $pidFile $tunnel.Id
    Start-Sleep 3
    if ($tunnel.HasExited) { throw "туннель SSH не поднялся (порт 6080 занят или ВМ не отвечает)" }
}
& $vm -Put (Join-Path $PSScriptRoot "screen.sh") | Out-Null
& $vm "bash screen.sh > /dev/null"
Write-Host "Экран ВМ: http://localhost:6080/vnc.html?autoconnect=1&resize=scale"
if ($Screen) { return }

if (-not $Driver) { $Driver = Join-Path $root "dist\downloads\EmulkonDriver_$((Get-Content (Join-Path $root 'VERSION') -TotalCount 1).Trim()).zip" }
foreach ($f in $Driver, $Epf) { if (-not (Test-Path $f)) { throw "нет $f" } }
& $vm "mkdir -p ~/emulkon-test" | Out-Null
& $vm "rm -f ~/emulkon-test/*.epf ~/emulkon-test/*.zip" | Out-Null
foreach ($f in $Driver, $Epf, (Join-Path $PSScriptRoot "run-1c.sh")) {
    & $vm -Put $f | Out-Null
    & $vm "mv -f ~/'$(Split-Path $f -Leaf)' ~/emulkon-test/" | Out-Null
}
$dir = "/home/emulkon/emulkon-test"
$report = "$dir/report.txt"
$param = "report=$report;driver=$dir/$(Split-Path $Driver -Leaf);server=$Server;workstation=$Workstation"

# «Оператор у кассы» — отвечает на экраны устройств тестового рабочего места.
$operator = Start-Job -ArgumentList $Server, $Workstation -ScriptBlock {
    param($Server, $Workstation)
    $answered = @{}
    while ($true) {
        try {
            foreach ($d in (Invoke-RestMethod "$Server/control/v1/devices" -TimeoutSec 5).devices) {
                $p = $d.state.prompt
                if ($d.workstation_id -ne $Workstation -or -not $p -or $answered[$p.prompt_id]) { continue }
                $body = switch ($d.type) {
                    "acquiring" { @{ prompt_id = $p.prompt_id; action = "card"; card = "visa" } }
                    "scales" { @{ prompt_id = $p.prompt_id; action = "weigh"; value = 0.777 } }
                }
                if (-not $body) { continue }
                Invoke-RestMethod -Method Post "$Server/control/v1/devices/$($d.device_id)/prompt" -TimeoutSec 5 `
                    -ContentType "application/json" -Body ($body | ConvertTo-Json) | Out-Null
                $answered[$p.prompt_id] = $true
                "оператор: $($d.device_id) $($p.operation) → $($body.action)"
            }
        } catch { "оператор: $($_.Exception.Message)" }
        Start-Sleep -Milliseconds 300
    }
}
try {
    & $vm "bash $dir/run-1c.sh '$Platform' '$Base' '$User' '$dir/$(Split-Path $Epf -Leaf)' '$param' '$report' $TimeoutSec"
    $code = $LASTEXITCODE
} finally {
    Stop-Job $operator; Receive-Job $operator | ForEach-Object { Write-Host "  $_" }; Remove-Job $operator
}
$out = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force $out | Out-Null
$name = "report-" + (Split-Path (Split-Path $Base) -Leaf) + ".txt"
& $vm -Get $report, (Join-Path $out $name) | Out-Null
if ($code -eq 124) { & $vm -Get "$report.png", $out | Out-Null }
if (Test-Path (Join-Path $out $name)) { Get-Content (Join-Path $out $name) -Encoding UTF8 }
"код 1С: $code"
