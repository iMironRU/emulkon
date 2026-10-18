# vm.ps1 — команда PowerShell в тестовой ВМ Windows (new-vm.ps1) по SSH: вход «Администратор» (встроенный админ
# русской Windows), ключ ~/.ssh/emulkon_vm_ed25519 (только по ключу — пароль не нужен).
#
#   tests\windows\vm.ps1 '$PSVersionTable.PSVersion'           # команда; код возврата — как в ВМ
#   tests\windows\vm.ps1 -Put dist\downloads\EmulkonSetup_X.exe # файл в ВМ (scp; см. ниже)
#   tests\windows\vm.ps1 -Get C:/Windows/Temp/x.log,.           # файл из ВМ
#
# Передача файлов: scp в этой ВМ ненадёжен (в системе уживаются встроенный OpenSSH 8.1 и OpenSSH 10 из MSI,
# берётся не та подсистема sftp). Надёжный путь для бинарников — HTTP: на хосте `serve.ps1 -Dir <папка>`, в ВМ
# `Invoke-WebRequest http://172.27.144.1:8099/<имя> -OutFile ...` (172.27.144.1 — хост в «Default Switch»).
#
# Сервер Эмулкона этого компьютера из ВМ — по адресу шлюза «Default Switch» (Get-NetRoute 0.0.0.0/0).
# Сохранять в UTF-8 with BOM.

param(
    [Parameter(Position = 0)][string]$Command,
    [string]$Put,
    [string[]]$Get,
    [string]$HostName = "emulkon-win.mshome.net",
    [string]$User = "Администратор"  # встроенный администратор русской Windows (не Administrator)
)

$ErrorActionPreference = "Continue"  # ssh пишет предупреждения в stderr
$key = Join-Path $HOME ".ssh\emulkon_vm_ed25519"
$opts = @("-i", $key, "-o", "UserKnownHostsFile=$(Join-Path $HOME '.ssh\emulkon_vm_known_hosts')",
    "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10")
if (-not (Test-Path $key)) { throw "Нет ключа $key" }

if ($Put) {
    & scp.exe @opts $Put "${User}@${HostName}:"
} elseif ($Get) {
    & scp.exe @opts "${User}@${HostName}:$($Get[0])" $(if ($Get.Count -gt 1) { $Get[1] } else { "." })
} elseif ($Command) {
    # Оболочка в ВМ — Windows PowerShell: команда уходит как -EncodedCommand (UTF-16LE), кавычки не портятся.
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes(
            "[Console]::OutputEncoding = [Text.Encoding]::UTF8; `$ProgressPreference = 'SilentlyContinue'; " + $Command))
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    & ssh.exe @opts "${User}@$HostName" "powershell -NoProfile -NonInteractive -EncodedCommand $enc"
} else {
    throw "Укажите команду, -Put или -Get"
}
exit $LASTEXITCODE
