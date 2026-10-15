# ci-env.ps1 — окружение сборки C++ на раннере GitHub Actions (windows-2022): clang-cl из LLVM раннера, MSVC и
# Windows SDK из его Visual Studio, Ninja. Пишет переменные в GITHUB_ENV для toolchain-файлов cmake/toolchains.
# Локально не нужен: там портативный тулчейн C:\.dev\.tools.

$ErrorActionPreference = "Stop"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Нет Visual Studio с компонентом MSVC" }
$vcVersion = (Get-Content (Join-Path $vs "VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt") -TotalCount 1).Trim()
$vc = Join-Path $vs "VC\Tools\MSVC\$vcVersion"

$sdk = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
# Последний SDK, где есть и windows.h, и mmc.h (оснастка MMC).
$sdkVersion = Get-ChildItem (Join-Path $sdk "Include") -Directory |
    Where-Object { (Test-Path (Join-Path $_.FullName "um\windows.h")) -and (Test-Path (Join-Path $_.FullName "um\mmc.h")) } |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1 -ExpandProperty Name
if (-not $sdkVersion) { throw "Нет Windows SDK с windows.h и mmc.h в $sdk" }

$llvm = "C:\Program Files\LLVM\bin"
foreach ($exe in "clang-cl.exe", "lld-link.exe", "llvm-rc.exe", "llvm-lib.exe") {
    if (-not (Test-Path (Join-Path $llvm $exe))) { throw "Нет $exe в $llvm" }
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    choco install ninja -y --no-progress | Out-Null
}

$vars = [ordered]@{
    VCToolsInstallDir    = $vc
    WindowsSdkDir        = $sdk
    WindowsSDKVersion    = $sdkVersion
    LLVM_DIR_BIN         = $llvm
    DEV_CMAKE_TOOLCHAINS = Join-Path $env:GITHUB_WORKSPACE "cmake\toolchains"
}
foreach ($k in $vars.Keys) {
    "$k=$($vars[$k])" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
    Write-Host "$k = $($vars[$k])"
}
& (Join-Path $llvm "clang-cl.exe") --version | Select-Object -First 1
cmake --version | Select-Object -First 1
