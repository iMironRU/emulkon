#include "methods.h"

#include "text.h"

namespace emu {

namespace {

const MethodDef kMethods[] = {
    // Раздел 2. Обязательные методы.
    {u"GetInterfaceRevision", u"ПолучитьРевизиюИнтерфейса", 0, Handling::Local, Local::InterfaceRevision},
    {u"GetDescription", u"ПолучитьОписание", 1, Handling::Local, Local::Description},
    {u"GetLastError", u"ПолучитьОшибку", 1, Handling::Local, Local::LastError},
    {u"EquipmentParameters", u"ПараметрыОборудования", 2, Handling::Local, Local::EquipmentParameters},
    {u"ConnectEquipment", u"ПодключитьОборудование", 3, Handling::Connect, Local::None},
    {u"DisconnectEquipment", u"ОтключитьОборудование", 1, Handling::Disconnect, Local::None},
    {u"EquipmentTest", u"ТестированиеОборудования", 4, Handling::Test, Local::None},
    {u"EquipmentAutoSetup", u"АвтонастройкаОборудования", 5, Handling::Local, Local::AutoSetup},
    {u"SetApplicationInformation", u"УстановитьИнформациюПриложения", 1, Handling::Local, Local::ApplicationInformation},
    {u"GetAdditionalActions", u"ПолучитьДополнительныеДействия", 1, Handling::Local, Local::AdditionalActions},
    {u"DoAdditionalAction", u"ВыполнитьДополнительноеДействие", 1, Handling::Local, Local::DoAdditionalAction},
    {u"GetLocalizationPattern", u"ПолучитьШаблонЛокализации", 1, Handling::Local, Local::LocalizationPattern},
    {u"SetLocalization", u"УстановитьЛокализацию", 2, Handling::Local, Local::SetLocalization},

    // Старая схема подключения (до 4.3): так подключает драйвер БПО УТ 10.3
    // (ПодключаемоеОборудованиеУниверсальныйДрайверКлиент). Методы устройств — те же, что ниже.
    {u"SetParameter", u"УстановитьПараметр", 2, Handling::Local, Local::LegacySetParameter},
    {u"Open", u"Подключить", 1, Handling::Local, Local::LegacyOpen},
    {u"Close", u"Отключить", 1, Handling::Local, Local::LegacyClose},
    {u"DeviceTest", u"ТестУстройства", 2, Handling::Local, Local::LegacyDeviceTest},
    {u"GetParameters", u"ПолучитьПараметры", 1, Handling::Local, Local::LegacyParameters},
    // Эквайринг старой схемы: позиционные параметры (0 ИД, 1 НомерКарты, 2 Сумма, 3 НомерЧека, 4 RRN,
    // 5 КодАвторизации, 6 Слип) — сервер переводит в операции 4.3+ (models/acquiring/legacy.go).
    {u"AuthorizeSales", u"ОплатитьПлатежнойКартой", 7, Handling::Forward, Local::None, true},
    {u"AuthorizeRefund", u"ВернутьПлатежПоПлатежнойКарте", 7, Handling::Forward, Local::None, true},
    {u"AuthorizeVoid", u"ОтменитьПлатежПоПлатежнойКарте", 7, Handling::Forward, Local::None},
    {u"AuthorizePreSales", u"ПреавторизацияПоПлатежнойКарте", 7, Handling::Forward, Local::None, true},
    {u"AuthorizeVoidPreSales", u"ОтменитьПреавторизациюПоПлатежнойКарте", 7, Handling::Forward, Local::None},
    {u"AuthorizeCompletion", u"ЗавершитьПреавторизациюПоПлатежнойКарте", 7, Handling::Forward, Local::None},
    // Принтер чеков старой схемы: чек строками (models/printers/legacy.go); дисплей — размер экрана.
    {u"OpenCheck", u"ОткрытьЧек", 6, Handling::Forward, Local::None},
    {u"PrintFiscalString", u"НапечататьФискСтроку", 7, Handling::Forward, Local::None},
    {u"PrintNonFiscalString", u"НапечататьНефискСтроку", 2, Handling::Forward, Local::None},
    {u"PrintBarCode", u"НапечататьШтрихКод", 3, Handling::Forward, Local::None},
    {u"CloseCheck", u"ЗакрытьЧек", 5, Handling::Forward, Local::None},
    {u"CancelCheck", u"ОтменитьЧек", 1, Handling::Forward, Local::None},
    {u"GetOutputParameters", u"ПолучитьПараметрыВывода", 3, Handling::Forward, Local::None},
    // Слипы печатает 1С (на терминале — нет): без ИДУстройства, отвечает компонента.
    {u"PrintSlipOnTerminal", u"ПечатьКвитанцийНаТерминале", 0, Handling::Local, Local::LegacyPrintSlipOnTerminal},

    // Раздел 3.5. ККТ с передачей в ОФД. Параметр 0 — DeviceID.
    {u"GetDataKKT", u"ПолучитьПараметрыККТ", 2, Handling::Forward, Local::None},
    {u"OperationFN", u"ОперацияФН", 3, Handling::Forward, Local::None},
    {u"OpenShift", u"ОткрытьСмену", 3, Handling::Forward, Local::None},
    {u"CloseShift", u"ЗакрытьСмену", 3, Handling::Forward, Local::None},
    {u"ProcessCheck", u"СформироватьЧек", 4, Handling::Forward, Local::None},
    {u"ProcessCorrectionCheck", u"СформироватьЧекКоррекции", 4, Handling::Forward, Local::None},
    {u"PrintTextDocument", u"НапечататьТекстовыйДокумент", 2, Handling::Forward, Local::None},
    {u"CashInOutcome", u"НапечататьЧекВнесенияВыемки", 3, Handling::Forward, Local::None},
    {u"PrintXReport", u"НапечататьОтчетБезГашения", 2, Handling::Forward, Local::None},
    {u"PrintCheckCopy", u"НапечататьКопиюЧека", 2, Handling::Forward, Local::None},
    {u"GetCurrentStatus", u"ПолучитьТекущееСостояние", 3, Handling::Forward, Local::None},
    {u"ReportCurrentStatusOfSettlements", u"ОтчетОТекущемСостоянииРасчетов", 3, Handling::Forward, Local::None},
    {u"OpenCashDrawer", u"ОткрытьДенежныйЯщик", 1, Handling::Forward, Local::None},
    {u"GetLineLength", u"ПолучитьШиринуСтроки", 2, Handling::Forward, Local::None},
    {u"OpenSessionRegistrationKM", u"ОткрытьСессиюРегистрацииКМ", 1, Handling::Forward, Local::None},
    {u"CloseSessionRegistrationKM", u"ЗакрытьСессиюРегистрацииКМ", 1, Handling::Forward, Local::None},
    {u"RequestKM", u"ЗапросКМ", 3, Handling::Forward, Local::None},
    {u"GetProcessingKMResult", u"ПолучитьРезультатыЗапросаКМ", 3, Handling::Forward, Local::None},
    {u"ConfirmKM", u"ПодтвердитьКМ", 3, Handling::Forward, Local::None},
    // Требования 5.0: передача данных ТС ПИоТ через ФН (docs/models/kkt.md, «ТС ПИоТ»).
    {u"PiotSetChallenge", u"ПодготовитьДанныеАутентификацииПИоТ", 3, Handling::Forward, Local::None},
    {u"PiotCheckChallenge", u"ИзвлечьТокенАутентификацииПИоТ", 3, Handling::Forward, Local::None},
    {u"PiotSetDKMsg", u"СформироватьКонтейнерПИоТ", 4, Handling::Forward, Local::None},
    {u"PiotGetDKMsg", u"РасшифроватьКонтейнерПИоТ", 5, Handling::Forward, Local::None},

    // Раздел 3.9. Эквайринговый терминал, XML-методы ревизии ≥ 4003 (pos-terminal.md).
    // Английские имена — как в требованиях, с их опечатками; в «ОплатаCВыдачейНаличных»
    // буква «C» латинская — так её вызывает БПО.
    {u"TerminalParamters", u"ПараметрыТерминала", 2, Handling::Forward, Local::None},
    {u"Pay", u"Оплатить", 4, Handling::Forward, Local::None, true},
    {u"ReturnPayment", u"ВернутьПлатеж", 4, Handling::Forward, Local::None, true},
    {u"CancelPayment", u"ОтменитьПлатеж", 4, Handling::Forward, Local::None},
    {u"PayWithCashWithdrawal", u"ОплатаCВыдачейНаличных", 4, Handling::Forward, Local::None, true},
    {u"PurchaseWithEnrollment", u"ПокупкаСЗачислением", 4, Handling::Forward, Local::None, true},
    {u"Authorisation", u"Преавторизация", 4, Handling::Forward, Local::None, true},
    {u"AuthConfirmation", u"ЗавершитьПреавторизацию", 4, Handling::Forward, Local::None},
    {u"CancelAuthorisation", u"ОтменитьПреавторизацию", 4, Handling::Forward, Local::None},
    {u"PayCertificate", u"ОплатитьСертификатом", 4, Handling::Forward, Local::None, true},
    {u"ReturnCertificate", u"ВернутьСертификатом", 4, Handling::Forward, Local::None, true},
    {u"OperationStatus", u"СтатусОперации", 4, Handling::Forward, Local::None},
    {u"EmergencyReversal", u"АварийнаяОтменаОперации", 1, Handling::Forward, Local::None},
    {u"GetOperationByCards", u"ПолучитьОперацииПоКартам", 2, Handling::Forward, Local::None},
    {u"Settlement", u"ИтогиДняПоКартам", 2, Handling::Forward, Local::None},
    {u"GetCardParametrs", u"ПолучитьПараметрыКарты", 8, Handling::Forward, Local::None, true},

    // Раздел 3.1. Электронные весы (scales.md). ПолучитьВес может ждать, пока вес введут
    // на экране весов (weight_mode=ask).
    {u"ScaleOptions", u"ПараметрыВесов", 2, Handling::Forward, Local::None},
    {u"Calibrate", u"УстановитьВесТары", 2, Handling::Forward, Local::None},
    {u"GetData", u"ПолучитьДанные", 2, Handling::Forward, Local::None},
    {u"GetWeight", u"ПолучитьВес", 2, Handling::Forward, Local::None, true},

    // Раздел 3.2. Весы с печатью этикеток, режим OFFLINE.
    {u"UploadGoods", u"ВыгрузитьТовары", 3, Handling::Forward, Local::None},
    {u"ClearGoods", u"ОчиститьТовары", 1, Handling::Forward, Local::None},
    {u"UploadProductImages", u"ВыгрузитьИзображенияТоваров", 3, Handling::Forward, Local::None},

    // Раздел 3.6. Принтер чеков — те же PrintTextDocument, OpenCashDrawer, GetLineLength, что у ККТ.
    // Раздел 3.7. Принтер этикеток (printers.md).
    {u"InitializePrinter", u"ИнициализацияПринтера", 1, Handling::Forward, Local::None},
    {u"PrintLabels", u"ПечатьЭтикеток", 3, Handling::Forward, Local::None},

    // Раздел 3.3. Дисплей покупателя (display.md).
    {u"GetCustomerDisplayOptions", u"ПолучитьПараметрыДисплеяПокупателя", 2, Handling::Forward, Local::None},
    {u"StringOutputOnCustomerDisplay", u"ВывестиСтрокуНаДисплейПокупателя", 2, Handling::Forward, Local::None},
    {u"ClearCustomerDisplay", u"ОчиститьДисплейПокупателя", 1, Handling::Forward, Local::None},
    {u"AddMediaFile", u"ДобавитьМедиафайл", 3, Handling::Forward, Local::None},
    {u"OutputHTML", u"ВывестиHTML", 2, Handling::Forward, Local::None},
    {u"QRCodeOutputOnCustomerDisplay", u"ВывестиQRКодНаДисплейПокупателя", 3, Handling::Forward, Local::None},
    {u"DisplayOperationStatus", u"ОтобразитьСтатусОперации", 2, Handling::Forward, Local::None},
    {u"DisplayWorkplaceStatus", u"ОтобразитьСтатусРабочегоМеста", 3, Handling::Forward, Local::None},

    // Раздел 3.4. Терминал сбора данных (tsd.md).
    {u"UploadTable", u"ВыгрузитьТаблицу", 3, Handling::Forward, Local::None},
    {u"DownloadTable", u"ЗагрузитьТаблицу", 2, Handling::Forward, Local::None},
    {u"ClearTable", u"ОчиститьТаблицу", 1, Handling::Forward, Local::None},

    // Раздел 3.10. Считыватель RFID (rfid.md). ПолучитьДанныеМетокRFID БПО зовёт синхронно
    // из обработки события RFID.
    {u"OpenSessionRFID", u"ОткрытьСессиюRFID", 1, Handling::Forward, Local::None},
    {u"CloseSessionRFID", u"ЗакрытьСессиюRFID", 1, Handling::Forward, Local::None},
    {u"GetDataTagsRFID", u"ПолучитьДанныеМетокRFID", 3, Handling::Forward, Local::None},
    {u"SaveDataTagRFID", u"ЗаписатьДанныеВМеткуRFID", 6, Handling::Forward, Local::None},

    // Раздел 3.11. Устройство распознавания (recognizer.md).
    {u"RecognizerOptions", u"ПараметрыУстройстваРаспознавания", 2, Handling::Forward, Local::None},
    {u"GetImage", u"ПолучитьИзображение", 2, Handling::Forward, Local::None},
    {u"Recognize", u"Распознать", 4, Handling::Forward, Local::None},
    {u"ConfirmRecognize", u"ПодтвердитьРаспознавание", 2, Handling::Forward, Local::None},
};

}  // namespace

const MethodDef* methods() { return kMethods; }

int methodCount() { return static_cast<int>(sizeof kMethods / sizeof kMethods[0]); }

int findMethod(const std::u16string& name) {
    for (int i = 0; i < methodCount(); ++i) {
        if (equalsIgnoreCase(name, kMethods[i].en) || equalsIgnoreCase(name, kMethods[i].ru)) return i;
    }
    return -1;
}

}  // namespace emu
