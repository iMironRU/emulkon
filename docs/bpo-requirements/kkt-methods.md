# Методы драйвера: общие (раздел 2) и ККТ (раздел 3.5)

Источники:

- **[Т]** «Требования к разработке драйверов подключаемого оборудования», **версия 4.8**
  (ревизия 4008, редакция 28.08.2026), ИТС: https://its.1c.ru/db/metod8dev#content:4829:hdoc.
  Разделы 1, 2 и 3.5. **Основной источник.**
- **[Р]** Код БПО 3.3.4.30 из выгрузки РМК 1.1.4.105,
  `CommonModules\ЛогированиеОперацийБПОКлиент\Ext\Module.bsl:483-615` — реестр операций
  (для логирования; местами отстаёт от реальных вызовов).
- **[Д]** Там же, `CommonModules\ПодключаемоеОборудованиеДрайверКлиент\Ext\Module.bsl` — реальные
  вызовы `Ждать ОбъектДрайвера.<РусскоеИмя>Асинх(...)`. При расхождении с [Р] прав [Д].

Сверено 2026-10-08: [Т] с [Р] и [Д] по именам, позициям, типам и направлениям.
XML-структуры (CheckPackage, InputParameters, OutputParameters и др.) — [kkt-xml.md](kkt-xml.md).

## Как читать

- Параметры **позиционные**, нумерация с 0. Тип — как в [Т]: STRING, LONG, DOUBLE, BOOL.
  **in** — входной, **out** — выходной (заполняет драйвер).
- Таблицы и структуры передаются **XML-строкой** (STRING, UTF-8, атрибуты на английском) — [Т] раздел 1.
- Возвращаемое значение — **BOOL**, если не указано иное. При `False` БПО вызывает
  `GetLastError` ([Д]:2422) и показывает `Параметры[0]` как текст ошибки.
- Метод, который оборудование не поддерживает, возвращает `False`, а `GetLastError` —
  текст «не поддерживается» ([Т] раздел 1).
- После вызова БПО читает и входные позиции: например, у `ProcessCorrectionCheck` она
  забирает `Параметры[2]` ([Д]:2602). Поэтому сервер возвращает массив целиком.
- Ревизия — число: 4008 = «4.8», 3005 = «3.5» ([Т] примечание к разделу 2). БПО выбирает
  сигнатуры и схему подключения по ревизии, которую вернул `GetInterfaceRevision`.
  **Эмулятор объявляет 4008** и поддерживает только актуальные сигнатуры.

## Раздел 2. Обязательные методы (все типы оборудования)

| Англ. | Рус. (вызов в [Д]) | Параметры | Возврат |
|---|---|---|---|
| GetInterfaceRevision | ПолучитьРевизиюИнтерфейса (:427) | — | **LONG** — ревизия (4008) |
| GetDescription | ПолучитьОписание (:377) | 0 DriverDescription STRING **out** (XML) | BOOL |
| GetLastError | ПолучитьОшибку (:2422) | 0 ErrorDescription STRING **out** | **LONG** — код ошибки |
| EquipmentParameters | ПараметрыОборудования (:543) | 0 EquipmentType STRING in · 1 TableParameters STRING **out** (XML) | BOOL |
| ConnectEquipment | ПодключитьОборудование (:802) | 0 DeviceID STRING **out** · 1 EquipmentType STRING in · 2 ConnectionParameters STRING in (XML) | BOOL |
| DisconnectEquipment | ОтключитьОборудование (:941) | 0 DeviceID STRING in | BOOL |
| EquipmentTest | ТестированиеОборудования (:996) | 0 EquipmentType STRING in · 1 ConnectionParameters STRING in · 2 Description STRING **out** · 3 DemoModeIsActivated STRING **out** | BOOL |
| EquipmentAutoSetup | АвтонастройкаОборудования (:1074) | 0 EquipmentType STRING **out** · 1 ConnectionParameters STRING in · 2 ConnectionParameters STRING **out** · 3 ShowAutoSetupWindow BOOL in · 4 Timeout LONG in (0 — без ограничения) | BOOL |
| SetApplicationInformation | УстановитьИнформациюПриложения (:2402) | 0 ApplicationSettings STRING in (XML: ApplicationName, ApplicationVersion) | BOOL |
| GetAdditionalActions | ПолучитьДополнительныеДействия (:577) | 0 TableActions STRING **out** (XML) | BOOL |
| DoAdditionalAction | ВыполнитьДополнительноеДействие (:1130) | 0 ActionName STRING in | BOOL |
| GetLocalizationPattern | ПолучитьШаблонЛокализации (:442) | 0 LocalizationPattern STRING **out** | BOOL |
| SetLocalization | УстановитьЛокализацию (:360) | 0 LanguageCode STRING in · 1 LocalizationPattern STRING in | BOOL |

**Кто отвечает.** Компонента сама отвечает на методы, которые БПО вызывает до
`ConnectEquipment`: GetInterfaceRevision, GetDescription, EquipmentParameters,
SetApplicationInformation, GetAdditionalActions, локализацию. Адрес сервера в этот
момент ещё неизвестен (см. docs/protocol.md §1.1). `ConnectEquipment`, `EquipmentTest` и
всё остальное уходит на сервер.

**Устаревшая схема.** `SetParameter`, `Open`, `Close`, `DeviceTest`, `GetParameters`,
`ПолучитьНомерВерсии` в [Т] 4.8 отсутствуют. БПО вызывает их только у драйверов с ревизией
≤ 4002 ([Д]:98, 540, 799, 938, 992, 1069). Эмулятор их не реализует.

**Описание драйвера** (`DriverDescription`, [Т] раздел 2). Корневой элемент
`<DriverDescription …/>` с атрибутами:
- обязательные: Name, Description, EquipmentType (EN или RU; можно несколько через «,»),
  IntegrationComponent, MainDriverInstalled, DriverVersion, IntegrationComponentVersion,
  LogIsEnabled, LogPath;
- DownloadURL — обязателен для многокомпонентных драйверов;
- необязательные: **IsEmulator**, LocalizationSupported, AutoSetup, EnvironmentInformation.

БПО читает их в `МенеджерОборудованияКлиент:363`.

**Настройки** (`TableParameters`): `<Settings><Page Caption><Group Caption><Parameter Name
Caption TypeValue(String|Number|Boolean) DefaultValue ReadOnly Description FieldFormat>`,
выбор — `<ChoiceList><Item Value>`. **Параметры подключения** (`ConnectionParameters`):
`<Parameters><Parameter Name Value/></Parameters>`. Неизвестные параметры драйвер обязан игнорировать.

**Архитектура** ([Т] раздел 1): один объект в компоненте, `RegisterExtensionAs` = вторая часть
ProgID. Для ККТ и эквайринга требуется **многокомпонентная** архитектура (интеграционный
компонент + основная поставка). Наш случай естественно ложится на неё: компонента —
интеграционный компонент, «основная поставка» — сервер эмуляции.

## Раздел 3.5. ККТ с передачей в ОФД

У всех методов позиция 0 — `DeviceID` STRING in: идентификатор, который вернул `ConnectEquipment`.

| Англ. | Рус. (вызов в [Д]) | Параметры (кроме 0 DeviceID) |
|---|---|---|
| GetDataKKT | ПолучитьПараметрыККТ (:2851) | 1 TableParametersKKT STRING **out** — регистрационные данные ФН |
| OperationFN | ОперацияФН (:3101) | 1 OperationType LONG in (1 — регистрация, 2 — изменение параметров регистрации, 3 — закрытие ФН) · 2 ParametersFiscal STRING in |
| OpenShift | ОткрытьСмену (:2869) | 1 InputParameters STRING in · 2 OutputParameters STRING **out** |
| CloseShift | ЗакрытьСмену (:2802) | 1 InputParameters STRING in · 2 OutputParameters STRING **out** |
| ProcessCheck | СформироватьЧек (:2546) | 1 Electronically BOOL in · 2 CheckPackage STRING in · 3 DocumentOutputParameters STRING **out** |
| ProcessCorrectionCheck | СформироватьЧекКоррекции (:2596) | 1 Electronically BOOL in · 2 CheckPackage STRING in · 3 DocumentOutputParameters STRING **out** |
| PrintTextDocument | НапечататьТекстовыйДокумент (:2710) | 1 DocumentPackage STRING in |
| CashInOutcome | НапечататьЧекВнесенияВыемки (:2761) | 1 InputParameters STRING in · 2 Amount **DOUBLE** in |
| PrintXReport | НапечататьОтчетБезГашения (:2978) | 1 InputParameters STRING in |
| PrintCheckCopy | НапечататьКопиюЧека (:3150) | 1 CheckNumber STRING in |
| GetCurrentStatus | ПолучитьТекущееСостояние (:3069) | 1 InputParameters STRING in · 2 OutputParameters STRING **out** |
| ReportCurrentStatusOfSettlements | ОтчетОТекущемСостоянииРасчетов (:2938) | 1 InputParameters STRING in · 2 OutputParameters STRING **out** |
| OpenCashDrawer | ОткрытьДенежныйЯщик (:2906) | — |
| GetLineLength | ПолучитьШиринуСтроки (:2462) | 1 LineLength **LONG out** |
| OpenSessionRegistrationKM | ОткрытьСессиюРегистрацииКМ (:3195) | — |
| CloseSessionRegistrationKM | ЗакрытьСессиюРегистрацииКМ (:3238) | — |
| RequestKM | ЗапросКМ (:3305) | 1 RequestKM STRING in · 2 RequestKMResult STRING **out** |
| GetProcessingKMResult | ПолучитьРезультатыЗапросаКМ (:3350) | 1 ProcessingKMResult STRING **out** · 2 RequestStatus **LONG out** (0 — получен, 1 — ещё нет, 2 — не может быть получен) |
| ConfirmKM | ПодтвердитьКМ (:3286) | 1 GUID STRING in (из RequestKM) · 2 ConfirmationType LONG in (0 — КМ войдёт в документ о реализации, 1 — не войдёт) |
| PiotSetChallenge (5.0) | ПодготовитьДанныеАутентификацииПИоТ | 1 IncomingData STRING in (Rsp_get_chlg, Base64 16–32 байт) · 2 OutputData STRING out (Req_chk_chlg с t_hdr 30 байт, Base64) |
| PiotCheckChallenge (5.0) | ИзвлечьТокенАутентификацииПИоТ | 1 IncomingData STRING in (Rsp_chk_chlg, Base64) · 2 OutputData STRING out (X-FN-SID, ASCII до 88 символов) |
| PiotSetDKMsg (5.0) | СформироватьКонтейнерПИоТ | 1 RequestPart STRING in (dk_msg1, 269 байт, Base64) · 2 Hash STRING in (32 байта) · 3 BinaryStructure STRING out (с t_hdr) |
| PiotGetDKMsg (5.0) | РасшифроватьКонтейнерПИоТ | 1 BinaryStructure STRING in (dk_ctrl с t_hdr) · 2 RequestPart out (до 1283 байт) · 3 ArrayNumbers out (rand_ar 16–32 байта) · 4 Hash out (32 байта) |

Поведение из [Т], важное для модели:

- `ProcessCheck` и `ProcessCorrectionCheck` считаются успешными, если чек **фискализирован**.
  Если сломалась только печать бумажной формы, метод возвращает `True`, а в выходных
  параметрах ставит `PrintError = True`.
- `ProcessCheck` с КМ в данных чека формирует ещё и уведомление о реализации маркированного
  товара. Такие КМ должны быть заранее проверены через `RequestKM`.

**Расхождения с реестром БПО [Р]:**
- `ProcessCorrectionCheck`: в [Р] 3 параметра, без Electronically. [Д] вызывает 4-параметровую
  форму при ревизии ≥ 4004 и 3-параметровую ниже ([Д]:2595).
- `ProcessCheck`: в [Р] Electronically указан как in/out, в [Т] — in.
- Ревизии ≤ 3.0 ([Р]:533, 535, 537, 539, 545) — старые сигнатуры с отдельными SessionNumber,
  CheckNumber, FiscalSign. Не поддерживаем.

## Дальше

1. ~~XML-структуры 3.5~~ — готово: [kkt-xml.md](kkt-xml.md).
2. Коды ошибок: [Т] задаёт только механизм (код LONG + текст). На тексты вроде «Нет бумаги»
   и «Открыта крышка» БПО реагирует отдельно — `МенеджерОборудованияКлиент.ЗаполнитьЛокальнуОбработкуОшибок` (:393).
3. Раздел 3.9 (эквайринг) — для этапа 3. В 4.8 добавлен метод `OperationStatus`.
