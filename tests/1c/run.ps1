# run.ps1 — прогон обработки ТестЭмулкона в базе 1С (тонкий клиент, /Execute).
#
#   .\run.ps1 [-Base C:\.temp\Base1C\EmulkonTest] [-User 'БорисовФМ (Администратор)'] [-TimeoutSec 300]
#   .\run.ps1 -Hook -Base C:\.temp\Base1C\EmulkonTest_UT103 -User 'Федоров (администратор)'   # УТ 10.3, толстый клиент
#
# Обработка получает в ПараметрЗапуска «report=…;driver=…;server=…;workstation=…»: куда писать
# отчёт, какой архив драйвера загрузить в БПО, адрес сервера Эмулкона и рабочее место устройств.
# Отчёт — build\report.txt. Если 1С не закрылась за TimeoutSec (застряла на диалоге), окна
# процесса снимаются в build\win-N.png, процесс 1С остаётся (его закрывает -Kill).
param(
    [string]$Base = "C:\.temp\Base1C\EmulkonTest",
    [string]$User = "БорисовФМ (Администратор)",
    [string]$Password = "",
    [string]$V8 = "C:\Program Files\1cv8\8.3.27.2325\bin\1cv8.exe",
    [string]$Epf = (Join-Path $PSScriptRoot "build\ТестЭмулкона.epf"),
    [string]$Server = "http://127.0.0.1:8737",
    [string]$Driver = "",
    [string]$Workstation = "EMULKON-1C-TEST",
    [int]$TimeoutSec = 300,
    [switch]$Kill,
    # Сколько ждать диалог, прежде чем считать прогон зависшим (первый запуск: «Да» вручную).
    [int]$DialogWaitSec = 10,
    # Обычное приложение (УТ 10.3): толстый клиент пропускает /Execute, обработку без формы
    # (ТестЭмулкона103) запускает крючок модуля приложения копии базы по «emulkon=<epf>» в /C
    # (prepare-ut103.ps1). Его сбой — в <epf>.hook.txt.
    [switch]$Hook,
    # info — только сведения о конфигурации и доступных типах оборудования (строки СВЕДЕНИЯ и ТИПЫ), без прогона.
    [string]$Mode = ""
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $Driver) {
    $version = (Get-Content (Join-Path $root "VERSION") -Raw).Trim()
    $Driver = Join-Path $root "dist\downloads\EmulkonDriver_$version.zip"
}
if (-not (Test-Path $Driver)) { throw "нет архива драйвера $Driver — соберите .\build.ps1" }
try { $null = Invoke-RestMethod "$Server/api/v1/downloads" -TimeoutSec 5 } catch { throw "сервер Эмулкона $Server не отвечает: $($_.Exception.Message)" }

$build = Join-Path $PSScriptRoot "build"
$report = Join-Path $build "report.txt"
$log = Join-Path $build "1c-out.txt"
foreach ($f in @($report, $log, "$report.status")) { if (Test-Path $f) { [IO.File]::Delete($f) } }
$param = "report=$report;driver=$Driver;server=$Server;workstation=$Workstation"
if ($Mode) { $param += ";mode=$Mode" }

$run = "/Execute`"$Epf`""
if ($Hook) {
    if (-not $PSBoundParameters.ContainsKey("Epf")) { $Epf = Join-Path $PSScriptRoot "build\ТестЭмулкона103.epf" }
    if (Test-Path "$Epf.hook.txt") { [IO.File]::Delete("$Epf.hook.txt") }
    $param = "emulkon=$Epf;$param"
    $run = $null
}
$args = @("ENTERPRISE", "/F`"$Base`"", "/N`"$User`"", "/P`"$Password`"", $run, "/C`"$param`"",
    "/DisableStartupMessages", "/DisableStartupDialogs", "/Out`"$log`"") | Where-Object { $_ }
# «Предупреждение безопасности» при открытии обработки ключом /DisableUnsafeActionProtection
# не снимается: обработка сама выключает его пользователю в копии базы (первый раз — «Да» руками).
# «Оператор у кассы»: отвечает на экраны устройств тестового рабочего места, пока идёт прогон —
# прикладывает карту VISA на терминале, кладёт 0,777 кг на весы (docs/protocol.md §2.0).
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

# Окна процесса 1С: заголовок, размер, снимок через PrintWindow — видно даже за другими окнами.
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class Win1C {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public struct RECT { public int L, T, R, B; }
  public static List<IntPtr> Of(uint pid) {
    var list = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
    return list;
  }
}
"@
function Get-1CWindows([int]$ProcessId) {
    foreach ($h in [Win1C]::Of([uint32]$ProcessId)) {
        $sb = New-Object Text.StringBuilder 256; [void][Win1C]::GetWindowText($h, $sb, 256)
        $r = New-Object Win1C+RECT; [void][Win1C]::GetWindowRect($h, [ref]$r)
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        if ($w -ge 20 -and $hh -ge 20) { [pscustomobject]@{ Handle = $h; Title = $sb.ToString(); Width = $w; Height = $hh } }
    }
}

$p = Start-Process -FilePath $V8 -ArgumentList $args -PassThru
# Ждём завершения. Небольшое окно (диалог, ошибка, вопрос), которое висит дольше 10 с, значит, что
# прогон встал: снимаем окна сразу, не дожидаясь общего таймаута. Всплывающие уведомления 1С —
# без заголовка, их не считаем.
$deadline = (Get-Date).AddSeconds($TimeoutSec); $dialogSince = $null; $stuck = $null
while (-not $p.WaitForExit(2000)) {
    # Окно БСП «Информационная база была перемещена или восстановлена…» в копии базы не модальное: прогон
    # идёт и при нём. Его не нажимаем и зависанием не считаем.
    $dialog = Get-1CWindows $p.Id | Where-Object { $_.Title -and $_.Height -lt 400 -and $_.Width -gt 200 -and $_.Title -notlike "Информационная база была перемещена*" } | Select-Object -First 1
    # Установка компоненты на клиент заканчивается сообщением «Внешняя компонента успешно
    # установлена» с одной кнопкой OK. Обработка ставит метку report.txt.status перед установкой;
    # окно этого размера при метке закрываем (WM_CLOSE — как крестик). Другие окна не трогаем.
    if ($dialog -and (Test-Path "$report.status") -and $dialog.Width -lt 450 -and $dialog.Height -lt 200) {
        Start-Sleep -Milliseconds 500
        [void][Win1C]::PostMessage($dialog.Handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        Write-Host "закрыто сообщение об установке компоненты: «$($dialog.Title)» $($dialog.Width)x$($dialog.Height)"
        $dialogSince = $null; continue
    }
    if ($dialog) { if (-not $dialogSince) { $dialogSince = Get-Date } } else { $dialogSince = $null }
    if ($dialogSince -and ((Get-Date) - $dialogSince).TotalSeconds -gt $DialogWaitSec) { $stuck = "висит окно «$($dialog.Title)» $($dialog.Width)x$($dialog.Height)"; break }
    if ((Get-Date) -gt $deadline) { $stuck = "не закрылась за $TimeoutSec с"; break }
}
if (-not $stuck) {
    Write-Host "1С завершилась, код $($p.ExitCode)"
} else {
    Write-Host "1С $stuck — окна процесса:"
    $n = 0
    foreach ($win in Get-1CWindows $p.Id) {
        $bmp = New-Object Drawing.Bitmap $win.Width, $win.Height; $g = [Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc(); [void][Win1C]::PrintWindow($win.Handle, $dc, 2); $g.ReleaseHdc($dc)
        $file = Join-Path $build "win-$n.png"; $bmp.Save($file, [Drawing.Imaging.ImageFormat]::Png)
        Write-Host "  [$n] $($win.Title) $($win.Width)x$($win.Height) → build\win-$n.png"; $n++
    }
    if ($Kill) { Stop-Process -Id $p.Id -Force; Write-Host "  процесс 1С закрыт" }
}
Stop-Job $operator; Receive-Job $operator | Select-Object -Unique | ForEach-Object { Write-Host $_ }; Remove-Job $operator
if (Test-Path $log) { Write-Host "--- вывод 1С"; Get-Content $log -Encoding UTF8 | Select-Object -First 30 }
if ($Hook -and (Test-Path "$Epf.hook.txt")) { Write-Host "--- сбой крючка"; Get-Content "$Epf.hook.txt" -Encoding UTF8 }
if (Test-Path $report) { Write-Host "--- отчёт"; Get-Content $report -Encoding UTF8 } else { Write-Host "отчёта нет" }
