# new-vm.ps1 — виртуальная машина Hyper-V с Ubuntu (рабочий стол, xrdp) для проверки драйвера Эмулкона в 1С
# под Linux. Образ — от Canonical для Hyper-V (partner-images.canonical.com/hyper-v/desktop): те же образы, что
# у «Быстрого создания» Hyper-V, с настроенным расширенным сеансом (xrdp). Галерея «Быстрого создания» застряла
# на 22.04 со старыми суммами, поэтому образ и SHA256SUMS берутся прямо у Canonical.
#
#   .\new-vm.ps1 -Download            # только скачать и проверить образ (~3,4 ГБ) — права администратора не нужны
#   .\new-vm.ps1                      # скачать (если ещё нет), создать и запустить ВМ — от администратора
#   .\new-vm.ps1 -Remove              # удалить ВМ и её диск (скачанный образ в кэше остаётся)
#
# Первый запуск ВМ — мастер Ubuntu: язык, раскладка, пользователь (один раз, в окне ВМ). Дальше — README.md:
# платформа 1С и проверка драйвера. Сохранять в UTF-8 with BOM.

param(
    [string]$Name = "EmulkonLinux",
    [string]$Root = "C:\.Hyper-V\Emulkon",
    [string]$Release = "noble",  # кодовое имя Ubuntu: noble — 24.04 LTS, jammy — 22.04 LTS
    [int]$Cpu = 4,
    [int]$MemoryGB = 8,
    [int]$DiskGB = 64,
    [string]$Switch = "Default Switch",
    [switch]$Download,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # иначе Invoke-WebRequest в Windows PowerShell качает в разы медленнее
$base = "https://partner-images.canonical.com/hyper-v/desktop/$Release/release/current"
$vmDir = Join-Path $Root $Name
$cache = Join-Path $Root "cache"

# Образ в кэше, проверенный по SHA256SUMS Canonical; распакованный VHDX — путь в ответе.
function Get-Image {
    New-Item -ItemType Directory -Force $cache | Out-Null
    $file = "ubuntu-$Release-hyperv-amd64-ubuntu-desktop-hyperv.vhdx.zip"
    $sums = [Text.Encoding]::ASCII.GetString((Invoke-WebRequest "$base/SHA256SUMS" -UseBasicParsing).Content)
    $line = $sums -split "`n" | Where-Object { $_ -match "^([0-9a-f]{64}) \*?$([regex]::Escape($file))$" } | Select-Object -First 1
    if (-not $line) { throw "В SHA256SUMS нет $file" }
    $want = ($line -split ' ')[0]
    $zip = Join-Path $cache $file
    if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $want) {
        Write-Host "Скачиваю $base/$file …"
        Start-BitsTransfer -Source "$base/$file" -Destination $zip -DisplayName "Ubuntu $Release для Hyper-V"
        if ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $want) { throw "SHA-256 образа не совпала с SHA256SUMS" }
    }
    Write-Host "Образ проверен: $zip"
    $unpacked = Join-Path $cache "$Release"
    if (-not (Get-ChildItem $unpacked -Recurse -Include *.vhdx -ErrorAction SilentlyContinue)) { Expand-Archive $zip $unpacked -Force }
    $vhdx = Get-ChildItem $unpacked -Recurse -Include *.vhdx | Select-Object -First 1 -ExpandProperty FullName
    if (-not $vhdx) { throw "В архиве образа нет диска VHDX" }
    $vhdx
}

if ($Download) { Get-Image; return }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Нужны права администратора: запустите PowerShell от имени администратора"
}
Import-Module Hyper-V

if ($Remove) {
    if (Get-VM -Name $Name -ErrorAction SilentlyContinue) {
        Stop-VM -Name $Name -TurnOff -Force -ErrorAction SilentlyContinue
        Remove-VM -Name $Name -Force
    }
    Remove-Item -LiteralPath $vmDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "ВМ $Name удалена" -ForegroundColor Green
    return
}
if (Get-VM -Name $Name -ErrorAction SilentlyContinue) { throw "ВМ $Name уже есть: .\new-vm.ps1 -Remove, чтобы пересоздать" }
New-Item -ItemType Directory -Force $vmDir | Out-Null

# --- 1. Образ Canonical: скачать (если ещё нет), сверить с SHA256SUMS, распаковать ---
$source = Get-Image

# --- 2. Диск ВМ: копия образа нужного размера ---
$disk = Join-Path $vmDir "$Name.vhdx"
Copy-Item $source $disk
Resize-VHD -Path $disk -SizeBytes ($DiskGB * 1GB)

# --- 3. ВМ как у «Быстрого создания» для Ubuntu: поколение 2, без Secure Boot, расширенный сеанс ---
New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemoryGB * 1GB) -VHDPath $disk -SwitchName $Switch -Path $Root | Out-Null
Set-VM -Name $Name -ProcessorCount $Cpu -AutomaticCheckpointsEnabled $false -EnhancedSessionTransportType HvSocket
Set-VMFirmware -VMName $Name -EnableSecureBoot Off
Enable-VMIntegrationService -VMName $Name -Name "Guest Service Interface"
Checkpoint-VM -Name $Name -SnapshotName "чистая Ubuntu"
Start-VM -Name $Name
vmconnect.exe localhost $Name
Write-Host "ВМ $Name запущена: пройдите мастер Ubuntu в окне ВМ (язык, пользователь)." -ForegroundColor Green
Write-Host "Дальше — tests\linux\README.md: платформа 1С (install-1c.sh) и проверка драйвера."
