# results.ps1 — общий отчёт прогонов: какие разделы тестов на каких релизах пройдены.
#
#   tests\1c\results.ps1            # build\report-<ключ>.txt → tests\1c\RESULTS.md
#
# Отчёт одного прогона (run.ps1 → build\report.txt) копируется в build\report-<ключ>.txt; ключи и названия —
# в $Configs ниже, порядок столбцов — тот же. В ячейке: «пройдено/всего» шагов раздела, «пропуск» — раздела
# нет из-за типа оборудования, которого нет в конфигурации, «—» — раздел к конфигурации не относится.
param([string]$Build = (Join-Path $PSScriptRoot "build"), [string]$Out = (Join-Path $PSScriptRoot "RESULTS.md"))

$Configs = [ordered]@{
    "retail23"    = "Розница 2.3.25"
    "retail2326"  = "Розница 2.3.26"
    "retail3"     = "Розница 3.0"
    "unf"         = "УНФ 3.0"
    "rmk"         = "РМК 1.1"
    "ut115"       = "УТ 11.5"
    "ut116"       = "УТ 11.6"
    "ka"          = "КА 2.6"
    "bp"          = "БП 3.0"
    "bpgc"        = "БП ГС 3.0"
    "bpgp"        = "БП ГП 3.0"
    "bpnpo"       = "БП НКО 3.0"
    "bgu"         = "БГУ 2.0"
    "ut103"       = "УТ 10.3"
}

# Конфигурации без полного прогона: причина. Типы оборудования по ним — из build\info-<ключ>.txt (run.ps1 -Mode info).
$NotRun = [ordered]@{
    "bpgc"  = "БСП при старте спрашивает «Запретить открытие внешних отчётов и обработок?» — защитный вопрос оставлен человеку"
    "bpgp"  = "то же"
    "bpnpo" = "то же"
}
# Типы оборудования в порядке перечисления БПО; УТ 10.3 (БПО 3.1, без ДоступныеТипыОборудования) — из прогона.
$TypeOrder = "ККТ", "ОблачнаяККТ", "ПринтерЧеков", "ЭквайринговыйТерминал", "СканерШтрихкода", "СчитывательМагнитныхКарт",
    "ЭлектронныеВесы", "ВесыСПечатьюЭтикеток", "ПринтерЭтикеток", "ДисплейПокупателя", "ТерминалСбораДанных",
    "СчитывательRFID", "УстройствоРаспознавания"
$TypeNames = @{ "ККТ" = "ККТ"; "ОблачнаяККТ" = "Облачная ККТ"; "ПринтерЧеков" = "Принтер чеков"; "ЭквайринговыйТерминал" = "Эквайринговый терминал";
    "СканерШтрихкода" = "Сканер штрихкода"; "СчитывательМагнитныхКарт" = "Считыватель карт"; "ЭлектронныеВесы" = "Электронные весы";
    "ВесыСПечатьюЭтикеток" = "Весы с печатью этикеток"; "ПринтерЭтикеток" = "Принтер этикеток"; "ДисплейПокупателя" = "Дисплей покупателя";
    "ТерминалСбораДанных" = "ТСД"; "СчитывательRFID" = "Считыватель RFID"; "УстройствоРаспознавания" = "Устройство распознавания" }
$Ut103Types = "ККТ", "ПринтерЧеков", "ЭквайринговыйТерминал", "СканерШтрихкода", "СчитывательМагнитныхКарт", "ЭлектронныеВесы",
    "ВесыСПечатьюЭтикеток", "ПринтерЭтикеток", "ДисплейПокупателя", "ТерминалСбораДанных", "СчитывательRFID"

$types = [ordered]@{}
foreach ($key in $Configs.Keys) {
    foreach ($f in @("report-$key.txt", "info-$key.txt")) {
        $path = Join-Path $Build $f
        if ($types.Contains($key) -or -not (Test-Path $path)) { continue }
        $info = @{ Release = ""; Bpo = ""; Types = $null }
        foreach ($line in Get-Content $path -Encoding UTF8) {
            if ($line -match "^СВЕДЕНИЯ (\S+) (\S+); БПО ([^;]+);") { $info.Release = $Matches[2]; $info.Bpo = $Matches[3] }
            elseif ($line -match "^ТИПЫ ([^;]+);") { $info.Types = @($Matches[1] -split ",\s*") }
        }
        if ($key -eq "ut103") { $info.Types = $Ut103Types }
        if ($info.Types) { $types[$key] = $info }
    }
}

$runs = [ordered]@{}
$sections = New-Object System.Collections.Generic.List[string]
foreach ($key in $Configs.Keys) {
    $file = Join-Path $Build "report-$key.txt"
    if (-not (Test-Path $file)) { continue }
    $r = @{ Name = $Configs[$key]; Release = ""; Bpo = ""; Platform = ""; Total = ""; Date = (Get-Item $file).LastWriteTime; Sections = @{} }
    $current = $null
    foreach ($line in Get-Content $file -Encoding UTF8) {
        if ($line -match "^СВЕДЕНИЯ (\S+) (\S+); БПО ([^;]+);.*платформа (\S+)") {
            $r.Release = $Matches[2]; $r.Bpo = $Matches[3]; $r.Platform = $Matches[4]
        } elseif ($line -match "^== (.+)$") {
            $current = $Matches[1].Trim()
            if (-not $sections.Contains($current)) { $sections.Add($current) }
            $r.Sections[$current] = @{ Ok = 0; Err = 0 }
        } elseif ($line -match "^ПРОПУСК «(.+)»") {
            $name = $Matches[1]
            if (-not $sections.Contains($name)) { $sections.Add($name) }
            $r.Sections[$name] = "пропуск"
        } elseif ($line -match "^ИТОГ шагов (\d+), ошибок (\d+)") {
            $r.Total = "$([int]$Matches[1] - [int]$Matches[2])/$($Matches[1])"
        } elseif ($current -and $line -match "^ОК ") {
            $r.Sections[$current].Ok++
        } elseif ($current -and $line -match "^ОШИБКА ") {
            $r.Sections[$current].Err++
        }
    }
    $runs[$key] = $r
}

$cols = @($runs.Values)
$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine("# Прогоны Эмулкона в типовых конфигурациях")
[void]$md.AppendLine()
[void]$md.AppendLine('Собрано `tests/1c/results.ps1` из отчётов прогонов (`tests/1c/README.md`). В ячейке — пройдено/всего шагов')
[void]$md.AppendLine('раздела; «пропуск» — тип оборудования не входит в конфигурацию (`ДоступныеТипыОборудования` БПО);')
[void]$md.AppendLine("«—» — раздел к конфигурации не относится (например, «Чек ККМ» есть только в Рознице и УНФ).")
[void]$md.AppendLine()
[void]$md.AppendLine("| | " + (($cols | % { $_.Name }) -join " | ") + " |")
[void]$md.AppendLine("|---|" + (($cols | % { "---" }) -join "|") + "|")
[void]$md.AppendLine("| Релиз | " + (($cols | % { $_.Release }) -join " | ") + " |")
[void]$md.AppendLine("| БПО | " + (($cols | % { $_.Bpo }) -join " | ") + " |")
[void]$md.AppendLine("| Платформа | " + (($cols | % { $_.Platform }) -join " | ") + " |")
[void]$md.AppendLine("| Дата прогона | " + (($cols | % { $_.Date.ToString("dd.MM.yyyy") }) -join " | ") + " |")
[void]$md.AppendLine("| **Итого** | " + (($cols | % { "**$($_.Total)**" }) -join " | ") + " |")
foreach ($s in $sections) {
    $cells = foreach ($c in $cols) {
        $v = $c.Sections[$s]
        if ($null -eq $v) { "—" }
        elseif ($v -is [string]) { $v }
        elseif ($v.Err -gt 0) { "**$($v.Ok)/$($v.Ok + $v.Err)**" }
        else { "$($v.Ok)/$($v.Ok)" }
    }
    [void]$md.AppendLine("| $s | " + ($cells -join " | ") + " |")
}
if ($NotRun.Count -gt 0) {
    [void]$md.AppendLine()
    [void]$md.AppendLine("Без полного прогона:")
    [void]$md.AppendLine()
    foreach ($k in $NotRun.Keys) { [void]$md.AppendLine("- **$($Configs[$k])** — $($NotRun[$k]).") }
}

# Какое оборудование разрешает конфигурация.
$tcols = @($types.Keys)
[void]$md.AppendLine()
[void]$md.AppendLine("## Оборудование по конфигурациям")
[void]$md.AppendLine()
[void]$md.AppendLine('Типы из `МенеджерОборудования.ДоступныеТипыОборудования()` (строка `ТИПЫ` отчёта); у УТ 10.3 (БПО 3.1) —')
[void]$md.AppendLine("типы, которые БПО знает и прогон подключил. ✓ — тип доступен, · — нет.")
[void]$md.AppendLine()
[void]$md.AppendLine("| Тип | " + (($tcols | % { $Configs[$_] }) -join " | ") + " |")
[void]$md.AppendLine("|---|" + (($tcols | % { ":-:" }) -join "|") + "|")
[void]$md.AppendLine("| *БПО* | " + (($tcols | % { "*$($types[$_].Bpo)*" }) -join " | ") + " |")
foreach ($t in $TypeOrder) {
    $cells = foreach ($k in $tcols) { if ($types[$k].Types -contains $t) { "✓" } else { "·" } }
    [void]$md.AppendLine("| $($TypeNames[$t]) | " + ($cells -join " | ") + " |")
}
[IO.File]::WriteAllText($Out, $md.ToString(), (New-Object Text.UTF8Encoding $false))
Write-Host "записано: $Out ($($cols.Count) конфигураций, $($sections.Count) разделов)"
