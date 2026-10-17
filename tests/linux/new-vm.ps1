# new-vm.ps1 — виртуальная машина Hyper-V с Ubuntu 24.04 для проверки драйвера Эмулкона в 1С под Linux.
#
# По умолчанию — облачный образ Ubuntu (cloud-images.ubuntu.com) без мастера и без окна: cloud-init заводит
# пользователя emulkon со входом только по SSH-ключу и sudo без пароля, ставит Xvfb (1С без экрана) и службы
# Hyper-V. ВМ доступна с этого компьютера: ssh по имени emulkon-linux.mshome.net (DNS «Default Switch»), см.
# vm.ps1. Ничего нажимать не нужно.
#   - qcow2 → VHD и ISO с настройками cloud-init собирает vmtool (Go, без qemu-img и без монтирования дисков);
#   - права администратора не нужны, если Hyper-V доступен пользователю (группа «Администраторы Hyper-V» и т. п.).
#
# -Desktop — прежний вариант: образ Ubuntu с рабочим столом для Hyper-V от Canonical, первый запуск — мастер Ubuntu
# в окне ВМ (пароль пользователя вводит человек).
#
#   .\new-vm.ps1 -Download        # только скачать и подготовить образ — без Hyper-V
#   .\new-vm.ps1                  # облачная ВМ EmulkonUbuntu: создать, дождаться cloud-init, снимок «чистая Ubuntu»
#   .\new-vm.ps1 -Remove          # удалить ВМ и её диск (образ в кэше остаётся)
#   .\new-vm.ps1 -Desktop         # ВМ EmulkonLinux с рабочим столом и мастером
#
# Сохранять в UTF-8 with BOM.

param(
    [string]$Name,
    [string]$Root = "C:\.Hyper-V\Emulkon",
    [string]$Release = "noble",  # кодовое имя Ubuntu: noble — 24.04 LTS, jammy — 22.04 LTS
    [int]$Cpu = 4,
    [int]$MemoryGB = 8,
    [int]$DiskGB = 40,
    [string]$Switch = "Default Switch",
    [switch]$Desktop,
    [switch]$Download,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # иначе Invoke-WebRequest в Windows PowerShell качает в разы медленнее
if (-not $Name) { $Name = if ($Desktop) { "EmulkonLinux" } else { "EmulkonUbuntu" } }
$vmDir = Join-Path $Root $Name
$cache = Join-Path $Root "cache"
$hostName = "emulkon-linux"  # имя ВМ в сети: emulkon-linux.mshome.net
$key = Join-Path $HOME ".ssh\emulkon_vm_ed25519"
$knownHosts = Join-Path $HOME ".ssh\emulkon_vm_known_hosts"

# go пишет ход работы в stderr: в Windows PowerShell 5.1 это не ошибка — итог по $LASTEXITCODE.
function Invoke-Native([string]$Exe) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Exe @args 2>&1 | ForEach-Object { "$_" } } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($args[0]): код $LASTEXITCODE" }
}

# Get-Verified — файл $file из $base в кэш, сверка с SHA256SUMS там же.
function Get-Verified([string]$base, [string]$file) {
    New-Item -ItemType Directory -Force $cache | Out-Null
    $sums = [Text.Encoding]::ASCII.GetString((Invoke-WebRequest "$base/SHA256SUMS" -UseBasicParsing).Content)
    $line = $sums -split "`n" | Where-Object { $_ -match "^([0-9a-f]{64}) \*?$([regex]::Escape($file))$" } | Select-Object -First 1
    if (-not $line) { throw "В $base/SHA256SUMS нет $file" }
    $want = ($line -split ' ')[0]
    $path = Join-Path $cache $file
    if (-not (Test-Path $path) -or (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() -ne $want) {
        Write-Host "Скачиваю $base/$file …"
        Start-BitsTransfer -Source "$base/$file" -Destination $path -DisplayName "Ubuntu $Release для Hyper-V"
        if ((Get-FileHash $path -Algorithm SHA256).Hash.ToLower() -ne $want) { throw "SHA-256 $file не совпала с SHA256SUMS" }
    }
    Write-Host "Образ проверен: $path"
    @{ Path = $path; Sha = $want }
}

# Образ с рабочим столом: распакованный VHDX.
function Get-DesktopImage {
    $img = Get-Verified "https://partner-images.canonical.com/hyper-v/desktop/$Release/release/current" "ubuntu-$Release-hyperv-amd64-ubuntu-desktop-hyperv.vhdx.zip"
    $unpacked = Join-Path $cache "$Release"
    if (-not (Get-ChildItem $unpacked -Recurse -Include *.vhdx -ErrorAction SilentlyContinue)) { Expand-Archive $img.Path $unpacked -Force }
    $vhdx = Get-ChildItem $unpacked -Recurse -Include *.vhdx | Select-Object -First 1 -ExpandProperty FullName
    if (-not $vhdx) { throw "В архиве образа нет диска VHDX" }
    $vhdx
}

# Облачный образ: qcow2 → фиксированный VHD (vmtool) один раз на версию образа.
function Get-CloudImage {
    $img = Get-Verified "https://cloud-images.ubuntu.com/$Release/current" "$Release-server-cloudimg-amd64.img"
    $vhd = Join-Path $cache "$Release-cloud-$($img.Sha.Substring(0, 12)).vhd"
    if (-not (Test-Path $vhd)) {
        Write-Host "qcow2 → VHD: $vhd"
        if (-not (Get-Command go -ErrorAction SilentlyContinue) -and (Test-Path 'C:\.dev\.tools\activate.ps1')) { . 'C:\.dev\.tools\activate.ps1' | Out-Null }
        Invoke-Native go -C (Join-Path $PSScriptRoot "vmtool") run . vhd $img.Path "$vhd.part"
        Move-Item "$vhd.part" $vhd
    }
    $vhd
}

# Ключ SSH для входа в ВМ: создаётся один раз, без пароля (только для тестовых ВМ этого компьютера).
function Get-SshKey {
    if (-not (Test-Path $key)) {
        New-Item -ItemType Directory -Force (Split-Path $key) | Out-Null
        Invoke-Native ssh-keygen -q -t ed25519 -N '""' -C "emulkon-test-vm" -f $key
    }
    (Get-Content "$key.pub" -Raw).Trim()
}

# Seed cloud-init (NoCloud): пользователь emulkon только с ключом, sudo без пароля, пакеты для 1С без экрана.
function New-Seed([string]$iso) {
    $pub = Get-SshKey
    $userData = @"
#cloud-config
hostname: $hostName
users:
  - name: emulkon
    gecos: Emulkon test
    shell: /bin/bash
    groups: [sudo]
    sudo: ALL=(ALL) NOPASSWD:ALL
    lock_passwd: true
    ssh_authorized_keys:
      - $pub
ssh_pwauth: false
package_update: true
packages:
  - linux-cloud-tools-virtual
  - xvfb
  - xauth
  - unzip
  - fonts-dejavu-core
  - libgtk-3-0t64
locale: ru_RU.UTF-8
timezone: Asia/Yekaterinburg
runcmd:
  - [systemctl, enable, --now, hv-kvp-daemon.service]
"@
    $tmp = Join-Path $vmDir "cloud-init"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $utf8 = New-Object Text.UTF8Encoding $false
    [IO.File]::WriteAllText((Join-Path $tmp "user-data"), $userData.Replace("`r`n", "`n"), $utf8)
    [IO.File]::WriteAllText((Join-Path $tmp "meta-data"), "instance-id: $Name-$(Get-Date -Format yyyyMMddHHmmss)`nlocal-hostname: $hostName`n", $utf8)
    if (-not (Get-Command go -ErrorAction SilentlyContinue) -and (Test-Path 'C:\.dev\.tools\activate.ps1')) { . 'C:\.dev\.tools\activate.ps1' | Out-Null }
    Invoke-Native go -C (Join-Path $PSScriptRoot "vmtool") run . seed $iso (Join-Path $tmp "user-data") (Join-Path $tmp "meta-data")
}

if ($Download) {
    if ($Desktop) { Get-DesktopImage } else { Get-CloudImage; Get-SshKey | Out-Null }
    return
}

# Hyper-V: хватает прав управлять ВМ (администратор, группа «Администраторы Hyper-V» или права, выданные
# иначе) — проверяем на деле, а не по группе.
Import-Module Hyper-V
try { Get-VM -ErrorAction Stop | Out-Null } catch {
    throw "Нет прав на Hyper-V ($($_.Exception.Message)): PowerShell от имени администратора или группа «Администраторы Hyper-V»"
}

if ($Remove) {
    if (Get-VM -Name $Name -ErrorAction SilentlyContinue) {
        Stop-VM -Name $Name -TurnOff -Force -ErrorAction SilentlyContinue
        Remove-VM -Name $Name -Force
    }
    Remove-Item -LiteralPath $vmDir -Recurse -Force -ErrorAction SilentlyContinue
    if (-not $Desktop) { Remove-Item $knownHosts -ErrorAction SilentlyContinue }  # у новой ВМ новый ключ хоста
    Write-Host "ВМ $Name удалена" -ForegroundColor Green
    return
}
if (Get-VM -Name $Name -ErrorAction SilentlyContinue) { throw "ВМ $Name уже есть: .\new-vm.ps1 -Remove, чтобы пересоздать" }
New-Item -ItemType Directory -Force $vmDir | Out-Null
$disk = Join-Path $vmDir "$Name.vhdx"

if ($Desktop) {
    # Как у «Быстрого создания» для Ubuntu: поколение 2, без Secure Boot, расширенный сеанс.
    Copy-Item (Get-DesktopImage) $disk
    Resize-VHD -Path $disk -SizeBytes ($DiskGB * 1GB)
    New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemoryGB * 1GB) -VHDPath $disk -SwitchName $Switch -Path $Root | Out-Null
    Set-VM -Name $Name -ProcessorCount $Cpu -AutomaticCheckpointsEnabled $false -EnhancedSessionTransportType HvSocket
    Set-VMFirmware -VMName $Name -EnableSecureBoot Off
    # «Интерфейс гостевой службы» (Copy-VMFile): имя зависит от языка Windows — ищем по идентификатору.
    Get-VMIntegrationService -VMName $Name | Where-Object { $_.Id -like "*\6C09BB55-D683-4DA0-8931-C9BF705F6480" } | Enable-VMIntegrationService
    Checkpoint-VM -Name $Name -SnapshotName "чистая Ubuntu"
    Start-VM -Name $Name
    vmconnect.exe localhost $Name
    Write-Host "ВМ $Name запущена: пройдите мастер Ubuntu в окне ВМ (язык, пользователь)." -ForegroundColor Green
    return
}

# --- Облачная ВМ ---
$vhd = Get-CloudImage
Convert-VHD -Path $vhd -DestinationPath $disk -VHDType Dynamic
Resize-VHD -Path $disk -SizeBytes ($DiskGB * 1GB)  # корень расширит cloud-init (growpart)
$seed = Join-Path $vmDir "seed.iso"
New-Seed $seed
New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemoryGB * 1GB) -VHDPath $disk -SwitchName $Switch -Path $Root | Out-Null
Set-VM -Name $Name -ProcessorCount $Cpu -AutomaticCheckpointsEnabled $false
Set-VMFirmware -VMName $Name -SecureBootTemplate MicrosoftUEFICertificateAuthority
Add-VMDvdDrive -VMName $Name -Path $seed
Set-VMFirmware -VMName $Name -FirstBootDevice (Get-VMHardDiskDrive -VMName $Name)
Get-VMIntegrationService -VMName $Name | Where-Object { $_.Id -like "*\6C09BB55-D683-4DA0-8931-C9BF705F6480" } | Enable-VMIntegrationService
Remove-Item $knownHosts -ErrorAction SilentlyContinue
Start-VM -Name $Name

# Ждём SSH (до 10 минут: первый запуск — обновление пакетов) и окончания cloud-init.
$ssh = @("-i", $key, "-o", "UserKnownHostsFile=$knownHosts", "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=5",
    "-o", "BatchMode=yes", "emulkon@$hostName.mshome.net")
$deadline = (Get-Date).AddMinutes(10)
$ErrorActionPreference = "Continue"  # пока ВМ грузится, ssh пишет в stderr — это не ошибка скрипта
do {
    Start-Sleep 10
    & ssh.exe @ssh "true" 2>&1 | Out-Null
} until ($LASTEXITCODE -eq 0 -or (Get-Date) -gt $deadline)
if ($LASTEXITCODE -ne 0) { throw "ВМ $Name не отвечает по SSH ($hostName.mshome.net) за 10 минут — консоль ВМ: vmconnect localhost $Name" }
& ssh.exe @ssh "cloud-init status --wait > /dev/null; cloud-init status --long; lsb_release -ds; uname -r" 2>&1 | ForEach-Object { "$_" }
$ErrorActionPreference = "Stop"
Remove-VMDvdDrive -VMName $Name -ControllerNumber 0 -ControllerLocation 1 -ErrorAction SilentlyContinue
Checkpoint-VM -Name $Name -SnapshotName "чистая Ubuntu"
Write-Host "ВМ $Name готова: tests\linux\vm.ps1 <команда> — команда в ВМ по SSH. Снимок «чистая Ubuntu»." -ForegroundColor Green
