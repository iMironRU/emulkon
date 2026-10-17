# vm.ps1 — команда в тестовой ВМ Linux (new-vm.ps1) по SSH: пользователь emulkon, ключ ~/.ssh/emulkon_vm_ed25519.
#
#   tests\linux\vm.ps1 "uname -a"                       # команда; код возврата — как в ВМ
#   tests\linux\vm.ps1 -Put dist\downloads\x.zip         # файл в домашний каталог emulkon (scp)
#   tests\linux\vm.ps1 -Get /home/emulkon/out.log .      # файл из ВМ
#
# Хост сервера Эмулкона для ВМ (адрес «Default Switch» этого компьютера) — в переменной ВМ не нужен: из ВМ он
# доступен как $(ip route | awk '/default/ {print $3}').

param(
    [Parameter(Position = 0)][string]$Command,
    [string]$Put,
    [string[]]$Get,
    [string]$HostName = "emulkon-linux.mshome.net"
)

$ErrorActionPreference = "Continue"  # ssh пишет предупреждения в stderr
$key = Join-Path $HOME ".ssh\emulkon_vm_ed25519"
$opts = @("-i", $key, "-o", "UserKnownHostsFile=$(Join-Path $HOME '.ssh\emulkon_vm_known_hosts')",
    "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10")
if (-not (Test-Path $key)) { throw "Нет ключа $key — ВМ создаёт tests\linux\new-vm.ps1" }

if ($Put) {
    & scp.exe @opts $Put "emulkon@${HostName}:"
} elseif ($Get) {
    & scp.exe @opts "emulkon@${HostName}:$($Get[0])" $(if ($Get.Count -gt 1) { $Get[1] } else { "." })
} elseif ($Command) {
    & ssh.exe @opts "emulkon@$HostName" $Command
} else {
    throw "Укажите команду, -Put или -Get"
}
exit $LASTEXITCODE
