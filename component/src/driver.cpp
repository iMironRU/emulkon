#include "driver.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "methods.h"
#include "text.h"
#include "update.h"

namespace emu {

namespace {

const char* kDefaultServer = "http://127.0.0.1:8737";

std::string computerName() {
#ifdef _WIN32
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) {
        return utf16to8(std::u16string(reinterpret_cast<const char16_t*>(buf), n));
    }
    return "WORKSTATION";
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) == 0) return buf;
    return "WORKSTATION";
#endif
}

// База 1С из командной строки процесса — если параметр «База 1С» пуст (text.h).
std::string infobaseOfProcess() {
#ifdef _WIN32
    const wchar_t* cmd = GetCommandLineW();
    return cmd ? infobaseFromCommandLine(utf16to8(reinterpret_cast<const char16_t*>(cmd))) : std::string();
#else
    return std::string();
#endif
}

std::string attrValue(const std::string& xml, const std::string& name) {
    // Простой поиск атрибута на корневом элементе (ApplicationSettings) — без зависимостей.
    std::string key = name + "=\"";
    size_t p = xml.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    size_t e = xml.find('"', p);
    return e == std::string::npos ? std::string() : xml.substr(p, e - p);
}

}  // namespace

// --- тексты описания и настроек (раздел 2 требований) ---

std::string EmulkonDriver::driverDescriptionXml() {
    const std::string v = EMULKON_VERSION;
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") +
           "<DriverDescription"
           " Name=\"ЭМУЛЯТОР Эмулкон " + v + " (" + kDeveloper + ")\""
           " Description=\"Виртуальные ККТ и эквайринговый терминал сервера эмуляции Эмулкон. НЕ ИСПОЛЬЗОВАТЬ В РАБОЧЕЙ "
           "БАЗЕ: чеки не фискальные, в ОФД не передаются, деньги с карт не списываются. Разработчик: " + kDeveloper + ".\""
           " EquipmentType=\"KKT,POSTerminal,WeighingScales,LabelPrintingScale,ReceiptPrinter,LabelPrinter,BarcodeScanner,CardReader,CustomerDisplay,DataCollectionTerminal,RFIDReader,Recognizer\""
           " IntegrationComponent=\"true\""
           " MainDriverInstalled=\"true\""
           " DriverVersion=\"" + v + "\""
           " IntegrationComponentVersion=\"" + v + "\""
           " DownloadURL=\"\""
           " EnvironmentInformation=\"Основная поставка — сервер эмуляции Эмулкон; журнал обменов ведётся на сервере. "
           "Разработчик: " + kDeveloper + ".\""
           " LogIsEnabled=\"false\""
           " LogPath=\"\""
           " IsEmulator=\"true\""
           " LocalizationSupported=\"false\""
           " AutoSetup=\"false\"/>";
}

std::string EmulkonDriver::settingsXml() {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") +
           "<Settings><Page Caption=\"Эмулкон\"><Group Caption=\"Сервер эмуляции\">"
           "<Parameter Name=\"ServerURL\" Caption=\"Адрес сервера\" TypeValue=\"String\" DefaultValue=\"" +
           kDefaultServer +
           "\" Description=\"Адрес сервера эмуляции Эмулкон, например http://emulkon.local:8737\"/>"
           "<Parameter Name=\"DeviceToken\" Caption=\"Токен устройства\" TypeValue=\"String\" DefaultValue=\"\""
           " Description=\"Токен рабочего места (выдаётся в веб-интерфейсе, вкладка «Рабочие места») или EMULKON_DEVICE_TOKEN сервера; пусто — открытый режим\"/>"
           "<Parameter Name=\"WorkstationID\" Caption=\"Рабочее место\" TypeValue=\"String\" DefaultValue=\"\""
           " Description=\"Идентификатор рабочего места на сервере; пусто — из токена рабочего места или имя компьютера\"/>"
           "<Parameter Name=\"TimeoutMs\" Caption=\"Таймаут, мс\" TypeValue=\"Number\" DefaultValue=\"30000\""
           " Description=\"Сколько ждать ответа сервера на одну команду\"/>"
           "<Parameter Name=\"AutoUpdate\" Caption=\"Обновлять драйвер автоматически\" TypeValue=\"Boolean\""
           " DefaultValue=\"true\" Description=\"Скачивать новую версию драйвера с сервера эмуляции; "
           "она начинает работать после перезапуска 1С\"/>"
           "</Group>"
           // Экземпляр в 1С (ARCHITECTURE §4.3). БПО не передаёт драйверу ни ссылку на экземпляр, ни
           // базу: идентификатор — новый GUID в DefaultValue при каждом запросе, форма «Настройка»
           // сохраняет его при «ОК». Обычные параметры: «только чтение» форма БПО не сохраняет.
           "<Group Caption=\"Экземпляр (заполняет Эмулкон)\">"
           "<Parameter Name=\"InstanceID\" Caption=\"Идентификатор экземпляра\" TypeValue=\"String\" DefaultValue=\"" +
           newRequestId() +
           "\" Description=\"Отличает экземпляры одного типа в базе: каждый — своё устройство на сервере. Не меняйте "
           "и не копируйте в другой экземпляр\"/>"
           "<Parameter Name=\"InstanceName\" Caption=\"Наименование в 1С\" TypeValue=\"String\" DefaultValue=\"\""
           " Description=\"Как экземпляр называется в базе — для веб-интерфейса Эмулкона\"/>"
           "<Parameter Name=\"Infobase\" Caption=\"База 1С\" TypeValue=\"String\" DefaultValue=\"\""
           " Description=\"Какая база подключает устройство — для веб-интерфейса Эмулкона; пусто — из командной строки 1С\"/>"
           "</Group></Page></Settings>";
}

std::string EmulkonDriver::additionalActionsXml() {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") +
           "<Actions><Action Name=\"" + kActionCheckUpdate + "\" Caption=\"Проверить обновление драйвера\"/></Actions>";
}

// --- жизненный цикл ---

EmulkonDriver::EmulkonDriver() = default;
EmulkonDriver::~EmulkonDriver() = default;

// Очередь внешних событий платформы: при переполнении 1С их отбрасывает (требования, раздел 3.8).
constexpr long kEventBufferDepth = 256;

bool EmulkonDriver::Init(void* disp) {
    connection_ = static_cast<IAddInDefBase*>(disp);
    if (connection_) connection_->SetEventBufferDepth(kEventBufferDepth);
    return true;
}

void EmulkonDriver::raiseEvent(const std::string& who, const std::string& name, const std::string& data) {
    if (!connection_) return;
    std::u16string w = utf8to16(who), n = utf8to16(name), d = utf8to16(data);
    connection_->ExternalEvent(const_cast<WCHAR_T*>(reinterpret_cast<const WCHAR_T*>(w.c_str())),
                               const_cast<WCHAR_T*>(reinterpret_cast<const WCHAR_T*>(n.c_str())),
                               const_cast<WCHAR_T*>(reinterpret_cast<const WCHAR_T*>(d.c_str())));
}

bool EmulkonDriver::setMemManager(void* mem) {
    memory_ = static_cast<IMemoryManager*>(mem);
    return memory_ != nullptr;
}

long EmulkonDriver::GetInfo() { return 2000; }

void EmulkonDriver::Done() {
    for (auto& kv : devices_) {
        if (kv.second.events) kv.second.events->stop();  // потоки событий — до того, как уйдёт платформа
    }
    devices_.clear();
    http_.reset();
}

bool EmulkonDriver::RegisterExtensionAs(WCHAR_T** wsExtensionName) {
    *wsExtensionName = allocString(kClassName);
    return *wsExtensionName != nullptr;
}

// --- свойств нет ---

long EmulkonDriver::GetNProps() { return 0; }
long EmulkonDriver::FindProp(const WCHAR_T*) { return -1; }
const WCHAR_T* EmulkonDriver::GetPropName(long, long) { return nullptr; }
bool EmulkonDriver::GetPropVal(const long, tVariant*) { return false; }
bool EmulkonDriver::SetPropVal(const long, tVariant*) { return false; }
bool EmulkonDriver::IsPropReadable(const long) { return false; }
bool EmulkonDriver::IsPropWritable(const long) { return false; }

// --- методы ---

long EmulkonDriver::GetNMethods() { return methodCount(); }

long EmulkonDriver::FindMethod(const WCHAR_T* wsMethodName) {
    if (!wsMethodName) return -1;
    return findMethod(std::u16string(wsMethodName));
}

const WCHAR_T* EmulkonDriver::GetMethodName(const long lMethodNum, const long lMethodAlias) {
    if (lMethodNum < 0 || lMethodNum >= methodCount()) return nullptr;
    const MethodDef& m = methods()[lMethodNum];
    return allocString(lMethodAlias == 0 ? m.en : m.ru);  // 0 — английское имя, 1 — локальное
}

long EmulkonDriver::GetNParams(const long lMethodNum) {
    if (lMethodNum < 0 || lMethodNum >= methodCount()) return 0;
    return methods()[lMethodNum].params;
}

bool EmulkonDriver::GetParamDefValue(const long, const long, tVariant*) { return false; }

bool EmulkonDriver::HasRetVal(const long lMethodNum) { return lMethodNum >= 0 && lMethodNum < methodCount(); }

bool EmulkonDriver::CallAsProc(const long lMethodNum, tVariant* paParams, const long lSizeArray) {
    tVariant ret;
    tVarInit(&ret);
    return CallAsFunc(lMethodNum, &ret, paParams, lSizeArray);
}

bool EmulkonDriver::CallAsFunc(const long lMethodNum, tVariant* pvarRetValue, tVariant* paParams, const long lSizeArray) {
    // Исключения C++ не должны уходить в платформу (требования, раздел 1).
    try {
        return call(lMethodNum, pvarRetValue, paParams, lSizeArray);
    } catch (const std::exception& e) {
        setError(kErrProtocol, std::string("Внутренняя ошибка драйвера: ") + e.what());
    } catch (...) {
        setError(kErrProtocol, "Внутренняя ошибка драйвера");
    }
    if (pvarRetValue) {
        tVarInit(pvarRetValue);
        TV_VT(pvarRetValue) = VTYPE_BOOL;
        TV_BOOL(pvarRetValue) = false;
    }
    return true;
}

void EmulkonDriver::SetLocale(const WCHAR_T*) {}
void EmulkonDriver::SetUserInterfaceLanguageCode(const WCHAR_T*) {}

// --- диспетчер ---

bool EmulkonDriver::call(long method, tVariant* ret, tVariant* params, long count) {
    if (method < 0 || method >= methodCount()) return false;
    const MethodDef& m = methods()[method];
    if (count < m.params) {
        setError(kErrBadParams, "Неверное число параметров");
        fromJson(ret, Json::boolean(false));
        return true;
    }
    bool ok = false;
    switch (m.handling) {
        case Handling::Local: return callLocal(static_cast<int>(m.local), ret, params, count);
        case Handling::Connect: ok = connect(params, count); break;
        case Handling::Disconnect: ok = disconnect(params, count); break;
        case Handling::Test: ok = test(params, count); break;
        case Handling::Forward: return forward(utf16to8(m.en), m.waitsForPerson, ret, params, count);
    }
    fromJson(ret, Json::boolean(ok));
    return true;
}

bool EmulkonDriver::callLocal(int local, tVariant* ret, tVariant* params, long count) {
    switch (static_cast<Local>(local)) {
        case Local::InterfaceRevision:
            fromJson(ret, Json::number(static_cast<int64_t>(kInterfaceRevision)));
            return true;
        case Local::Description:
            setString(&params[0], driverDescriptionXml());
            break;
        case Local::LastError:
            setString(&params[0], lastErrorText_);
            fromJson(ret, Json::number(static_cast<int64_t>(lastErrorCode_)));
            return true;  // GetLastError не сбрасывает последнюю ошибку
        case Local::EquipmentParameters:
            setString(&params[1], settingsXml());
            break;
        case Local::AutoSetup:
            setError(kErrNotSupported, "Автонастройка не поддерживается эмулятором");
            fromJson(ret, Json::boolean(false));
            return true;
        case Local::ApplicationInformation: {
            std::string xml = stringParam(params, count, 0);
            applicationName_ = attrValue(xml, "ApplicationName");
            applicationVersion_ = attrValue(xml, "ApplicationVersion");
            break;
        }
        case Local::AdditionalActions:
            setString(&params[0], additionalActionsXml());
            break;
        case Local::DoAdditionalAction:
            fromJson(ret, Json::boolean(doAction(stringParam(params, count, 0))));
            return true;
        case Local::LocalizationPattern:
            setString(&params[0], "");
            break;
        case Local::LegacySetParameter:
            legacy_[stringParam(params, count, 0)] = stringParam(params, count, 1);
            break;
        case Local::LegacyOpen: {
            bool ok = legacyOpen(params, count);
            fromJson(ret, Json::boolean(ok));
            return true;
        }
        case Local::LegacyClose: {
            bool ok = disconnect(params, count);
            fromJson(ret, Json::boolean(ok));
            return true;
        }
        case Local::LegacyDeviceTest: {
            bool ok = legacyTest(params, count);
            fromJson(ret, Json::boolean(ok));
            return true;
        }
        case Local::LegacyPrintSlipOnTerminal:
            clearError();
            fromJson(ret, Json::boolean(false));
            return true;
        case Local::LegacyParameters:
            setString(&params[0], settingsXml());
            break;
        case Local::SetLocalization:
        case Local::None:
            break;
    }
    clearError();
    fromJson(ret, Json::boolean(true));
    return true;
}

// --- подключение ---

Endpoint EmulkonDriver::endpointFrom(const std::string& connectionParameters, std::string& sanitized) const {
    auto p = parseConnectionParameters(connectionParameters);
    Endpoint ep;
    ep.serverUrl = p.count("ServerURL") && !p["ServerURL"].empty() ? p["ServerURL"] : kDefaultServer;
    ep.deviceToken = p["DeviceToken"];
    ep.workstation = p.count("WorkstationID") && !p["WorkstationID"].empty() ? p["WorkstationID"] : computerName();
    ep.workstationAuto = !p.count("WorkstationID") || p["WorkstationID"].empty();
    if (p.count("TimeoutMs") && !p["TimeoutMs"].empty()) {
        int t = std::atoi(p["TimeoutMs"].c_str());
        if (t > 0) ep.timeoutMs = t;
    }
    ep.instanceId = p["InstanceID"];
    ep.instanceName = p["InstanceName"];
    ep.infobase = !p["Infobase"].empty() ? p["Infobase"] : infobaseOfProcess();
    if (p.count("AutoUpdate")) {
        std::string v = p["AutoUpdate"];
        for (auto& c : v) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        ep.autoUpdate = !(v == "false" || v == "0" || v == "ложь");
    }
    // В журнал сервера уходит копия параметров без токена.
    sanitized = "<Parameters>";
    for (const auto& kv : p) {
        if (kv.first == "DeviceToken") continue;
        sanitized += "<Parameter Name=\"" + xmlAttr(kv.first) + "\" Value=\"" + xmlAttr(kv.second) + "\"/>";
    }
    sanitized += "</Parameters>";
    return ep;
}

bool EmulkonDriver::connect(tVariant* params, long count) {
    // ConnectEquipment(0 DeviceID out, 1 EquipmentType, 2 ConnectionParameters).
    std::string equipmentType = stringParam(params, count, 1);
    std::string sanitized;
    Endpoint ep = endpointFrom(stringParam(params, count, 2), sanitized);
    if (!http_) http_ = makeHttpClient(std::string("Emulkon-Driver/") + EMULKON_VERSION);
    // Тот же InstanceID уже подключён в этом процессе 1С — экземпляр скопировали вместе с
    // параметрами. Копия получает свой ключ «<id>#2» и признак копии (ARCHITECTURE §4.3).
    if (!ep.instanceId.empty()) {
        auto taken = [this](const std::string& id) {
            for (const auto& kv : devices_)
                if (kv.second.client && kv.second.client->endpoint().instanceId == id) return true;
            return false;
        };
        const std::string base = ep.instanceId;
        for (int n = 2; taken(ep.instanceId); n++) {
            ep.instanceId = base + "#" + std::to_string(n);
            ep.instanceCopy = true;
        }
    }

    Device dev;
    dev.equipmentType = equipmentType.empty() ? "KKT" : equipmentType;
    dev.client.reset(new DeviceClient(*http_, ep, dev.equipmentType, EMULKON_VERSION, kInterfaceRevision));
    std::string message;
    if (!dev.client->open(message)) {
        setError(kErrNoConnection, message);
        return false;
    }
    std::string id = "emulkon-" + std::to_string(nextDevice_++);
    Json sent = Json::array();
    sent.push(Json::string(id)).push(Json::string(dev.equipmentType)).push(Json::string(sanitized));
    // Передаём на сервер сам вызов: подключение видно в журнале и модели (docs/protocol.md §1.1).
    // Выходной параметр 0 заполняем сами — своим дескриптором, а не ответом сервера.
    CallResult r = dev.client->call("ConnectEquipment", sent);
    if (!r.transportOk) {
        setError(kErrNoConnection, r.message);
        return false;
    }
    if (!(r.result.isBool() && r.result.b)) {
        setError(r.errorCode, r.errorText);
        return false;
    }
    setString(&params[0], id);
    rememberServer(ep.serverUrl);
    // Новая версия драйвера скачивается в фоне: операция 1С её не ждёт (update.h).
    if (ep.autoUpdate && dev.client->updateOffer().valid()) {
        startBackgroundUpdate(ep.serverUrl, dev.client->updateOffer(), ep.timeoutMs, ep.workstation);
    }
    Device& d = devices_[id] = std::move(dev);
    // События устройства (сканер, считыватель, весы…) — в 1С внешними событиями (events.h).
    d.events.reset(new EventListener(*http_, *d.client,
                                     [this, id](const std::string& name, const std::string& data) { raiseEvent(id, name, data); }));
    d.events->start();
    clearError();
    return true;
}

bool EmulkonDriver::disconnect(tVariant* params, long count) {
    std::string id = stringParam(params, count, 0);
    auto it = devices_.find(id);
    if (it == devices_.end()) {
        setError(kErrNotConnected, "Устройство «" + id + "» не подключено");
        return false;
    }
    if (it->second.events) it->second.events->stop();
    Json sent = Json::array();
    sent.push(Json::string(id));
    it->second.client->call("DisconnectEquipment", sent);  // ответ не важен: отключаемся в любом случае
    devices_.erase(it);
    clearError();
    return true;
}

bool EmulkonDriver::test(tVariant* params, long count) {
    // EquipmentTest(0 EquipmentType, 1 ConnectionParameters, 2 Description out, 3 DemoModeIsActivated out).
    std::string equipmentType = stringParam(params, count, 0);
    std::string sanitized;
    Endpoint ep = endpointFrom(stringParam(params, count, 1), sanitized);
    if (!http_) http_ = makeHttpClient(std::string("Emulkon-Driver/") + EMULKON_VERSION);
    DeviceClient client(*http_, ep, equipmentType.empty() ? "KKT" : equipmentType, EMULKON_VERSION, kInterfaceRevision);
    std::string message;
    if (!client.open(message)) {
        setString(&params[2], message);
        setString(&params[3], "ЭМУЛЯТОР Эмулкон");
        setError(kErrNoConnection, message);
        return false;
    }
    Json sent = Json::array();
    sent.push(Json::string(equipmentType)).push(Json::string(sanitized)).push(Json::string("")).push(Json::string(""));
    tVariant ret;
    tVarInit(&ret);
    // Пробная сессия закрывается в любом исходе (docs/protocol.md §1.1).
    struct CloseOnExit {
        DeviceClient& c;
        ~CloseOnExit() { c.close(); }
    } closeOnExit{client};
    if (!remote(client, "EquipmentTest", sent, &ret, params, count)) return false;
    rememberServer(ep.serverUrl);
    const UpdateOffer& offer = client.updateOffer();
    if (offer.valid()) {
        // Тест устройства — то место, где пользователь читает ответ: говорим и о новой версии.
        std::string note;
        if (updateCached(offer)) {
            note = "Загружена версия драйвера " + offer.version + " — начнёт работать после перезапуска 1С.";
        } else if (ep.autoUpdate) {
            startBackgroundUpdate(ep.serverUrl, offer, ep.timeoutMs, ep.workstation);
            note = "На сервере есть версия драйвера " + offer.version +
                   " — загружается, начнёт работать после перезапуска 1С.";
        } else {
            note = "На сервере есть версия драйвера " + offer.version +
                   " (автообновление выключено): «Проверить обновление драйвера».";
        }
        std::string desc = stringParam(params, count, 2);
        replaceString(&params[2], desc.empty() ? note : desc + "\n" + note);
    }
    return TV_VT(&ret) == VTYPE_BOOL && TV_BOOL(&ret);
}

// --- старая схема подключения (требования до 4.3, БПО УТ 10.3) ---

std::string EmulkonDriver::legacyParametersXml() const {
    std::string xml = "<Parameters>";
    for (const auto& kv : legacy_) {
        if (kv.first == "EquipmentType") continue;
        xml += "<Parameter Name=\"" + xmlAttr(kv.first) + "\" Value=\"" + xmlAttr(kv.second) + "\"/>";
    }
    return xml + "</Parameters>";
}

// freeStrings освобождает строки временных параметров (выделены через менеджер памяти 1С).
void EmulkonDriver::freeStrings(tVariant* v, int n) {
    for (int i = 0; i < n; i++) {
        if (TV_VT(&v[i]) == VTYPE_PWSTR && v[i].pwstrVal && memory_) memory_->FreeMemory(reinterpret_cast<void**>(&v[i].pwstrVal));
        tVarInit(&v[i]);
    }
}

// Подключить(0 DeviceID out) = ConnectEquipment(DeviceID, EquipmentType, параметры из УстановитьПараметр).
bool EmulkonDriver::legacyOpen(tVariant* params, long count) {
    tVariant t[3];
    for (auto& v : t) tVarInit(&v);
    setString(&t[1], legacy_["EquipmentType"]);
    setString(&t[2], legacyParametersXml());
    bool ok = connect(t, 3);
    if (ok && count > 0) setString(&params[0], stringParam(t, 3, 0));
    freeStrings(t, 3);
    return ok;
}

// ТестУстройства(0 Description out, 1 DemoModeIsActivated out) = EquipmentTest по параметрам из УстановитьПараметр.
bool EmulkonDriver::legacyTest(tVariant* params, long count) {
    tVariant t[4];
    for (auto& v : t) tVarInit(&v);
    setString(&t[0], legacy_["EquipmentType"]);
    setString(&t[1], legacyParametersXml());
    setString(&t[2], "");
    setString(&t[3], "");
    bool ok = test(t, 4);
    if (count > 0) setString(&params[0], stringParam(t, 4, 2));
    if (count > 1) setString(&params[1], stringParam(t, 4, 3));
    freeStrings(t, 4);
    return ok;
}

bool EmulkonDriver::doAction(const std::string& name) {
    if (name != kActionCheckUpdate) {
        setError(kErrNotSupported, "Неизвестное дополнительное действие «" + name + "»");
        return false;
    }
    // Параметры подключения БПО дополнительным действиям не передаёт — берём адрес
    // сервера последнего подключения или теста устройства.
    std::string server = rememberedServer();
    if (server.empty()) server = kDefaultServer;
    if (!http_) http_ = makeHttpClient(std::string("Emulkon-Driver/") + EMULKON_VERSION);
    std::string message;
    UpdateCheck r = checkForUpdate(*http_, server, 60000, computerName(), message);
    if (r == UpdateCheck::Failed) {
        setError(kErrNoConnection, message);
        return false;
    }
    notify(message);
    clearError();
    return true;
}

void EmulkonDriver::notify(const std::string& text) {
    if (!connection_) return;
    std::u16string t = utf8to16(text);
    connection_->AddError(ADDIN_E_INFO, u"Эмулкон", t.c_str(), 0);
}

bool EmulkonDriver::forward(const std::string& method, bool waitsForPerson, tVariant* ret, tVariant* params, long count) {
    std::string id = stringParam(params, count, 0);
    auto it = devices_.find(id);
    if (it == devices_.end()) {
        setError(kErrNotConnected, "Устройство «" + id + "» не подключено (вызовите ПодключитьОборудование)");
        fromJson(ret, Json::boolean(false));
        return true;
    }
    Json sent = Json::array();
    for (long i = 0; i < count; ++i) sent.push(toJson(params[i]));
    int timeout = waitsForPerson ? std::max(it->second.client->endpoint().timeoutMs, kPersonWaitMs) : 0;
    if (!remote(*it->second.client, method, sent, ret, params, count, timeout)) fromJson(ret, Json::boolean(false));
    return true;
}

bool EmulkonDriver::remote(DeviceClient& client, const std::string& method, const Json& sent, tVariant* ret,
                          tVariant* params, long count, int timeoutMs) {
    CallResult r = client.call(method, sent, timeoutMs);
    if (!r.transportOk) {
        setError(kErrNoConnection, r.message);
        return false;
    }
    // Записываем обратно только изменённые позиции: это выходные параметры
    // (и входные, если устройство их поменяло). Остальные 1С сохраняет как есть.
    if (r.params.isArray()) {
        long n = static_cast<long>(r.params.items.size());
        for (long i = 0; i < n && i < count; ++i) {
            if (i < static_cast<long>(sent.items.size()) && r.params.items[i] == sent.items[i]) continue;
            fromJson(&params[i], r.params.items[i]);
        }
    }
    fromJson(ret, r.result);
    if (r.result.isBool() && !r.result.b) setError(r.errorCode, r.errorText);
    else clearError();
    return true;
}

// --- ошибки ---

void EmulkonDriver::setError(int code, const std::string& text) {
    lastErrorCode_ = code;
    lastErrorText_ = text;
}

void EmulkonDriver::clearError() {
    lastErrorCode_ = 0;
    lastErrorText_.clear();
}

// --- значения 1С ---

WCHAR_T* EmulkonDriver::allocString(const std::u16string& s) {
    if (!memory_) return nullptr;
    WCHAR_T* p = nullptr;
    unsigned long bytes = static_cast<unsigned long>((s.size() + 1) * sizeof(WCHAR_T));
    if (!memory_->AllocMemory(reinterpret_cast<void**>(&p), bytes) || !p) return nullptr;
    std::memcpy(p, s.c_str(), bytes);
    return p;
}

void EmulkonDriver::replaceString(tVariant* v, const std::string& utf8) {
    if (!v) return;
    if (TV_VT(v) == VTYPE_PWSTR && v->pwstrVal && memory_) memory_->FreeMemory(reinterpret_cast<void**>(&v->pwstrVal));
    setString(v, utf8);
}

void EmulkonDriver::setString(tVariant* v, const std::string& utf8) {
    if (!v) return;
    std::u16string s = utf8to16(utf8);
    tVarInit(v);
    WCHAR_T* p = allocString(s);
    if (!p) return;  // нет менеджера памяти — оставляем пустое значение
    TV_VT(v) = VTYPE_PWSTR;
    v->pwstrVal = p;
    v->wstrLen = static_cast<uint32_t>(s.size());
}

std::string EmulkonDriver::stringParam(tVariant* params, long count, long i) const {
    if (i >= count) return {};
    Json j = toJson(params[i]);
    return j.isString() ? j.text : (j.isNull() ? std::string() : j.dump());
}

Json EmulkonDriver::toJson(const tVariant& v) const {
    switch (TV_VT(&v) & VTYPE_TYPEMASK) {
        case VTYPE_EMPTY:
        case VTYPE_NULL: return Json::null();
        case VTYPE_BOOL: return Json::boolean(v.bVal);
        case VTYPE_I1: return Json::number(static_cast<int64_t>(v.i8Val));
        case VTYPE_I2: return Json::number(static_cast<int64_t>(v.shortVal));
        case VTYPE_I4:
        case VTYPE_ERROR: return Json::number(static_cast<int64_t>(v.lVal));
        case VTYPE_INT: return Json::number(static_cast<int64_t>(v.intVal));
        case VTYPE_I8: return Json::number(static_cast<int64_t>(v.llVal));
        case VTYPE_UI1: return Json::number(static_cast<int64_t>(v.ui8Val));
        case VTYPE_UI2: return Json::number(static_cast<int64_t>(v.ushortVal));
        case VTYPE_UI4: return Json::number(static_cast<int64_t>(v.ulVal));
        case VTYPE_UINT: return Json::number(static_cast<int64_t>(v.uintVal));
        case VTYPE_UI8: return Json::number(static_cast<int64_t>(v.ullVal));
        case VTYPE_R4: return Json::number(static_cast<double>(v.fltVal));
        case VTYPE_R8: return Json::number(v.dblVal);
        case VTYPE_PWSTR:
            if (!v.pwstrVal) return Json::string("");
            return Json::string(utf16to8(std::u16string(v.pwstrVal, v.wstrLen)));
        case VTYPE_PSTR:
            if (!v.pstrVal) return Json::string("");
            return Json::string(std::string(v.pstrVal, v.strLen));
        default: return Json::null();  // даты, двоичные данные — в требованиях к ККТ не используются
    }
}

void EmulkonDriver::fromJson(tVariant* v, const Json& j) {
    if (!v) return;
    switch (j.type) {
        case Json::Type::Null:
            tVarInit(v);
            break;
        case Json::Type::Bool:
            tVarInit(v);
            TV_VT(v) = VTYPE_BOOL;
            TV_BOOL(v) = j.b;
            break;
        case Json::Type::Number:
            tVarInit(v);
            if (j.isInteger()) {
                int64_t n = j.asInt();
                if (n >= INT32_MIN && n <= INT32_MAX) {
                    TV_VT(v) = VTYPE_I4;
                    TV_I4(v) = static_cast<int32_t>(n);
                    break;
                }
            }
            TV_VT(v) = VTYPE_R8;
            TV_R8(v) = j.asDouble();
            break;
        case Json::Type::String:
            setString(v, j.text);
            break;
        default:
            setString(v, j.dump());  // массивы и объекты в ответах не ожидаются
    }
}

}  // namespace emu
