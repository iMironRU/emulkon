# ui-live.ps1 — проверка окон оснастки на установленном Эмулконе (разработческая машина): «Служба → Свойства»
# (токены управления и устройств, запуск и остановка, «Кассы в сети») и свойства рабочего места (подпись).
#
# Нажимает кнопки сообщениями окнам консоли (WM_COMMAND), без эмуляции мыши и клавиатуры. Меняет установленную
# службу (реестр, брандмауэр, перезапуски, сценарии) и в конце возвращает как было: без токенов, сеть — как до
# теста, тестового правила нет. Сценарии: выключить, удалить, окна выгрузки и загрузки (открыть, «Отмена»);
# журнал: отбор, сбросить отбор, окно выгрузки.
# Нужны права администратора. Отчёт — в файл -Out (UTF-8), код возврата 0 — всё прошло.
#
#   powershell -MTA -ExecutionPolicy Bypass -File ui-live.ps1 -Out <отчёт>   (MTA — см. Async)
#
# Сохранять в UTF-8 with BOM.

param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Msc = "C:\Program Files\Emulkon\console\Emulkon.msc"
)

$ErrorActionPreference = "Stop"
"ui-live $(Get-Date -Format s)" | Out-File $Out -Encoding utf8
$script:passed = 0
$script:failed = 0
function Log($s) { $s | Out-File $Out -Append -Encoding utf8 }
function Check($ok, $what) {
    if ($ok) { $script:passed++; Log "  ok   $what" } else { $script:failed++; Log "  FAIL $what" }
}

Add-Type @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class U {
  public delegate bool F(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(F f, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, F f, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, StringBuilder l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, string l, uint f, uint t, out IntPtr r);
  public static string Cls(IntPtr h) { var s = new StringBuilder(64); GetClassName(h, s, 64); return s.ToString(); }
  public static string Text(IntPtr h) { var s = new StringBuilder(4096); IntPtr r; SendMessageTimeout(h, 0x000D, (IntPtr)4096, s, 2, 2000, out r); return s.ToString(); }
  // Top — видимое окно процесса pid класса cls, заголовок равен title (или начинается с него, prefix).
  public static IntPtr Top(uint pid, string cls, string title, bool prefix) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h) && Cls(h) == cls) { var t = Text(h); if (prefix ? t.StartsWith(title) : t == title) found = h; }
      return true; }, IntPtr.Zero);
    return found;
  }
  // EditIn — первое поле ввода (Edit) внутри окна: имя файла в окне «Сохранить» лежит в ComboBoxEx32 → ComboBox → Edit.
  public static IntPtr EditIn(IntPtr parent) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(parent, (h, l) => { if (found == IntPtr.Zero && Cls(h) == "Edit") found = h; return true; }, IntPtr.Zero);
    return found;
  }
  public static IntPtr Child(IntPtr parent, int id) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(parent, (h, l) => { if (found == IntPtr.Zero && GetDlgCtrlID(h) == id) found = h; return true; }, IntPtr.Zero);
    return found;
  }
  public static string AllText(IntPtr parent) {
    var sb = new StringBuilder();
    EnumChildWindows(parent, (h, l) => { if (Cls(h) == "Static") sb.Append(Text(h)).Append(" "); return true; }, IntPtr.Zero);
    return sb.ToString();
  }
  // Click — «нажать» кнопку: WM_COMMAND/BN_CLICKED её окну-владельцу, асинхронно (за кнопкой может быть модальное окно).
  public static void Click(IntPtr ctrl) {
    PostMessage(GetParent(ctrl), 0x0111, (IntPtr)(GetDlgCtrlID(ctrl) & 0xFFFF), ctrl);
  }
  public static void Command(IntPtr dlg, int id) { PostMessage(dlg, 0x0111, (IntPtr)id, IntPtr.Zero); }
  public static void SetCheck(IntPtr ctrl, bool on) { IntPtr r; SendMessageTimeout(ctrl, 0x00F1, (IntPtr)(on ? 1 : 0), IntPtr.Zero, 2, 2000, out r); }
  public static bool Checked(IntPtr ctrl) { IntPtr r; SendMessageTimeout(ctrl, 0x00F0, IntPtr.Zero, IntPtr.Zero, 2, 2000, out r); return r == (IntPtr)1; }
  public static void SetText(IntPtr ctrl, string s) { IntPtr r; SendMessageTimeout(ctrl, 0x000C, IntPtr.Zero, s, 2, 2000, out r); }
  // Буфер обмена через Win32: скрипт работает в MTA, Get-Clipboard там не работает.
  [DllImport("user32.dll")] static extern bool OpenClipboard(IntPtr h);
  [DllImport("user32.dll")] static extern bool CloseClipboard();
  [DllImport("user32.dll")] static extern bool EmptyClipboard();
  [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint f);
  [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr h);
  public static string Clip() {
    if (!OpenClipboard(IntPtr.Zero)) return null;
    try { var h = GetClipboardData(13); if (h == IntPtr.Zero) return null; var p = GlobalLock(h);
          try { return Marshal.PtrToStringUni(p); } finally { GlobalUnlock(h); } }
    finally { CloseClipboard(); }
  }
  public static void ClearClip() { if (OpenClipboard(IntPtr.Zero)) { EmptyClipboard(); CloseClipboard(); } }
}
"@

$base = "http://127.0.0.1:8737"
$serverKey = "HKLM:\SOFTWARE\Emulkon\Server"
$mmcPid = 0

function Wait-Top($cls, $title, [switch]$Prefix, $sec = 30) {
    for ($i = 0; $i -lt $sec * 5; $i++) {
        $h = [U]::Top([uint32]$mmcPid, $cls, $title, [bool]$Prefix)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 200
    }
    return [IntPtr]::Zero
}

# Answer — ответить на окно-вопрос консоли («Эмулкон»): текст в отчёт, кнопка id (6 — «Да», 1 — «ОК»).
function Answer($id, $expect) {
    $box = Wait-Top "#32770" "Эмулкон"
    if ($box -eq [IntPtr]::Zero) { Check $false "окно-вопрос «$expect»"; return }
    $text = [U]::AllText($box)
    Check ($text -like "*$expect*") "окно-вопрос: $($text.Substring(0, [Math]::Min(70, $text.Length)))…"
    [U]::Command($box, $id)
    Start-Sleep -Milliseconds 300
}

function Sha256($s) {
    $b = [System.Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($s))
    -join ($b | ForEach-Object { $_.ToString("x2") })
}
function Status($path, $token) {
    $h = @{}
    if ($token) { $h.Authorization = "Bearer $token" }
    try { (Invoke-WebRequest "$base$path" -Headers $h -UseBasicParsing -TimeoutSec 5).StatusCode } catch { [int]$_.Exception.Response.StatusCode }
}
function OpenSession($token) {
    $body = @{ workstation_id = "UI-LIVE"; device_token = $token; equipment_type = "BarcodeScanner"; driver_version = "ui-live"; interface_revision = 4008 } | ConvertTo-Json
    try { (Invoke-WebRequest "$base/api/v1/sessions" -Method Post -Body $body -ContentType "application/json" -UseBasicParsing -TimeoutSec 5).StatusCode }
    catch { [int]$_.Exception.Response.StatusCode }
}
function RegValue($name) { (Get-ItemProperty $serverKey -ErrorAction SilentlyContinue).$name }
function Running { (Get-Service Emulkon).Status -eq "Running" }
function ServicePid { (Get-CimInstance Win32_Service -Filter "Name='Emulkon'").ProcessId }
function WaitFor([scriptblock]$cond, $sec = 30) {
    for ($i = 0; $i -lt $sec * 5; $i++) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 200 }
    return $false
}
# Правило брандмауэра — через netsh: Get-NetFirewallRule на каждой попытке слишком медленный.
function FirewallRule { $null = netsh advfirewall firewall show rule name="Emulkon (TCP 8737)"; $LASTEXITCODE -eq 0 }
# Go на Windows слушает 0.0.0.0 сокетом IPv4/IPv6 — его адрес «::».
function ListenAll { [bool](Get-NetTCPConnection -LocalPort 8737 -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalAddress -in "0.0.0.0", "::" }) }

# Async — пункт меню, за которым модальное окно: вызов автоматизации MMC ждёт, пока окно закроют, поэтому —
# из второго потока (MTA, как и сам скрипт: запускать с -MTA), а этот поток заполняет окно.
$script:jobs = @()
$script:stuck = $false
function Async($obj, [string]$method, $a1, $a2) {
    $rs = [runspacefactory]::CreateRunspace()
    $rs.ApartmentState = "MTA"
    $rs.Open()
    $ps = [powershell]::Create()
    $ps.Runspace = $rs
    [void]$ps.AddScript({ param($o, $m, $x, $y) if ($null -ne $y) { $o.$m($x, $y) } else { $o.$m($x) } }).
        AddArgument($obj).AddArgument($method).AddArgument($a1).AddArgument($a2)
    $script:jobs += , @($ps, $ps.BeginInvoke())
}
function Wait-Async {
    foreach ($j in $script:jobs) {
        if (-not $j[1].AsyncWaitHandle.WaitOne(60000)) { Check $false "пункт меню не завершился за 60 с — окно осталось открытым"; $script:stuck = $true; continue }
        try { $j[0].EndInvoke($j[1]) | Out-Null } catch { Log "  async: $_" }
        $j[0].Runspace.Close()
    }
    $script:jobs = @()
    if ($script:stuck) { throw "окно консоли зависло" }
}
# FileDialogCancel — пункт открыл окно выбора файла с нужным заголовком; «Отмена».
function FileDialogCancel($title) {
    $dlg = Wait-Top "#32770" $title
    Check ($dlg -ne [IntPtr]::Zero) "окно «$title» открылось"
    if ($dlg -ne [IntPtr]::Zero) { [U]::Command($dlg, 2) }
}
function FindNode($name) {
    $n = $ns.GetChild($rootNode)
    while ($n -and $n.Name -ne $name) { try { $n = $ns.GetNext($n) } catch { $n = $null } }
    $n
}
function FindItem($v, [int]$col, $value) {
    foreach ($i in @($v.ListItems)) { if ($v.CellContents($i, $col) -eq $value) { return $i } }
    $null
}
function Utf8Body($obj) { , [Text.Encoding]::UTF8.GetBytes(($obj | ConvertTo-Json -Depth 6 -Compress)) }  # «,» — массив байтов одним объектом

$wasNetwork = (RegValue "Network") -eq 1
$mmc = $null
try {
    Check (Running) "служба работает"
    Check (-not (RegValue "ControlTokenHash") -and -not (RegValue "DeviceTokenHash")) "до теста токенов нет"

    $mmc = New-Object -ComObject MMC20.Application
    $mmc.Load($Msc)
    $mmcPid = (Get-Process mmc | Sort-Object StartTime -Descending | Select-Object -First 1).Id
    $doc = $mmc.Document
    $ns = $doc.ScopeNamespace
    $rootNode = $doc.ActiveView.ActiveScopeNode  # «Эмулкон»: от него ищутся разделы
    $service = $ns.GetChild($rootNode)

    # --- Служба → Свойства ---
    Log "== Служба → Свойства"
    $doc.ActiveView.ExecuteScopeNodeMenuItem("Свойства", $service)
    $sheet = Wait-Top "#32770" "Свойства: Служба"
    Check ($sheet -ne [IntPtr]::Zero) "окно «Свойства: Служба»"
    $state = [U]::Child($sheet, 100)
    Check ([U]::Text($state) -eq "работает") "состояние: $([U]::Text($state))"

    # Токен управления: выдать.
    [U]::Click([U]::Child($sheet, 130))
    Answer 6 "Выдать новый токен управления"
    $tokDlg = Wait-Top "#32770" "Эмулкон — новый токен"
    Check ($tokDlg -ne [IntPtr]::Zero) "окно нового токена"
    $control = [U]::Text([U]::Child($tokDlg, 200))
    Check ($control -match '^emk_[A-Za-z0-9_-]{43}$') "токен управления: emk_… ($($control.Length) символов)"
    [U]::Click([U]::Child($tokDlg, 201))
    Start-Sleep -Milliseconds 500
    Check ([U]::Clip() -eq $control) "«Копировать» — токен в буфере обмена"
    Check ([U]::Text([U]::Child($tokDlg, 201)) -eq "Скопировано") "кнопка — «Скопировано»"
    [U]::Command($tokDlg, 1)
    [U]::ClearClip()
    Check ((RegValue "ControlTokenHash") -eq (Sha256 $control)) "в реестре — SHA-256 токена, не сам токен"
    Check (WaitFor { (Status "/control/v1/info" $null) -eq 401 }) "Control API без токена — 401"
    Check ((Status "/control/v1/info" $control) -eq 200) "Control API с токеном — 200"
    Check (WaitFor { [U]::Text([U]::Child($sheet, 120)) -eq "задан" } 5) "на странице: управление — задан"

    # Токен устройств: выдать — драйвер без токена не подключается, с токеном — да.
    [U]::Click([U]::Child($sheet, 132))
    Answer 6 "Выдать новый токен устройств"
    $tokDlg = Wait-Top "#32770" "Эмулкон — новый токен"
    $device = [U]::Text([U]::Child($tokDlg, 200))
    Check ($device -match '^emk_' -and $device -ne $control) "токен устройств выдан"
    [U]::Command($tokDlg, 1)
    Check ((RegValue "DeviceTokenHash") -eq (Sha256 $device)) "в реестре — SHA-256 токена устройств"
    Check (WaitFor { (OpenSession "") -eq 401 }) "драйвер без токена — 401"
    Check ((OpenSession $device) -eq 200) "драйвер с токеном устройств — 200"

    # Снять оба.
    [U]::Click([U]::Child($sheet, 133))
    Answer 6 "Снять токен устройств"
    Check (WaitFor { -not (RegValue "DeviceTokenHash") } 10) "токен устройств снят из реестра"
    Check (WaitFor { (OpenSession "") -eq 200 }) "драйвер без токена — снова 200"
    [U]::Click([U]::Child($sheet, 131))
    Answer 6 "Снять токен управления"
    Check (WaitFor { -not (RegValue "ControlTokenHash") } 10) "токен управления снят из реестра"
    Check (WaitFor { (Status "/control/v1/info" $null) -eq 200 }) "Control API без токена — снова 200"
    # Устройства, заведённые тестом, — убрать.
    $devs = @((Invoke-RestMethod "$base/control/v1/devices").devices | Where-Object { $_.workstation_id -eq "UI-LIVE" })
    foreach ($d in $devs) { Invoke-RestMethod "$base/control/v1/devices/$($d.device_id)" -Method Delete | Out-Null }
    Check ($devs.Count -ge 1) "устройство UI-LIVE заведено и удалено"

    # Остановить, запустить, перезапустить.
    [U]::Click([U]::Child($sheet, 141))
    Check (WaitFor { (Get-Service Emulkon).Status -eq "Stopped" }) "«Остановить» — служба остановлена"
    Check (WaitFor { [U]::Text($state) -eq "остановлена" } 10) "на странице: остановлена"
    [U]::Click([U]::Child($sheet, 140))
    Check (WaitFor { Running }) "«Запустить» — служба работает"
    $pid1 = ServicePid
    Check (WaitFor { [U]::Text($state) -eq "работает" } 10) "на странице: работает"
    [U]::Click([U]::Child($sheet, 142))
    Check (WaitFor { (Running) -and (ServicePid) -ne $pid1 }) "«Перезапустить» — новый процесс службы"

    # Кассы в сети: включить и выключить через «Применить» (мастер «Изменить»).
    $net = [U]::Child($sheet, 110)
    if (-not $wasNetwork) {
        [U]::SetCheck($net, $true)
        [U]::Click($net)
        Log "  …  «Применить»: мастер «Изменить» включает сеть"
        [U]::Command($sheet, 0x3021)  # «Применить»
        Answer 6 "Открыть сервер для касс в сети"
        Check (WaitFor { (RegValue "Network") -eq 1 } 120) "Network=1 в реестре"
        Check (WaitFor { (FirewallRule) -and (ListenAll) } 60) "правило брандмауэра есть, служба слушает 0.0.0.0:8737"
        Start-Sleep -Seconds 2
        [U]::SetCheck($net, $false)
        [U]::Click($net)
        [U]::Command($sheet, 0x3021)
        Check (WaitFor { (RegValue "Network") -eq 0 } 120) "Network=0 в реестре"
        Check (WaitFor { -not (FirewallRule) -and -not (ListenAll) -and (Running) } 60) "правила нет, служба только для этого компьютера"
    } else {
        Log "  ---  «Кассы в сети» были включены до теста — переключение пропущено"
    }
    [U]::Command($sheet, 2)  # «Отмена» — закрыть
    Start-Sleep -Seconds 1

    # --- Рабочее место → Свойства: подпись ---
    Log "== Рабочие места → Свойства"
    $node = $ns.GetNext($service)
    $doc.Views.Add($node, 0) | Out-Null
    $v = $doc.ActiveView
    $item = @($v.ListItems)[0]
    $wsId = $v.CellContents($item, 2)
    $v.Select($item)
    $v.DisplaySelectionPropertySheet()
    $wsSheet = Wait-Top "#32770" "Свойства: " -Prefix
    Check ($wsSheet -ne [IntPtr]::Zero) "окно свойств рабочего места $wsId"
    $alias = "UI-тест «подпись»"
    [U]::SetText([U]::Child($wsSheet, 300), $alias)
    [U]::SetText([U]::Child($wsSheet, 301), "заметка UI-теста")
    [U]::Command($wsSheet, 0x3021)
    $ws = $null
    Check (WaitFor { $ws = (Invoke-RestMethod "$base/control/v1/workstations").workstations | Where-Object { $_.workstation_id -eq $wsId }; $ws.alias -eq $alias } 10) "«Применить» — подпись на сервере"
    [U]::SetText([U]::Child($wsSheet, 300), "")
    [U]::SetText([U]::Child($wsSheet, 301), "")
    [U]::Command($wsSheet, 1)  # «ОК» — применить и закрыть
    Check (WaitFor { $ws = (Invoke-RestMethod "$base/control/v1/workstations").workstations | Where-Object { $_.workstation_id -eq $wsId }; -not $ws.alias -and -not $ws.note } 10) "подпись и заметка стёрты"

    # --- Сценарии: выключить, выгрузить, удалить, загрузить ---
    Log "== Сценарии"
    $ruleId = "ui-live-rule"
    $rule = @{ id = $ruleId; name = "UI-тест: нет бумаги"; enabled = $true; match = @{ type = "kkt"; method = "PrintTextDocument" };
               action = @{ type = "error"; error_code = 3; error_text = "UI-тест" } }
    Invoke-RestMethod "$base/control/v1/scenarios" -Method Post -Body (Utf8Body $rule) -ContentType "application/json; charset=utf-8" | Out-Null
    function RuleOnServer { (Invoke-RestMethod "$base/control/v1/scenarios").rules | Where-Object { $_.id -eq $ruleId } }
    $scNode = FindNode "Сценарии"
    $doc.Views.Add($scNode, 0) | Out-Null
    $v = $doc.ActiveView
    $item = FindItem $v 2 $ruleId
    Check ($null -ne $item) "правило в списке; условие: $(if ($item) { $v.CellContents($item, 4) })"
    Check ($item -and $v.CellContents($item, 5) -eq "ошибка 3: UI-тест") "действие словами"
    $v.Select($item)
    $v.ExecuteSelectionMenuItem("Выключить")
    Check (WaitFor { -not (RuleOnServer).enabled } 5) "«Выключить» — на сервере выключено"

    # Окна выбора файла — стандартные Windows (DirectUI, полей ввода для автоматизации не видно): проверяем, что
    # пункт открывает нужное окно и «Отмена» ничего не меняет. Что пишется в файл и как читается — живой тест.
    Async $v "ExecuteScopeNodeMenuItem" "Выгрузить сценарии…" $scNode
    FileDialogCancel "Выгрузить сценарии"
    Wait-Async
    Async $v "ExecuteScopeNodeMenuItem" "Загрузить сценарии…" $scNode
    FileDialogCancel "Загрузить сценарии"
    Wait-Async
    Check ($null -ne (RuleOnServer)) "после «Отмены» правило на месте"

    $v = $doc.ActiveView
    $item = FindItem $v 2 $ruleId
    $v.Select($item)
    Async $v "ExecuteSelectionMenuItem" "Удалить…"
    Answer 6 "Удалить правило"
    Wait-Async
    Check (WaitFor { -not (RuleOnServer) } 5) "«Удалить…» — правила нет на сервере"

    # --- Журнал: отбор, выгрузка ---
    Log "== Журнал"
    $jNode = FindNode "Журнал"
    $doc.Views.Add($jNode, 0) | Out-Null
    $v = $doc.ActiveView
    Check (@($v.ListItems).Count -gt 0) "журнал: строк $(@($v.ListItems).Count)"
    Async $v "ExecuteScopeNodeMenuItem" "Отбор…" $jNode
    $ask = Wait-Top "#32770" "Эмулкон — отбор журнала"
    Check ($ask -ne [IntPtr]::Zero) "окно отбора"
    [U]::SetText([U]::Child($ask, 200), "control.scenario")
    [U]::Command($ask, 1)
    Wait-Async
    $v = $doc.ActiveView
    $rows = @($v.ListItems)
    $methods = @($rows | ForEach-Object { $v.CellContents($_, 6) } | Sort-Object -Unique)
    Check ($rows.Count -ge 3 -and $methods.Count -eq 1 -and $methods[0] -eq "control.scenario") "отбор: $($rows.Count) записей control.scenario"

    Async $v "ExecuteScopeNodeMenuItem" "Выгрузить журнал (с отбором)…" $jNode
    FileDialogCancel "Выгрузить журнал (с отбором)"
    Wait-Async
    $v.ExecuteScopeNodeMenuItem("Сбросить отбор", $jNode)
    Start-Sleep -Seconds 1
    $v = $doc.ActiveView
    $methods = @(@($v.ListItems) | ForEach-Object { $v.CellContents($_, 6) } | Sort-Object -Unique)
    Check ($methods.Count -gt 1) "«Сбросить отбор» — снова все записи"
} catch {
    Check $false "сбой: $_"
} finally {
    try { Invoke-RestMethod "$base/control/v1/scenarios/ui-live-rule" -Method Delete | Out-Null; Log "  восстановлено: удалено правило ui-live-rule" } catch { }
    # Вернуть как было: без токенов, сеть — как до теста.
    $changed = $false
    foreach ($n in "ControlTokenHash", "DeviceTokenHash") {
        if (RegValue $n) { Remove-ItemProperty $serverKey -Name $n; $changed = $true; Log "  восстановлено: снят $n" }
    }
    if (((RegValue "Network") -eq 1) -ne $wasNetwork) {
        $flag = if ($wasNetwork) { "/network" } else { "/nonetwork" }
        Start-Process "C:\Program Files\Emulkon\bin\EmulkonSetup.exe" -ArgumentList "/modify", $flag, "/quiet" -Wait
        Log "  восстановлено: Кассы в сети $flag"
    }
    if ($changed -or -not (Running)) { Restart-Service Emulkon; Log "  восстановлено: служба перезапущена" }
    # Зависшее окно держит консоль: завершить только свой mmc (его PID), чужие консоли не трогать.
    if ($script:stuck -and $mmcPid) { Stop-Process -Id $mmcPid -Force -ErrorAction SilentlyContinue; Log "  завершён свой mmc $mmcPid" }
    elseif ($mmc) {
        try { $mmc.Document.Close($false) } catch { }  # без вопроса «Сохранить параметры консоли?»
        try { $mmc.Quit() } catch { }
    }
    Log "ui-live: $script:passed passed, $script:failed failed"
}
exit ([int]($script:failed -ne 0))
