# prepare-ut103.ps1 — тестовая копия демо-базы УТ 10.3 для прогона (обычное приложение, БПО 3.1.10).
#
#   tests\1c\prepare-ut103.ps1 [-Base C:\.temp\Base1C\EmulkonTest_UT103]
#
# Толстый клиент молча пропускает /Execute: и на 8.3.27, и на 8.5 обработка не открывается, в журнале
# регистрации пусто. Поэтому в копии базы (только в ней: путь должен содержать EmulkonTest) модуль
# обычного приложения получает крючок: при старте с «emulkon=<путь к epf>» в параметре запуска (/C)
# открывается эта обработка. Для этого с копии снимается поддержка поставщика. Код БПО не меняется.
# Повторный запуск крючок не дублирует.
param(
    [string]$Base = "C:\.temp\Base1C\EmulkonTest_UT103",
    [string]$Template = "D:\.temp\tmplts\1c\trade\10_3_88_3\1Cv8.dt",
    [string]$User = "Федоров (администратор)",
    [string]$IbCmd = "C:\Program Files\1cv8\8.3.27.2325\bin\ibcmd.exe"
)
$ErrorActionPreference = "Stop"
if ($Base -notlike "*EmulkonTest*") { throw "крючок ставится только в тестовую копию (EmulkonTest в пути): $Base" }

function Invoke-IbCmd([string[]]$Arguments) {
    # ibcmd пишет ход работы в stderr: в PowerShell 5.1 при Stop это стало бы исключением.
    $ErrorActionPreference = "Continue"
    & $IbCmd @Arguments 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match "ERROR|ОШИБКА|Ошибка" } | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "ibcmd $($Arguments[0..2] -join ' '): код $LASTEXITCODE" }
}

if (-not (Test-Path (Join-Path $Base "1Cv8.1CD"))) {
    Write-Host "база: создаю из $Template"
    Invoke-IbCmd @("infobase", "create", "--db-path=$Base", "--restore=$Template")
}

$work = Join-Path ([IO.Path]::GetTempPath()) "emulkon-ut103-$PID"
foreach ($d in @("$work\src", "$work\data")) { New-Item -ItemType Directory $d -Force | Out-Null }
try {
    $db = @("--db-path=$Base", "--data=$work\data", "--user=$User")
    Invoke-IbCmd (@("infobase", "config", "export", "objects") + $db + @("--out=$work\src", "Configuration"))
    $file = "$work\src\Ext\OrdinaryApplicationModule.bsl"
    $text = [IO.File]::ReadAllText($file)

    # Крючок между метками: прежний вариант заменяется целиком.
    $begin = "// Эмулкон: крючок начало"; $end = "// Эмулкон: крючок конец"
    # Обработка прогона — без формы: управляемую форму внешней обработки обычное приложение не
    # открывает («не поддерживается»), обычная форма хранится только двоичным Form.bin. Прогон живёт
    # в модуле объекта: Начать(ПараметрЗапуска), затем Шаг() по обработчику ожидания модуля
    # приложения (возвращает паузу до следующего вызова в секундах, 0 — конец), внешние события
    # устройств передаются в ВнешнееСобытие().
    $hook = @"
$begin (tests/1c/prepare-ut103.ps1). Толстый клиент пропускает /Execute,
// поэтому обработка прогона запускается по параметру запуска «emulkon=<путь к epf>;…».
Процедура ОткрытьОбработкуЭмулкона() Экспорт
	ОтключитьОбработчикОжидания("ОткрытьОбработкуЭмулкона");
	Позиция = Найти(ПараметрЗапуска, "emulkon=");
	Если Позиция = 0 Тогда
		Возврат;
	КонецЕсли;
	Путь = Сред(ПараметрЗапуска, Позиция + СтрДлина("emulkon="));
	Позиция = Найти(Путь, ";");
	Если Позиция > 0 Тогда
		Путь = Лев(Путь, Позиция - 1);
	КонецЕсли;
	Попытка
		глОбработкаЭмулкона = ВнешниеОбработки.Создать(Путь, Ложь);
		глОбработкаЭмулкона.Начать(ПараметрЗапуска);
		ПодключитьОбработчикОжидания("ШагОбработкиЭмулкона", 1);
	Исключение
		ОшибкаОбработкиЭмулкона(Путь, ОписаниеОшибки());
	КонецПопытки;
КонецПроцедуры

Процедура ШагОбработкиЭмулкона() Экспорт
	ОтключитьОбработчикОжидания("ШагОбработкиЭмулкона");
	Попытка
		Пауза = глОбработкаЭмулкона.Шаг();
	Исключение
		ОшибкаОбработкиЭмулкона(глОбработкаЭмулкона.ИспользуемоеИмяФайла, ОписаниеОшибки());
		Возврат;
	КонецПопытки;
	Если Пауза > 0 Тогда
		ПодключитьОбработчикОжидания("ШагОбработкиЭмулкона", Пауза);
	Иначе
		ЗавершитьРаботуСистемы(Ложь);
	КонецЕсли;
КонецПроцедуры

// Причина сбоя — рядом с epf, в <путь>.hook.txt: отчёта обработка уже не допишет.
Процедура ОшибкаОбработкиЭмулкона(Путь, Описание)
	Текст = Новый ЗаписьТекста(Путь + ".hook.txt", КодировкаТекста.UTF8);
	Текст.ЗаписатьСтроку(Описание);
	Текст.Закрыть();
	ЗавершитьРаботуСистемы(Ложь);
КонецПроцедуры
$end

"@
    $var = "Перем глОбработкаЭмулкона; // Эмулкон: крючок`n"
    $call = "`n`tПодключитьОбработчикОжидания(""ОткрытьОбработкуЭмулкона"", 1); // Эмулкон: крючок"
    $event = "`n`tЕсли глОбработкаЭмулкона <> Неопределено Тогда глОбработкаЭмулкона.ВнешнееСобытие(Источник, Событие, Данные); КонецЕсли; // Эмулкон: крючок"
    $i = $text.IndexOf($begin)
    if ($i -ge 0) {
        $j = $text.IndexOf($end, $i) + $end.Length
        $text = $text.Substring(0, $i) + $text.Substring($j).TrimStart("`n")
        foreach ($s in @($var, $call, $event)) { $text = $text.Replace($s, "") }
    }
    # Переменные модуля объявляются до процедур — в начало модуля (после BOM).
    $text = $var + $text.TrimStart([char]0xFEFF)
    $start = "Процедура ПриНачалеРаботыСистемы()"
    $i = $text.IndexOf($start)
    if ($i -lt 0) { throw "в модуле обычного приложения нет «$start»" }
    $body = $i + $start.Length
    $text = $text.Substring(0, $i) + $hook.Replace("`r`n", "`n") + $text.Substring($i, $body - $i) + $call + $text.Substring($body)
    $start = "Процедура ОбработкаВнешнегоСобытия(Источник, Событие, Данные)"
    $i = $text.IndexOf($start)
    if ($i -lt 0) { throw "в модуле обычного приложения нет «$start»" }
    $body = $i + $start.Length
    $text = $text.Substring(0, $body) + $event + $text.Substring($body)
    [IO.File]::WriteAllText($file, $text, (New-Object Text.UTF8Encoding $true))

    # Конфигурация на поддержке без права изменения: в копии поддержку снимаем (пересоздаётся из шаблона).
    Invoke-IbCmd (@("infobase", "config", "support", "disable") + $db + @("--force"))
    Invoke-IbCmd (@("infobase", "config", "import", "files") + $db + @("--base-dir=$work\src", "--partial", "Configuration.xml"))
    Invoke-IbCmd (@("infobase", "config", "apply") + $db + @("--force"))
    Write-Host "крючок поставлен: $Base"
} finally {
    [IO.Directory]::Delete($work, $true)
}
