# new-vm.ps1 — тестовая ВМ Hyper-V с Windows Server 2022 для проверок установщика Эмулкона на чистой системе:
# Defender, удаление, трей другого пользователя (#20). Поколение 1 (BIOS): с пустого диска загрузка с DVD идёт без
# «Press any key», Windows ставится сама по autounattend.xml — его и emulkon-setup.ps1 кладёт на второй DVD vmtool.
#
# Человек делает одно: на экране «Параметры» в окне ВМ задаёт пароль администратора (скрипт его не знает и не
# спрашивает). Дальше доступ — по SSH-ключу ~/.ssh/emulkon_vm_ed25519 (вход только по ключу), см. vm.ps1.
#
#   .\new-vm.ps1 -Iso D:\…\windows_server_2022….iso   # создать EmulkonWin и начать установку
#   .\new-vm.ps1 -Snapshot                          # после установки: убрать DVD, снимок «чистая Windows»
#   .\new-vm.ps1 -Remove                            # удалить ВМ и её диск
#
# Сохранять в UTF-8 with BOM.

param(
    [string]$Name = "EmulkonWin",
    [string]$Root = "D:\.Hyper-V\Emulkon",  # ВМ и образы — только на D: (решение владельца 2026-10-17)
    [string]$Iso,
    [int]$Cpu = 4,
    [int]$MemoryGB = 6,
    [int]$DiskGB = 60,
    [string]$Switch = "Default Switch",
    [switch]$Snapshot,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$vmDir = Join-Path $Root $Name
$key = Join-Path $HOME ".ssh\emulkon_vm_ed25519"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")

try { Get-VM -ErrorAction Stop | Out-Null } catch { throw "Нет доступа к Hyper-V: $($_.Exception.Message)" }

if ($Remove) {
    if (Get-VM $Name -ErrorAction SilentlyContinue) {
        Stop-VM $Name -TurnOff -Force -ErrorAction SilentlyContinue
        Remove-VM $Name -Force
    }
    if (Test-Path $vmDir) { Remove-Item $vmDir -Recurse -Force }
    "Удалена: $Name"
    return
}

if ($Snapshot) {
    Get-VMDvdDrive $Name | Set-VMDvdDrive -Path $null  # на ходу: IDE-привод у работающей ВМ не убрать
    Checkpoint-VM $Name -SnapshotName "чистая Windows Server 2022 + OpenSSH"
    "Снимок сделан, DVD извлечены"
    return
}

if (-not $Iso -or -not (Test-Path $Iso)) { throw "Укажите образ Windows: -Iso <путь к .iso>" }
if (Get-VM $Name -ErrorAction SilentlyContinue) { throw "ВМ $Name уже есть (-Remove, чтобы пересоздать)" }
if (-not (Test-Path "$key.pub")) { throw "Нет ключа $key.pub — его создаёт tests/linux/new-vm.ps1" }

# Диск ответов: autounattend.xml, emulkon-setup.ps1 и открытый ключ для administrators_authorized_keys.
New-Item -ItemType Directory -Force $vmDir | Out-Null
$stage = Join-Path $vmDir "answer"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $PSScriptRoot "autounattend.xml"), (Join-Path $PSScriptRoot "emulkon-setup.ps1") $stage -Force
Copy-Item "$key.pub" (Join-Path $stage "authorized_keys") -Force
# OpenSSH для ВМ — MSI Win32-OpenSSH (Microsoft) в кэше на D:; без него ВМ возьмёт компонент из Центра обновления.
$sshMsi = Get-ChildItem (Join-Path $Root "cache") -Filter "OpenSSH-Win64-*.msi" -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if ($sshMsi) {
    if ((Get-AuthenticodeSignature $sshMsi.FullName).Status -ne 'Valid') { throw "$($sshMsi.Name): подпись не действительна" }
    Copy-Item $sshMsi.FullName (Join-Path $stage "openssh.msi") -Force
} else {
    Write-Warning "Нет $Root\cache\OpenSSH-Win64-*.msi (github.com/PowerShell/Win32-OpenSSH) — OpenSSH из Центра обновления, долго"
}
$answerIso = Join-Path $vmDir "answer.iso"
Push-Location (Join-Path $repo "tests\linux\vmtool")
try {
    $files = Get-ChildItem $stage | ForEach-Object FullName
    & go run . iso $answerIso ANSWER @files
    if ($LASTEXITCODE -ne 0) { throw "vmtool iso: код $LASTEXITCODE" }
} finally { Pop-Location }

$vhd = Join-Path $vmDir "$Name.vhdx"
New-VM -Name $Name -Generation 1 -MemoryStartupBytes ($MemoryGB * 1GB) -NewVHDPath $vhd -NewVHDSizeBytes ($DiskGB * 1GB) `
    -SwitchName $Switch -Path $Root | Out-Null
Set-VM $Name -ProcessorCount $Cpu -StaticMemory -AutomaticCheckpointsEnabled $false -CheckpointType Standard
Set-VMDvdDrive -VMName $Name -ControllerNumber 1 -ControllerLocation 0 -Path $Iso
Add-VMDvdDrive -VMName $Name -ControllerNumber 1 -ControllerLocation 1 -Path $answerIso
Set-VMBios $Name -StartupOrder @("CD", "IDE", "LegacyNetworkAdapter", "Floppy")
Start-VM $Name
"ВМ $Name запущена: Windows ставится сама (~15–25 мин). Затем в окне ВМ задайте пароль администратора;"
"проверка: testswindowsm.ps1 hostname (вход — «Администратор», по ключу)"
