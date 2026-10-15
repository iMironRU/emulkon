# XML-структуры ККТ (раздел 3.5)

Источники — как в [kkt-methods.md](kkt-methods.md):

- **[Т]** — требования к драйверам 4.8, раздел 3.5 и справочные таблицы;
- **[Б]** — код БПО 3.3.4.30, выгрузка РМК 1.1.4.105, общие модули:
  - `ОборудованиеЧекопечатающиеУстройства` — **ОЧУ**;
  - `ОборудованиеЧекопечатающиеУстройстваКлиентСервер` — **ОЧУ-КС**;
  - `МенеджерОборудованияМаркировкаСлужебныйКлиентСервер` — **МарК-КС**.

Здесь — выжимка для реализации модели, а не копия документа. Описание каждого атрибута —
в [Т]. В этом файле отмечено то, чего в [Т] нет: что БПО реально шлёт, что читает и на чём ломается.

## Общие правила

| Что | Правило | Источник |
|---|---|---|
| Кодировка | UTF-8, объявление `<?xml version="1.0" encoding="UTF-8"?>`, атрибуты на английском | [Т] раздел 1 |
| Булево | БПО пишет `true`/`false` (`XMLСтрока`), а читает через `ВРег(…) = "TRUE"`, т.е. без учёта регистра. Отдаём `true`/`false` | [Б] ОЧУ:1023 и далее |
| Дата и время | xs:dateTime **без часового пояса**: `2026-10-08T12:59:00`. БПО читает через `XMLЗначение(Тип("Дата"), …)` и сама пишет так же | [Т] пример DocumentOutputParameters; [Б] ОЧУ:1685, ОЧУ:4048 |
| Числа | точка как разделитель, без пробелов (`XMLСтрока`) | [Б] |
| Base64 | MarkingCode, Barcode/ValueBase64, все поля GoodCodeData (кроме MI) | [Т] |
| Неизвестные атрибуты | читатель обязан их игнорировать: БПО шлёт и атрибуты вне [Т] (см. ParametersFiscal) | [Т] раздел 2, [Б] |
| Без отступов | БПО после корня сразу берёт следующий узел (`ЧтениеXML.Прочитать()` → ждёт `Parameters`). Отдаём XML без пробелов и переводов строк между элементами | [Б] ОЧУ:1676, ОЧУ-КС:2124 |
| **Пустой числовой атрибут** | БПО делает `Число(ЗначениеАтрибута(…))` без проверки у ShiftState (ОЧУ-КС:2132), PlannedStatus (МарК-КС:119), ResultCode/StatusInfo при наличии атрибута (МарК-КС:204, 208). **Пустая строка там — исключение в 1С.** Числовой атрибут либо со значением, либо отсутствует | [Б] |

Два вида корня:

- **атрибуты на корне** — TableParametersKKT, ParametersFiscal, RequestKM, RequestKMResult,
  ProcessingKMResult: `<RequestKMResult Checking="true" CheckingResult="true"/>`;
- **вложенный `<Parameters …/>`** — InputParameters, OutputParameters, DocumentOutputParameters:
  `<OutputParameters><Parameters ShiftNumber="5" …/></OutputParameters>`.

## Где какая структура

| Структура | Направление | Метод · позиция |
|---|---|---|
| TableParametersKKT | out | GetDataKKT · 1 |
| ParametersFiscal | in | OperationFN · 2 |
| InputParameters | in | OpenShift, CloseShift, GetCurrentStatus, ReportCurrentStatusOfSettlements, PrintXReport, CashInOutcome · 1 |
| OutputParameters | out | OpenShift, CloseShift, GetCurrentStatus, ReportCurrentStatusOfSettlements · 2 |
| CheckPackage | in | ProcessCheck, ProcessCorrectionCheck · 2 |
| DocumentOutputParameters | out | ProcessCheck, ProcessCorrectionCheck · 3 |
| DocumentPackage | in | PrintTextDocument · 1 |
| RequestKM | in | RequestKM · 1 |
| RequestKMResult | out | RequestKM · 2 |
| ProcessingKMResult | out | GetProcessingKMResult · 1 |

## TableParametersKKT — параметры ККТ (out)

Корень `<TableParametersKKT …/>`, атрибуты на корне. БПО принимает и корень `Parameters` (ОЧУ:991).

- **Обязательные всегда:** KKTSerialNumber, Fiscal (bool), FNSerialNumber, IsAutomatic (bool).
- **Обязательные для фискализированного ФН:** KKTNumber, FFDVersionFN (`1.0`, `1.1`, `1.2`),
  FFDVersionKKT (`1.0`, `1.0.5`, `1.1`, `1.2`), DocumentNumber, DateTime (регистрации),
  CompanyName, INN, SaleAddress, SaleLocation, TaxationSystems (коды через «,»), признаки режимов
  (bool): IsOffline, IsEncrypted, IsService, IsExcisable, IsGambling, IsLottery, BSOSing, IsOnline,
  IsAutomaticPrinter, IsMarking, IsPawnshop, IsAssurance, IsVendingMachine, IsCateringServices,
  IsWholesaleTrade, а также AgentTypes (коды через «,»), AutomaticNumber, OFDCompany, OFDCompanyINN.
- **Необязательные:** FirmwareVersion, SupportFNPiot (bool), FNSURL, SenderEmail.
- **БПО читает и то, чего нет в [Т]:** `KKTModelID`, `IsOnlineOnly` (ОЧУ:1015, 1031). Отдаём их тоже.
- Пустой или неизвестный FFDVersion БПО считает «1.1» (ОЧУ:993, 1003). Эмулятор отдаёт `1.2`.

## ParametersFiscal — регистрация ФН (in)

Корень `<ParametersFiscal …/>`, атрибуты на корне. Это атрибуты TableParametersKKT плюс:
CashierName (обяз.), CashierINN (обяз.), FFDVersion (обяз.), RegistrationLabelCodes
(коды причин изменения сведений через «,»; обязателен при OperationType = 2).

БПО ([Б] ОЧУ:908) **не шлёт** FFDVersion, KKTSerialNumber, FNSerialNumber. Шлёт лишний `IsOnlineOnly`,
а при ревизии < 3004 — `RegistrationReasonCode` вместо RegistrationLabelCodes. Значит,
обязательность атрибутов по [Т] модель **не проверяет**, иначе регистрация из БПО не пройдёт.

## InputParameters — параметры операции (in)

`<InputParameters><Parameters …/></InputParameters>`:

| Атрибут | Тип | Обяз. | Примечание |
|---|---|---|---|
| CashierName | string | да | БПО пишет только если кассир заполнен (ОЧУ-КС:1837) — модель не должна падать без него |
| CashierINN | string | | |
| SaleAddress, SaleLocation | string | | |
| PrintRequired | bool | | БПО шлёт при ревизии ≥ 4000 |
| TimeSynchronization | bool | для OpenShift | при `true` — синхронизировать время ККТ с ПК; БПО шлёт при ревизии ≥ 4007 |

## OutputParameters — состояние смены (out)

`<OutputParameters><Parameters …/></OutputParameters>`. Что обязательно, зависит от метода
(**О** — OpenShift, **З** — CloseShift, **С** — GetCurrentStatus, **Р** — ReportCurrentStatusOfSettlements):

| Атрибут | Тип | О | З | С | Р | Что читает БПО (ОЧУ-КС:2125–2153) |
|---|---|---|---|---|---|---|
| ShiftNumber | int | ● | ● | ● | | номер смены |
| CheckNumber | int | | | ● | | номер последнего ФД |
| ShiftClosingCheckNumber | int | | | ● | | номер чека за смену |
| DateTime | datetime | ● | ● | | | дата смены |
| **ShiftState** | int | ● | ● | ● | ● | **`Число()` без проверки — отдавать всегда.** 1 — закрыта, 2 — открыта, 3 — истекла |
| DocumentsCounter | int | | ● | | | количество чеков |
| CountersOperationType1…4 | OperationCounters | | ● | | | не читает |
| CashBalance | double | | ● | ● | ● | остаток наличных |
| BacklogDocumentsCounter | int | | ● | ● | ● | непереданные ФД |
| BacklogDocumentFirstNumber | int | | ● | ● | ● | |
| BacklogDocumentFirstDateTime | datetime | | ● | ● | ● | |
| FNError, FNOverflow, FNFail | bool | ● | ● | | | срочная замена ФН / память переполнена / ресурс исчерпан |
| FNValidityDate | datetime | ● | ● | | | срок действия ФН |

Типы операций в CountersOperationType1…4: 1 — приход, 2 — возврат прихода, 3 — расход,
4 — возврат расхода. **OperationCounters**: CheckCount (int), TotalChecksAmount (double),
CorrectionCheckCount (int), TotalCorrectionChecksAmount (double).

*Уточнить:* как именно вложены CountersOperationTypeN — дочерним элементом
`<CountersOperationType1 CheckCount=… …/>` внутри `Parameters` или иначе. [Т] показывает только
таблицу, БПО этих полей не читает. Эмулятор пишет дочерние элементы — до подтверждения
образцом от реального драйвера.

## DocumentOutputParameters — результат чека (out)

`<DocumentOutputParameters><Parameters …/></DocumentOutputParameters>`. Все обязательны, кроме двух последних:

| Атрибут | Тип | Значение |
|---|---|---|
| ShiftNumber | int | номер смены |
| CheckNumber | int | номер фискального документа |
| ShiftClosingCheckNumber | int | номер чека за смену |
| AddressSiteInspections | string | адрес сайта проверки (`www.nalog.gov.ru`) |
| FiscalSign | string | фискальный признак документа |
| DateTime | datetime | дата и время документа |
| MTNumber | int | номер уведомления о реализации маркированного товара |
| PrintError | bool | бумажная форма не напечаталась, а чек фискализирован (метод при этом вернул True) |

Все поля БПО читает (ОЧУ:1677–1685).

## CheckPackage — чек и чек коррекции (in)

```
CheckPackage
├─ Parameters  (атрибуты чека)
│   ├─ CorrectionData        только для чека коррекции
│   ├─ CustomerDetail
│   ├─ AgentData, VendorData
│   ├─ UserAttribute
│   ├─ OperationalAttribute
│   └─ IndustryAttribute
├─ Positions  (порядок элементов = порядок печати)
│   ├─ FiscalString  (+ AgentData, VendorData, FractionalQuantity, GoodCodeData, IndustryAttribute)
│   ├─ TextString Text
│   └─ Barcode Type ValueBase64
├─ Payments Cash ElectronicPayment PrePayment PostPayment Barter
└─ ElectronicPaymentInfo
    └─ ElectronicPayment* Amount PaymentMethod Identifiers AdditionalInformation
```

**Parameters:**
- обязательные — CashierName, OperationType (1 — приход, 2 — возврат прихода,
  3 — расход, 4 — возврат расхода), TaxationSystem (код из справочника ниже);
- необязательные — CashierINN, CustomerEmail, CustomerPhone, SenderEmail, SaleAddress,
  SaleLocation, AutomatNumber, AgentType, AdditionalAttribute (строка), OperationOnline (bool),
  TimeZone (int; если не передан — часовая зона ККТ), GroupingPositionsWhenPrinting (bool).

**Вложенные элементы Parameters** (атрибуты на элементе):

| Элемент | Атрибуты |
|---|---|
| CorrectionData | Type (0 — самостоятельно, 1 — по предписанию), Date (datetime), Number (номер предписания — с 4006 только при Type = 1), Description. *В [Т] имя «Datе» с кириллической «е» — опечатка; БПО пишет латинское `Date` (ОЧУ:4048)* |
| CustomerDetail | Info, INN, DateOfBirth (`DD.MM.YYYY`), Citizenship (код ОКСМ), DocumentTypeCode (int, ФФД табл. 116), DocumentData, Address |
| AgentData | AgentOperation, AgentPhone, PaymentProcessorPhone, AcquirerOperatorPhone, AcquirerOperatorName, AcquirerOperatorAddress, AcquirerOperatorINN (телефоны — несколько через «,») |
| VendorData | VendorPhone, VendorName, VendorINN |
| UserAttribute | Name, Value |
| OperationalAttribute | DateTime, OperationID (int), OperationData |
| IndustryAttribute | IdentifierFOIV, DocumentDate (`DD.MM.YYYY`), DocumentNumber, AttributeValue (`k=v&k=v`) |

БПО пишет AgentData и VendorData и пустыми (`<AgentData/>`) — пустой элемент означает «нет данных».

**FiscalString:**

| Атрибут | Тип | Обяз. | Примечание |
|---|---|---|---|
| Name | string | да | при печати длинные строки переносятся |
| Quantity | double | да | |
| PriceWithDiscount | double | да | цена единицы со скидками |
| AmountWithDiscount | double | да | итог строки со скидками |
| DiscountAmount | double | | > 0 — скидка, < 0 — наценка |
| Department | int | | |
| VATRate | string | да | `none`, `22`, `20`, `18`, `10`, `7`, `5`, `0`, `22/122`, `20/120`, `18/118`, `10/110`, `7/107`, `5/105` |
| VATAmount | double | да, кроме `none` | ККТ налог не считает, сумму НДС даёт 1С. Без НДС — суммируется AmountWithDiscount |
| PaymentMethod | int | | признак способа расчёта |
| CalculationSubject | int | | признак предмета расчёта |
| CalculationAgent | int | | признак агента по предмету расчёта |
| MeasureOfQuantity | int | | мера количества (ФФД табл. 114; 0 — штуки) |
| MarkingCode | string | | полный КМ в Base64; должен быть заранее проверен (RequestKM → GetProcessingKMResult → ConfirmKM) |
| CountryOfOrigin, CustomsDeclaration, AdditionalAttribute | string | | |
| ExciseAmount | double | | |

Дочерние элементы FiscalString: AgentData, VendorData, IndustryAttribute (как выше),
`FractionalQuantity Numerator Denominator` (дробное количество маркированного товара),
`GoodCodeData` — код товара. У GoodCodeData атрибуты NotIdentified, EAN8, EAN13, ITF14, GS1.0,
GS1.M, KMK, MI, EGAIS20, EGAIS30, F1–F6, все в Base64, кроме MI (КИЗ мехового изделия).

**TextString** `Text` — нефискальная строка. **Barcode** `Type` (EAN8 | EAN13 | CODE39 | QR) +
`ValueBase64`. Если тип не поддерживается моделью — ошибка.

**Payments** (decimal): Cash, ElectronicPayment, PrePayment, PostPayment, Barter.
По [Т] сумма оплат «должна быть больше суммы чека». Это ≥ (переплата — сдача наличными).

**ElectronicPaymentInfo/ElectronicPayment** — каждая безналичная оплата отдельно
(БПО шлёт при ревизии ≥ 4004, ОЧУ:1533):
- Amount — тег 1082;
- PaymentMethod — тег 1236: 1 — НПС, 2 — МПС, 3 — СБП, 6 — прочее (MultiQR);
- Identifiers — тег 1237, `k=v&k=v`, до 256 байт;
- AdditionalInformation — тег 1238, `k=v&k=v`, до 256 байт.

PaymentSystemIdentifier в Identifiers: 4 — Visa, 5 — MasterCard, 6 — UnionPay, 22 — Мир.

Чек успешен, если **фискализирован**. Сбой печати при фискализированном чеке — True + `PrintError="true"`.

## DocumentPackage — нефискальный документ (in)

`<Document><Positions>…</Positions></Document>`, внутри — `TextString Text` и `Barcode Type
ValueBase64` в порядке печати. Все строки нефискальные. Корень — `Document` (сама структура
в [Т] называется DocumentPackage). БПО пишет именно так: ОЧУ-КС:1881–1928. При ревизии < 3003
штрихкод уходит атрибутом `Value` без Base64 — нас это не касается.

## Маркировка: RequestKM, RequestKMResult, ProcessingKMResult

**RequestKM** (in), атрибуты на корне. Пишет — МарК-КС:15.

| Атрибут | Тип | Обяз. | |
|---|---|---|---|
| GUID | string | да | идентификатор запроса, формирует 1С |
| MarkingCode | string | да | полный КМ в Base64 (в КМ бывают разделители GS) |
| PlannedStatus | int | да | планируемый статус товара (ФФД табл. 105) |
| WaitForResult | bool | | ждать ответа ОИСМ. БПО пишет `True`/`False` с заглавной |
| NotSendToServer | bool | | `true` — проверить локально, в ОИСМ не отправлять |
| Quantity | double | | |
| MeasureOfQuantity | int | | ФФД табл. 114 |
| FractionalQuantity | элемент | | `Numerator`, `Denominator` |

**RequestKMResult** (out) — локальная проверка ФН, атрибуты на корне:
`Checking` (bool — КМ проверен ключом ФН), `CheckingResult` (bool — результат проверки).

**ProcessingKMResult** (out) — ответ ОИСМ, атрибуты на корне:

| Атрибут | Тип | Обяз. | |
|---|---|---|---|
| GUID | string | да | из RequestKM |
| Result | bool | да | итог проверки |
| ResultCode | int | да | тег 2106 (ФФД табл. 110) — **битовая маска**: БПО раскладывает её в 8 бит (МарК-КС:205) |
| StatusInfo | int | | 1–3 (ФФД табл. 106); 1 — статус корректен |
| HandleCode | int | да | 0–2 (ФФД табл. 132) |

Сценарий маркировки по [Т]: OpenSessionRegistrationKM → RequestKM → GetProcessingKMResult
опрашивается с интервалом, пока RequestStatus не станет 0 (получен) или 2 (не будет получен);
до этого другие операции невозможны → ConfirmKM (0 — в чек, 1 — отказ) → ProcessCheck с
MarkingCode (сессия закрывается автоматически) или CloseSessionRegistrationKM при отмене чека.

## Справочники

- **TaxationSystem:** 0 — общая, 1 — УСН доход, 2 — УСН доход минус расход,
  3 — ЕНВД (утратила силу), 4 — ЕСХН, 5 — патент.
- **PaymentMethod** (способ расчёта): 1 — предоплата 100 %, 2 — частичная предоплата, 3 — аванс,
  4 — полный расчёт, 5 — частичный расчёт и кредит, 6 — передача в кредит, 7 — оплата кредита.
- **CalculationSubject** (предмет расчёта): 1 — товар, 2 — подакцизный товар, 3 — работа,
  4 — услуга, 10 — платёж, 13 — иной предмет расчёта, 30–33 — маркированные товары
  (30/31 подакцизные без КМ / с КМ, 32/33 прочие без КМ / с КМ). Полный список 1–27, 30–33 — в [Т].
- **AgentType / CalculationAgent:** 0 — банковский платёжный агент, 1 — банковский платёжный
  субагент, 2 — платёжный агент, 3 — платёжный субагент, 4 — поверенный, 5 — комиссионер, 6 — агент.

## Что следует для модели ККТ

1. **Разбор входа — снисходительный.** Неизвестные атрибуты игнорируются, обязательность по [Т]
   строго не проверяется: БПО шлёт не всё, что [Т] называет обязательным
   (ParametersFiscal, CashierName в InputParameters).
2. **Выход — полный и без пустых чисел.** Числовые атрибуты либо со значением, либо отсутствуют;
   ShiftState — всегда; даты — без часового пояса.
3. **Проверки, которые сделала бы настоящая ККТ** (источник сбоев для сценариев):
   - смена открыта и не старше 24 ч;
   - TaxationSystem входит в TaxationSystems регистрации;
   - VATAmount есть, если ставка не `none`;
   - сумма оплат не меньше суммы чека;
   - КМ в чеке прошёл RequestKM и ConfirmKM;
   - тип штрихкода поддерживается.

   Тексты ошибок — те, на которые реагирует БПО (`МенеджерОборудованияКлиент.ЗаполнитьЛокальнуОбработкуОшибок`, :393).
