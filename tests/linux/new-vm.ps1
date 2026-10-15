# new-vm.ps1 — виртуальная машина Hyper-V с Ubuntu (рабочий стол, xrdp) для проверки драйвера Эмулкона в 1С
# под Linux. Образ — из галереи «Быстрое создание» Hyper-V (Microsoft): тот же, что предлагает Hyper-V Manager,
# с настроенным расширенным сеансом (xrdp). Контрольная сумма — из описания галереи.
#
#   .\new-vm.ps1                      # скачать образ (~2 ГБ), проверить SHA-256, создать и запустить ВМ
#   .\new-vm.ps1 -List                # какие образы Ubuntu есть в галерее
#   .\new-vm.ps1 -Remove              # удалить ВМ и её диск (скачанный образ в кэше остаётся)
#
# Нужны права администратора (Hyper-V). Первый запуск ВМ — мастер Ubuntu: язык, раскладка, пользователь
# (один раз, в окне ВМ). Дальше — README.md: платформа 1С и проверка драйвера. Сохранять в UTF-8 with BOM.

param(
    [string]$Name = "EmulkonLinux",
    [string]$Root = "C:\.Hyper-V\Emulkon",
    [string]$Match = "Ubuntu",  # образ галереи: первое совпадение по имени (самая новая версия Ubuntu)
    [int]$Cpu = 4,
    [int]$MemoryGB = 8,
    [int]$DiskGB = 64,
    [string]$Switch = "Default Switch",
    [switch]$List,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$gallery = "https://go.microsoft.com/fwlink/?linkid=851584"  # галерея «Быстрое создание» Hyper-V
$vmDir = Join-Path $Root $Name
$cache = Join-Path $Root "cache"

function Get-GalleryImages {
    $json = Invoke-RestMethod $gallery -UseBasicParsing
    @($json.images | Where-Object { $_.name -match $Match })
}

if ($List) {
    Get-GalleryImages | ForEach-Object { "{0} · версия {1} · {2}" -f $_.name, $_.version, $_.disk.uri }
    return
}

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
New-Item -ItemType Directory -Force $vmDir, $cache | Out-Null

# --- 1. Образ галереи: скачать, сверить SHA-256 из описания галереи, распаковать ---
$img = Get-GalleryImages | Sort-Object { $_.version } -Descending | Select-Object -First 1
if (-not $img) { throw "В галерее Hyper-V нет образа «$Match» (.\new-vm.ps1 -List)" }
$uri = $img.disk.uri
$want = ($img.disk.hash -replace '^sha256:', '').ToLower()
$zip = Join-Path $cache ([IO.Path]::GetFileName(([uri]$uri).AbsolutePath))
Write-Host "Образ: $($img.name), версия $($img.version)"
if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $want) {
    Write-Host "Скачиваю $uri …"
    Start-BitsTransfer -Source $uri -Destination $zip
}
if ($want -and (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $want) { throw "SHA-256 образа не совпала с описанием галереи" }
$unpacked = Join-Path $cache ([IO.Path]::GetFileNameWithoutExtension($zip))
if (-not (Test-Path $unpacked)) { Expand-Archive $zip $unpacked }
$source = Get-ChildItem $unpacked -Recurse -Include *.vhdx, *.vhd | Select-Object -First 1 -ExpandProperty FullName
if (-not $source) { throw "В архиве образа нет диска VHDX" }

# --- 2. Диск ВМ: копия образа нужного размера ---
$disk = Join-Path $vmDir "$Name.vhdx"
Copy-Item $source $disk
Resize-VHD -Path $disk -SizeBytes ($DiskGB * 1GB)

# --- 3. ВМ как у «Быстрого создания»: поколение 2, UEFI с сертификатом для Linux, расширенный сеанс ---
New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemoryGB * 1GB) -VHDPath $disk -SwitchName $Switch -Path $Root | Out-Null
Set-VM -Name $Name -ProcessorCount $Cpu -AutomaticCheckpointsEnabled $false -EnhancedSessionTransportType HvSocket
Set-VMFirmware -VMName $Name -SecureBootTemplate MicrosoftUEFICertificateAuthority
Enable-VMIntegrationService -VMName $Name -Name "Guest Service Interface"
Checkpoint-VM -Name $Name -SnapshotName "чистая Ubuntu"
Start-VM -Name $Name
vmconnect.exe localhost $Name
Write-Host "ВМ $Name запущена: пройдите мастер Ubuntu в окне ВМ (язык, пользователь)." -ForegroundColor Green
Write-Host "Дальше — tests\linux\README.md: платформа 1С (install-1c.sh) и проверка драйвера."
