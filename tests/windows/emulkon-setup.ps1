# emulkon-setup.ps1 — этап specialize установки тестовой ВМ Windows (autounattend.xml): OpenSSH со входом
# администраторов только по ключу из administrators_authorized_keys (ключ кладёт new-vm.ps1 рядом на диск ответов).
# Пароли не задаются и не нужны. Лог — C:\Windows\Temp\emulkon-setup.log.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
"начало $(Get-Date)"

# OpenSSH — из openssh.msi с диска ответов (Win32-OpenSSH от Microsoft, кладёт new-vm.ps1): компонент Windows по
# требованию качается из Центра обновления, а на свежей Server 2022 это зависало на десятки минут.
$msi = Join-Path $here 'openssh.msi'
if (Test-Path $msi) {
    $p = Start-Process msiexec.exe -ArgumentList "/i `"$msi`" /qn /norestart ADDLOCAL=Server" -Wait -PassThru
    "openssh.msi: код $($p.ExitCode)"
} else {
    $job = Start-Job { Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0 }
    if (-not (Wait-Job $job -Timeout 900)) { "OpenSSH из Центра обновления не встал за 15 мин"; Stop-Job $job }
}

$keys = Join-Path $here 'authorized_keys'
New-Item -ItemType Directory -Force C:\ProgramData\ssh | Out-Null
Copy-Item $keys C:\ProgramData\ssh\administrators_authorized_keys -Force
icacls C:\ProgramData\ssh\administrators_authorized_keys /inheritance:r /grant 'SYSTEM:F' /grant '*S-1-5-32-544:F'

# Вход только по ключу; оболочка — PowerShell.
Set-Service sshd -StartupType Automatic
Start-Service sshd  # создаёт sshd_config по умолчанию
$cfg = 'C:\ProgramData\ssh\sshd_config'
(Get-Content $cfg) -replace '^#?PasswordAuthentication .*', 'PasswordAuthentication no' | Set-Content $cfg -Encoding ascii
New-Item -Force HKLM:\SOFTWARE\OpenSSH -ErrorAction SilentlyContinue | Out-Null
New-ItemProperty -Path HKLM:\SOFTWARE\OpenSSH -Name DefaultShell -Value 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' -PropertyType String -Force | Out-Null
Restart-Service sshd
if (-not (Get-NetFirewallRule -Name OpenSSH-Server-In-TCP -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -Name OpenSSH-Server-In-TCP -DisplayName 'OpenSSH Server (sshd)' -Protocol TCP -LocalPort 22 -Direction Inbound -Action Allow | Out-Null
}
"sshd: $((Get-Service sshd).Status)"
"конец $(Get-Date)"
