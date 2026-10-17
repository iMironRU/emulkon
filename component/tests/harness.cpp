// Стенд «как платформа 1С»: загружает драйвер (Windows — LoadLibrary, Linux — dlopen), создаёт объект
// через GetClassObject и вызывает методы через vtable IComponentBase — так же, как 1С.
// Проверяет ABI (порядок виртуальных методов, __stdcall на x86) и поведение драйвера.
//
//   harness <путь к драйверу>                       — без сервера
//   harness <путь к драйверу> http://127.0.0.1:8737 — плюс сценарии на живом сервере
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AddInDefBase.h"
#include "ComponentBase.h"
#include "IMemoryManager.h"
#include "http.h"      // запросы к Control API — тем же клиентом, что у драйвера (ядро компоненты)
#include "platform.h"  // окружение, временный каталог
#include "text.h"

namespace {

int g_failed = 0;
int g_passed = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::u16string u16(const std::string& s) { return emu::utf8to16(s); }

std::string u8(const char16_t* s, size_t n) { return emu::utf16to8(std::u16string(s, n)); }

void pause(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Загрузка драйвера — как у платформы: Windows — LoadLibrary, Linux — dlopen.
#ifdef _WIN32
using Module = HMODULE;
Module openModule(const std::string& path) {
    std::u16string w = u16(path);
    return LoadLibraryW(reinterpret_cast<LPCWSTR>(w.c_str()));
}
void closeModule(Module m) { FreeLibrary(m); }
void* symbol(Module m, const char* name) { return reinterpret_cast<void*>(GetProcAddress(m, name)); }
std::string loadError() { return "код " + std::to_string(GetLastError()); }
// isLoaded — эта библиотека уже в процессе (переадресация экспортов в копию из кэша).
bool isLoaded(const std::string& path) {
    std::u16string w = u16(path);
    return GetModuleHandleW(reinterpret_cast<LPCWSTR>(w.c_str())) != nullptr;
}
#else
using Module = void*;
Module openModule(const std::string& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void closeModule(Module m) { dlclose(m); }
void* symbol(Module m, const char* name) { return dlsym(m, name); }
std::string loadError() {
    const char* e = dlerror();
    return e ? e : "неизвестная ошибка";
}
bool isLoaded(const std::string& path) {
    Module m = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (m) dlclose(m);
    return m != nullptr;
}
#endif

// --- «платформа» ---

class Memory : public IMemoryManager {
public:
    bool ADDIN_API AllocMemory(void** p, unsigned long n) override {
        *p = std::malloc(n);
        ++allocs;
        return *p != nullptr;
    }
    void ADDIN_API FreeMemory(void** p) override {
        std::free(*p);
        *p = nullptr;
    }
    int allocs = 0;
};

class Platform : public IAddInDefBaseEx {
public:
    bool ADDIN_API AddError(unsigned short, const WCHAR_T* source, const WCHAR_T* descr, long) override {
        std::printf("AddError: %s\n", descr ? u8(descr, std::char_traits<char16_t>::length(descr)).c_str() : "");
        (void)source;
        return true;
    }
    bool ADDIN_API Read(WCHAR_T*, tVariant*, long*, WCHAR_T**) override { return false; }
    bool ADDIN_API Write(WCHAR_T*, tVariant*) override { return false; }
    bool ADDIN_API RegisterProfileAs(WCHAR_T*) override { return true; }
    bool ADDIN_API SetEventBufferDepth(long) override { return true; }
    long ADDIN_API GetEventBufferDepth() override { return 1; }
    // ExternalEvent приходит из фонового потока событий драйвера — копим под замком.
    bool ADDIN_API ExternalEvent(WCHAR_T* who, WCHAR_T* what, WCHAR_T* data) override {
        auto s = [](WCHAR_T* p) { return p ? u8(p, std::char_traits<char16_t>::length(p)) : std::string(); };
        std::lock_guard<std::mutex> lock(mu);
        events.push_back({s(who), s(what), s(data)});
        return true;
    }
    // waitEvent — ждёт событие what от who (до timeoutMs) и возвращает его данные.
    bool waitEvent(const std::string& who, const std::string& what, std::string& data, int timeoutMs = 3000) {
        for (int waited = 0; waited <= timeoutMs; waited += 50) {
            {
                std::lock_guard<std::mutex> lock(mu);
                for (size_t i = 0; i < events.size(); ++i) {
                    if (events[i][0] == who && events[i][1] == what) {
                        data = events[i][2];
                        events.erase(events.begin() + static_cast<long>(i));
                        return true;
                    }
                }
            }
            pause(50);
        }
        return false;
    }
    std::mutex mu;
    std::vector<std::array<std::string, 3>> events;
    void ADDIN_API CleanEventBuffer() override {}
    bool ADDIN_API SetStatusLine(WCHAR_T*) override { return true; }
    void ADDIN_API ResetStatusLine() override {}
    IInterface* ADDIN_API GetInterface(Interfaces) override { return nullptr; }
};

// --- значения ---

struct Value {
    enum Kind { Empty, Bool, Int, Double, String } kind = Empty;
    bool b = false;
    long long i = 0;
    double d = 0;
    std::string s;

    static Value str(const std::string& v) { Value x; x.kind = String; x.s = v; return x; }
    static Value boolean(bool v) { Value x; x.kind = Bool; x.b = v; return x; }
    static Value integer(long long v) { Value x; x.kind = Int; x.i = v; return x; }
    static Value number(double v) { Value x; x.kind = Double; x.d = v; return x; }
};

struct Result {
    bool called = false;
    Value ret;
    std::vector<Value> params;
};

class Component {
public:
    bool load(const std::string& path) {
        dll_ = openModule(path);
        if (!dll_) return false;
        getClassObject_ = reinterpret_cast<GetClassObjectPtr>(symbol(dll_, "GetClassObject"));
        destroyObject_ = reinterpret_cast<DestroyObjectPtr>(symbol(dll_, "DestroyObject"));
        getClassNames_ = reinterpret_cast<GetClassNamesPtr>(symbol(dll_, "GetClassNames"));
        setCaps_ = reinterpret_cast<SetPlatformCapabilitiesPtr>(symbol(dll_, "SetPlatformCapabilities"));
        attachType_ = reinterpret_cast<GetAttachTypePtr>(symbol(dll_, "GetAttachType"));
        return getClassObject_ && destroyObject_ && getClassNames_ && setCaps_ && attachType_;
    }

    std::string classNames() { const WCHAR_T* n = getClassNames_(); return u8(n, std::char_traits<char16_t>::length(n)); }

    bool create(const char16_t* name) {
        setCaps_(eAppCapabilities3);
        if (!getClassObject_(name, &obj_) || !obj_) return false;
        if (!obj_->Init(static_cast<IAddInDefBase*>(&platform_))) return false;
        return obj_->setMemManager(&memory_);
    }

    void destroy() {
        if (obj_) {
            obj_->Done();
            destroyObject_(&obj_);
        }
        if (dll_) closeModule(dll_);
    }

    IComponentBase* obj() { return obj_; }
    Platform& platform() { return platform_; }
    Memory& memory() { return memory_; }
    long attachType() { return attachType_(); }

    std::string takeString(WCHAR_T* p) {
        if (!p) return "(null)";
        std::string s = u8(p, std::char_traits<char16_t>::length(p));
        memory_.FreeMemory(reinterpret_cast<void**>(&p));
        return s;
    }

    Result call(const std::string& name, const std::vector<Value>& args) {
        Result r;
        std::u16string wname = u16(name);
        long m = obj_->FindMethod(wname.c_str());
        if (m < 0) {
            check(false, "метод не найден: " + name);
            return r;
        }
        long n = obj_->GetNParams(m);
        check(n == static_cast<long>(args.size()),
              name + ": GetNParams=" + std::to_string(n) + ", передано " + std::to_string(args.size()));
        std::vector<tVariant> params(n);
        for (long k = 0; k < n; ++k) {
            tVarInit(&params[k]);
            if (k >= static_cast<long>(args.size())) continue;
            const Value& a = args[k];
            switch (a.kind) {
                case Value::Bool: TV_VT(&params[k]) = VTYPE_BOOL; TV_BOOL(&params[k]) = a.b; break;
                case Value::Int: TV_VT(&params[k]) = VTYPE_I4; TV_I4(&params[k]) = static_cast<int32_t>(a.i); break;
                case Value::Double: TV_VT(&params[k]) = VTYPE_R8; TV_R8(&params[k]) = a.d; break;
                case Value::String: {
                    std::u16string s = u16(a.s);
                    void* p = nullptr;
                    memory_.AllocMemory(&p, static_cast<unsigned long>((s.size() + 1) * 2));
                    std::memcpy(p, s.c_str(), (s.size() + 1) * 2);
                    TV_VT(&params[k]) = VTYPE_PWSTR;
                    params[k].pwstrVal = static_cast<WCHAR_T*>(p);
                    params[k].wstrLen = static_cast<uint32_t>(s.size());
                    break;
                }
                case Value::Empty: break;
            }
        }
        tVariant ret;
        tVarInit(&ret);
        r.called = obj_->CallAsFunc(m, &ret, n ? &params[0] : nullptr, n);
        r.ret = fromVariant(ret);
        for (auto& p : params) r.params.push_back(fromVariant(p));
        return r;
    }

private:
    Value fromVariant(tVariant& v) {
        Value x;
        switch (TV_VT(&v)) {
            case VTYPE_BOOL: x.kind = Value::Bool; x.b = TV_BOOL(&v); break;
            case VTYPE_I4: x.kind = Value::Int; x.i = TV_I4(&v); break;
            case VTYPE_R8: x.kind = Value::Double; x.d = TV_R8(&v); break;
            case VTYPE_PWSTR:
                x.kind = Value::String;
                x.s = u8(v.pwstrVal, v.wstrLen);
                memory_.FreeMemory(reinterpret_cast<void**>(&v.pwstrVal));
                break;
            default: break;
        }
        tVarInit(&v);
        return x;
    }

    Module dll_ = nullptr;
    GetClassObjectPtr getClassObject_ = nullptr;
    DestroyObjectPtr destroyObject_ = nullptr;
    GetClassNamesPtr getClassNames_ = nullptr;
    SetPlatformCapabilitiesPtr setCaps_ = nullptr;
    GetAttachTypePtr attachType_ = nullptr;
    IComponentBase* obj_ = nullptr;
    Memory memory_;
    Platform platform_;
};

bool contains(const Value& v, const std::string& what) { return v.kind == Value::String && v.s.find(what) != std::string::npos; }
bool isTrue(const Value& v) { return v.kind == Value::Bool && v.b; }
bool isFalse(const Value& v) { return v.kind == Value::Bool && !v.b; }

std::string show(const Value& v) {
    switch (v.kind) {
        case Value::Bool: return v.b ? "True" : "False";
        case Value::Int: return std::to_string(v.i);
        case Value::Double: return std::to_string(v.d);
        case Value::String: return "\"" + v.s.substr(0, 160) + (v.s.size() > 160 ? "…" : "") + "\"";
        default: return "(пусто)";
    }
}

std::string lastError(Component& c) {
    Result e = c.call("ПолучитьОшибку", {Value::str("")});
    return show(e.ret) + " " + show(e.params[0]);
}

void offline(Component& c) {
    IComponentBase* o = c.obj();
    check(c.classNames() == "EmulkonDriver", "GetClassNames = " + c.classNames());
    WCHAR_T* ext = nullptr;
    check(o->RegisterExtensionAs(&ext), "RegisterExtensionAs");
    check(c.takeString(ext) == "EmulkonDriver", "имя расширения = второй части ProgID");
    check(o->GetInfo() == 2000, "GetInfo");
    check(o->GetNProps() == 0, "свойств нет");
    check(o->GetNMethods() == 99, "методов 99: все типы оборудования 4.8, ТС ПИоТ 5.0 и старая схема");
    check(c.takeString(const_cast<WCHAR_T*>(o->GetMethodName(0, 0))) == "GetInterfaceRevision", "англ. имя метода 0");
    check(c.takeString(const_cast<WCHAR_T*>(o->GetMethodName(0, 1))) == "ПолучитьРевизиюИнтерфейса", "рус. имя метода 0");
    std::u16string lower = u16("сформироватьчек");
    check(o->FindMethod(lower.c_str()) == o->FindMethod(u"ProcessCheck"), "поиск без учёта регистра (кириллица)");
    check(o->HasRetVal(0), "у методов есть возвращаемое значение");

    Result r = c.call("ПолучитьРевизиюИнтерфейса", {});
    check(r.called && r.ret.kind == Value::Int && r.ret.i == 4008, "ревизия 4008: " + show(r.ret));

    r = c.call("ПолучитьОписание", {Value::str("")});
    check(isTrue(r.ret), "ПолучитьОписание → True");
    check(contains(r.params[0], "<DriverDescription") && contains(r.params[0], "IsEmulator=\"true\"") &&
              contains(r.params[0], "ЭМУЛЯТОР") && contains(r.params[0], "EquipmentType=\"KKT,POSTerminal,WeighingScales,LabelPrintingScale,ReceiptPrinter,LabelPrinter,BarcodeScanner,CardReader,CustomerDisplay,DataCollectionTerminal,RFIDReader,Recognizer\"") && contains(r.params[0], "(imiron.ru)\""),
          "описание драйвера: " + show(r.params[0]));

    r = c.call("ПараметрыОборудования", {Value::str("KKT"), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[1], "Name=\"ServerURL\"") && contains(r.params[1], "Name=\"DeviceToken\"") &&
              contains(r.params[1], "Name=\"AutoUpdate\""),
          "таблица параметров: " + show(r.params[1]));
    check(r.params[0].s == "KKT", "входной параметр не изменён");
    // InstanceID — новый GUID при каждом запросе (ARCHITECTURE §4.3): форма БПО сохранит свой.
    {
        auto guidOf = [](const std::string& xml) {
            size_t p = xml.find("Name=\"InstanceID\"");
            size_t d = p == std::string::npos ? p : xml.find("DefaultValue=\"", p);
            return d == std::string::npos ? std::string() : xml.substr(d + 14, 36);
        };
        std::string g1 = guidOf(r.params[1].s);
        Result r2 = c.call("ПараметрыОборудования", {Value::str("KKT"), Value::str("")});
        std::string g2 = guidOf(r2.params[1].s);
        check(g1.size() == 36 && g2.size() == 36 && g1 != g2 && contains(r.params[1], "Name=\"Infobase\"") &&
                  contains(r.params[1], "Name=\"InstanceName\""),
              "идентификатор экземпляра в настройках: " + g1 + " / " + g2);
    }

    r = c.call("ПолучитьДополнительныеДействия", {Value::str("")});
    check(isTrue(r.ret) && contains(r.params[0], "<Actions><Action Name=\"CheckUpdate\""),
          "дополнительные действия: " + show(r.params[0]));
    // Адрес сервера для действия — из кэша (стенд записал туда недоступный адрес).
    r = c.call("ВыполнитьДополнительноеДействие", {Value::str("CheckUpdate")});
    check(isFalse(r.ret) && lastError(c).find("Нет связи") != std::string::npos, "проверка обновления без сервера: " + lastError(c));
    r = c.call("ВыполнитьДополнительноеДействие", {Value::str("Нет")});
    check(isFalse(r.ret) && lastError(c).rfind("903 ", 0) == 0, "неизвестное действие: " + lastError(c));

    r = c.call("ПодключитьОборудование",
               {Value::str(""), Value::str("KKT"),
                Value::str("<Parameters><Parameter Name=\"ServerURL\" Value=\"http://127.0.0.1:9\"/>"
                           "<Parameter Name=\"TimeoutMs\" Value=\"1000\"/></Parameters>")});
    check(isFalse(r.ret), "подключение к недоступному серверу → False");
    std::string err = lastError(c);
    const std::string fullErr = c.call("ПолучитьОшибку", {Value::str("")}).params[0].s;  // lastError обрезает для показа
    check(err.rfind("900 ", 0) == 0 && err.find("Нет связи") != std::string::npos && fullErr.find("не запущен или не установлен") != std::string::npos,
          "ошибка связи с подсказкой: " + err);

    r = c.call("СформироватьЧек", {Value::str("emulkon-99"), Value::boolean(false), Value::str("<CheckPackage/>"), Value::str("")});
    check(isFalse(r.ret), "чек на неподключённом устройстве → False");
    check(lastError(c).rfind("902 ", 0) == 0, "ошибка «не подключено»: " + lastError(c));

    r = c.call("АвтонастройкаОборудования", {Value::str(""), Value::str(""), Value::str(""), Value::boolean(false), Value::integer(0)});
    check(isFalse(r.ret), "автонастройка не поддерживается");
}

// HTTP к Control API (определение ниже).
std::string http(const char* verb, const std::string& url, const std::string& body, int* status);

void online(Component& c, const std::string& server) {
    char ws[64];
    std::snprintf(ws, sizeof ws, "HARNESS-%lld", static_cast<long long>(std::time(nullptr)));
    std::string cp = "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + server +
                     "\"/><Parameter Name=\"WorkstationID\" Value=\"" + ws +
                     "\"/><Parameter Name=\"DeviceToken\" Value=\"\"/></Parameters>";
    const std::string in = "<InputParameters><Parameters CashierName=\"Стенд\"/></InputParameters>";

    Result r = c.call("ТестированиеОборудования", {Value::str("KKT"), Value::str(cp), Value::str(""), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[2], "ЭМУЛЯТОР") && contains(r.params[3], "ЭМУЛЯТОР"),
          "тест устройства: " + show(r.params[2]) + " / " + show(r.params[3]));

    r = c.call("ПодключитьОборудование", {Value::str(""), Value::str("KKT"), Value::str(cp)});
    check(isTrue(r.ret), "подключение: " + lastError(c));
    std::string id = r.params[0].s;
    check(id.rfind("emulkon-", 0) == 0, "DeviceID = " + id);

    r = c.call("ОткрытьСмену", {Value::str(id), Value::str(in), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[2], "ShiftState=\"2\""), "открытие смены: " + show(r.params[2]) + " " + lastError(c));

    const std::string check1 =
        "<CheckPackage><Parameters CashierName=\"Стенд\" OperationType=\"1\" TaxationSystem=\"0\"/><Positions>"
        "<FiscalString Name=\"Товар\" Quantity=\"1\" PriceWithDiscount=\"100\" AmountWithDiscount=\"100\" VATRate=\"20\" VATAmount=\"16.67\"/>"
        "</Positions><Payments Cash=\"100\" ElectronicPayment=\"0\" PrePayment=\"0\" PostPayment=\"0\" Barter=\"0\"/></CheckPackage>";
    r = c.call("СформироватьЧек", {Value::str(id), Value::boolean(false), Value::str(check1), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[3], "<DocumentOutputParameters>") && contains(r.params[3], "FiscalSign="),
          "чек: " + show(r.params[3]) + " " + lastError(c));
    check(r.params[2].s == check1 && isFalse(r.params[1]), "входные параметры чека не переписаны");

    std::string bad = check1;
    bad.replace(bad.find("Cash=\"100\""), 10, "Cash=\"10\"");
    r = c.call("СформироватьЧек", {Value::str(id), Value::boolean(false), Value::str(bad), Value::str("")});
    std::string err = lastError(c);
    check(isFalse(r.ret) && err.rfind("32 ", 0) == 0 && err.find("Сумма оплаты меньше суммы чека") != std::string::npos,
          "ошибка устройства через ПолучитьОшибку: " + err);

    r = c.call("ПолучитьШиринуСтроки", {Value::str(id), Value::integer(0)});
    check(isTrue(r.ret) && r.params[1].kind == Value::Int && r.params[1].i == 42, "ширина ленты: " + show(r.params[1]));

    // ТС ПИоТ (требования 5.0): без признака piot у ФН — отказ, с признаком — контейнер туда и обратно.
    const std::string challenge = "AAAAAAAAAAAAAAAAAAAAAA==";  // 16 нулевых байт
    r = c.call("ПодготовитьДанныеАутентификацииПИоТ", {Value::str(id), Value::str(challenge), Value::str("")});
    check(isFalse(r.ret) && lastError(c).find("ТС ПИоТ") != std::string::npos, "ПИоТ без поддержки ФН: " + lastError(c));
    {
        int status = 0;
        std::string devs = http("GET", server + "/control/v1/devices", "", &status);
        size_t at = devs.find(std::string("\"workstation_id\":\"") + ws + "\"");
        size_t from = at == std::string::npos ? at : devs.rfind("\"device_id\":\"", at);
        std::string dev = from == std::string::npos ? "" : devs.substr(from + 13, devs.find('"', from + 13) - from - 13);
        http("PUT", server + "/control/v1/devices/" + dev + "/state", "{\"piot\":true}", &status);
        check(status == 200, "piot: true для " + dev);
    }
    r = c.call("ПодготовитьДанныеАутентификацииПИоТ", {Value::str(id), Value::str(challenge), Value::str("")});
    check(isTrue(r.ret) && r.params[2].s.size() > 40, "Req_chk_chlg: " + show(r.params[2]) + " " + lastError(c));
    const std::string part = std::string(359, 'A') + "=";  // 269 нулевых байт: 89 групп по 3 и 2 байта
    r = c.call("СформироватьКонтейнерПИоТ", {Value::str(id), Value::str(part),
                                              Value::str("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="), Value::str("")});
    check(isTrue(r.ret), "СформироватьКонтейнерПИоТ: " + lastError(c));
    Result back = c.call("РасшифроватьКонтейнерПИоТ", {Value::str(id), Value::str(r.params[3].s), Value::str(""), Value::str(""), Value::str("")});
    check(isTrue(back.ret) && back.params[4].s == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", "РасшифроватьКонтейнерПИоТ: " + lastError(c));

    r = c.call("ЗакрытьСмену", {Value::str(id), Value::str(in), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[2], "ShiftState=\"1\""), "закрытие смены: " + show(r.params[2]));
    check(lastError(c).rfind("0 ", 0) == 0, "успех сбрасывает ошибку: " + lastError(c));

    r = c.call("ВыполнитьДополнительноеДействие", {Value::str("CheckUpdate")});
    check(isTrue(r.ret), "проверка обновления на сервере: " + lastError(c));

    r = c.call("ОтключитьОборудование", {Value::str(id)});
    check(isTrue(r.ret), "отключение");
    r = c.call("ОткрытьСмену", {Value::str(id), Value::str(in), Value::str("")});
    check(isFalse(r.ret), "после отключения устройство недоступно");
}

// --- эквайринг на живом сервере ---

// http — синхронный запрос к Control API: стенд отвечает на экране терминала, как трей.
std::string http(const char* verb, const std::string& url, const std::string& body, int* status) {
    static std::unique_ptr<emu::HttpClient> client = emu::makeHttpClient("Emulkon-Harness");
    const std::string v = verb;
    emu::HttpResponse r = v == "POST"     ? client->post(url, body, {}, 30000)
                          : v == "PUT"    ? client->put(url, body, {}, 30000)
                          : v == "PATCH"  ? client->patch(url, body, {}, 30000)
                          : v == "DELETE" ? client->remove(url, {}, 30000)
                                          : client->get(url, {}, 30000);
    *status = r.status;
    return r.body;
}

// field — значение первого строкового поля "key":"…" в JSON или атрибута key="…" в XML.
std::string field(const std::string& text, const std::string& key, bool xmlAttr = false) {
    std::string k = xmlAttr ? " " + key + "=\"" : "\"" + key + "\":\"";
    size_t p = text.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    return text.substr(p, text.find('"', p) - p);
}

void acquiring(Component& c, const std::string& server) {
    char ws[64];
    std::snprintf(ws, sizeof ws, "HARNESS-ACQ-%lld", static_cast<long long>(std::time(nullptr)));
    std::string cp = "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + server + "\"/><Parameter Name=\"WorkstationID\" Value=\"" +
                     ws + "\"/></Parameters>";
    Result r = c.call("ТестированиеОборудования", {Value::str("POSTerminal"), Value::str(cp), Value::str(""), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[2], "эквайрингового терминала"), "тест терминала: " + show(r.params[2]));
    r = c.call("ПодключитьОборудование", {Value::str(""), Value::str("POSTerminal"), Value::str(cp)});
    check(isTrue(r.ret), "подключение терминала: " + lastError(c));
    const std::string id = r.params[0].s;
    r = c.call("ПараметрыТерминала", {Value::str(id), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[1], "TerminalID=\"EMU") && contains(r.params[1], "PrintSlipOnTerminal=\"false\""),
          "параметры терминала: " + show(r.params[1]));

    // Оплата ждёт карту на экране терминала — отвечаем через Control API, как трей.
    const std::string in = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><OperationParameters RequestID=\"0000BEEF\" MerchantNumber=\"0\" "
                           "UseBiometrics=\"0\" AuthorizationType=\"0\" Amount=\"111\" AmountCash=\"0\" AmountOriginalTransaction=\"0\"/>";
    Result pay;
    std::thread payer([&] { pay = c.call("Оплатить", {Value::str(id), Value::str(in), Value::str(""), Value::str("")}); });
    std::string promptId, deviceId;
    int status = 0;
    for (int i = 0; i < 100 && promptId.empty(); ++i) {
        pause(100);
        std::string j = http("GET", server + "/control/v1/journal?method=device.prompt&workstation_id=" + ws + "&limit=1", "", &status);
        promptId = field(j, "prompt_id");
        deviceId = field(j, "device_id");
    }
    check(!promptId.empty(), "экран терминала открыл запрос «приложите карту»");
    std::string ans = http("POST", server + "/control/v1/devices/" + deviceId + "/prompt",
                           "{\"prompt_id\":\"" + promptId + "\",\"action\":\"card\",\"card\":\"mastercard\"}", &status);
    check(status == 200, "ответ с экрана: HTTP " + std::to_string(status) + " " + ans);
    payer.join();
    check(isTrue(pay.ret) && contains(pay.params[2], "PaymentSystemIdentifier=\"5\"") && contains(pay.params[3], "ОДОБРЕНО"),
          "оплата картой: " + show(pay.params[2]) + " " + lastError(c));
    const std::string rrn = field(pay.params[2].s, "RRNCode", true);
    check(rrn.size() == 12, "RRN оплаты: " + rrn);

    std::string cancel = in;
    cancel.replace(cancel.find("/>"), 2, " RRNCode=\"" + rrn + "\"/>");
    r = c.call("ОтменитьПлатеж", {Value::str(id), Value::str(cancel), Value::str(""), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[2], rrn), "отмена оплаты по RRN: " + lastError(c));
    r = c.call("ИтогиДняПоКартам", {Value::str(id), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[1], "СВЕРКА ИТОГОВ"), "сверка итогов: " + show(r.params[1]));
    r = c.call("ОтключитьОборудование", {Value::str(id)});
    check(isTrue(r.ret), "отключение терминала");
}

// number — числовое значение из выходного параметра (R8 или I4).
double number(const Value& v) { return v.kind == Value::Double ? v.d : v.kind == Value::Int ? static_cast<double>(v.i) : -1; }

// deviceOf — устройство рабочего места ws по последней записи журнала с методом method.
std::string deviceOf(const std::string& server, const std::string& ws, const std::string& method) {
    int status = 0;
    for (int i = 0; i < 20; ++i) {
        std::string j = http("GET", server + "/control/v1/journal?method=" + method + "&workstation_id=" + ws + "&limit=1", "", &status);
        std::string id = field(j, "device_id");
        if (!id.empty()) return id;
        pause(100);
    }
    return {};
}

// --- экземпляры 1С на живом сервере (ARCHITECTURE §4.3) ---

void instances(Component& c, const std::string& server) {
    char ws[64], inst[64];
    std::snprintf(ws, sizeof ws, "HARNESS-I-%lld", static_cast<long long>(std::time(nullptr)));
    std::snprintf(inst, sizeof inst, "inst-%lld", static_cast<long long>(std::time(nullptr)));
    auto cp = [&](const std::string& id) {
        return "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + server + "\"/><Parameter Name=\"WorkstationID\" Value=\"" +
               ws + "\"/><Parameter Name=\"InstanceID\" Value=\"" + id +
               "\"/><Parameter Name=\"Infobase\" Value=\"Стенд\"/><Parameter Name=\"InstanceName\" Value=\"Касса стенда\"/></Parameters>";
    };
    Result a = c.call("ПодключитьОборудование", {Value::str(""), Value::str("KKT"), Value::str(cp(inst))});
    Result b = c.call("ПодключитьОборудование", {Value::str(""), Value::str("KKT"), Value::str(cp(std::string(inst) + "-b"))});
    // Тот же InstanceID вторым подключением — копия экземпляра.
    Result copy = c.call("ПодключитьОборудование", {Value::str(""), Value::str("KKT"), Value::str(cp(inst))});
    check(isTrue(a.ret) && isTrue(b.ret) && isTrue(copy.ret), "три экземпляра ККТ: " + lastError(c));

    int status = 0;
    std::string j = http("GET", server + "/control/v1/devices", "", &status);
    auto deviceWith = [&](const std::string& id) {
        size_t p = j.find("\"instance_id\":\"" + id + "\"");
        if (p == std::string::npos) return std::string();
        size_t s = j.rfind("\"device_id\":", p);
        size_t e = j.find("\"device_id\":", p);
        return j.substr(s, e == std::string::npos ? std::string::npos : e - s);
    };
    std::string da = deviceWith(inst), db = deviceWith(std::string(inst) + "-b"), dc = deviceWith(std::string(inst) + "#2");
    check(!da.empty() && !db.empty() && !dc.empty() && field(da, "device_id") != field(db, "device_id") &&
              field(da, "device_id") != field(dc, "device_id"),
          "свои устройства на сервере: " + field(da, "device_id") + ", " + field(db, "device_id") + ", " + field(dc, "device_id"));
    check(da.find("\"infobase\":\"Стенд\"") != std::string::npos && da.find("\"keyed_by\":\"instance\"") != std::string::npos &&
              dc.find("\"copy\":true") != std::string::npos,
          "база, ключ и копия: " + da.substr(0, 300));
    for (const Result* r : {&a, &b, &copy}) c.call("ОтключитьОборудование", {Value::str(r->params[0].s)});
}

// Токен рабочего места (docs/protocol.md §1.1): место без параметра WorkstationID берётся из
// токена; без токена место с токеном не пускают, причина видна в ошибке.
void workstationToken(Component& c, const std::string& server) {
    char ws[64];
    std::snprintf(ws, sizeof ws, "HARNESS-T-%lld", static_cast<long long>(std::time(nullptr)));
    int status = 0;
    std::string tok = field(http("POST", server + "/control/v1/workstations/" + ws + "/token", "", &status), "token");
    check(status == 200 && tok.rfind("emk_ws_", 0) == 0, "выдача токена рабочему месту");
    auto cp = [&](const std::string& extra) {
        return "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + server + "\"/>" + extra + "</Parameters>";
    };
    Result a = c.call("ПодключитьОборудование",
                      {Value::str(""), Value::str("BarcodeScanner"), Value::str(cp("<Parameter Name=\"DeviceToken\" Value=\"" + tok + "\"/>"))});
    check(isTrue(a.ret), "подключение по токену без рабочего места: " + lastError(c));
    std::string j = http("GET", server + "/control/v1/journal?method=session.open&workstation_id=" + ws + "&limit=1", "", &status);
    check(j.find("\"auth\":\"workstation\"") != std::string::npos && j.find(tok) == std::string::npos,
          "место из токена, токен не в журнале: " + j.substr(0, 300));
    Result b = c.call("ПодключитьОборудование",
                      {Value::str(""), Value::str("BarcodeScanner"), Value::str(cp(std::string("<Parameter Name=\"WorkstationID\" Value=\"") + ws + "\"/>"))});
    std::string err = c.call("ПолучитьОшибку", {Value::str("")}).params[0].s;  // целиком, без обрезки show()
    check(isFalse(b.ret) && err.find("отклонил токен") != std::string::npos && err.find("выдан токен") != std::string::npos,
          "место с токеном без токена: " + err);
    // Тест устройства закрывает свою пробную сессию, отключение — свою (docs/protocol.md §1.1).
    Result t = c.call("ТестированиеОборудования", {Value::str("BarcodeScanner"),
                                                  Value::str(cp("<Parameter Name=\"DeviceToken\" Value=\"" + tok + "\"/>")),
                                                  Value::str(""), Value::str("")});
    check(isTrue(t.ret), "тест устройства по токену: " + lastError(c));
    c.call("ОтключитьОборудование", {Value::str(a.params[0].s)});
    std::string sess = http("GET", server + "/control/v1/sessions", "", &status);
    check(status == 200 && sess.find(std::string("\"workstation_id\":\"") + ws + "\"") == std::string::npos,
          "после теста и отключения сессий места нет: " + sess.substr(0, 300));
    http("DELETE", server + "/control/v1/workstations/" + ws + "/token", "", &status);
    check(status == 200, "отзыв токена");
}

// Старая схема подключения (требования до 4.3, БПО УТ 10.3): УстановитьПараметр по одному,
// Подключить, методы устройства, ТестУстройства, Отключить.
void legacyScheme(Component& c, const std::string& server) {
    char ws[64];
    std::snprintf(ws, sizeof ws, "HARNESS-OLD-%lld", static_cast<long long>(std::time(nullptr)));
    auto set = [&](const char* name, const std::string& value) {
        Result r = c.call("УстановитьПараметр", {Value::str(name), Value::str(value)});
        check(isTrue(r.ret), std::string("УстановитьПараметр ") + name);
    };
    set("EquipmentType", "ККТ");
    set("ServerURL", server);
    set("WorkstationID", ws);
    Result p = c.call("ПолучитьПараметры", {Value::str("")});
    check(isTrue(p.ret) && contains(p.params[0], "Name=\"ServerURL\""), "ПолучитьПараметры — таблица параметров");
    Result t = c.call("ТестУстройства", {Value::str(""), Value::str("")});
    check(isTrue(t.ret) && contains(t.params[0], "ЭМУЛЯТОР"), "ТестУстройства: " + show(t.params[0]) + " " + lastError(c));
    Result r = c.call("Подключить", {Value::str("")});
    check(isTrue(r.ret) && r.params[0].s.rfind("emulkon-", 0) == 0, "Подключить → ИДУстройства: " + show(r.params[0]) + " " + lastError(c));
    const std::string id = r.params[0].s;
    Result w = c.call("ПолучитьШиринуСтроки", {Value::str(id), Value::integer(0)});
    check(isTrue(w.ret) && w.params[1].kind == Value::Int && w.params[1].i > 0, "ККТ по старой схеме: ширина ленты " + show(w.params[1]));
    int status = 0;
    std::string j = http("GET", server + "/control/v1/journal?method=session.open&workstation_id=" + ws + "&limit=1", "", &status);
    check(j.find("\"equipment_type\":\"ККТ\"") != std::string::npos, "сессия рабочего места из УстановитьПараметр: " + j.substr(0, 200));
    Result d = c.call("Отключить", {Value::str(id)});
    check(isTrue(d.ret), "Отключить: " + lastError(c));

    // Эквайринг старыми именами: оплата и отмена позиционными параметрами.
    set("EquipmentType", "ЭквайринговыйТерминал");
    r = c.call("Подключить", {Value::str("")});
    check(isTrue(r.ret), "терминал по старой схеме: " + lastError(c));
    const std::string term = r.params[0].s;
    {
        std::string devs = http("GET", server + "/control/v1/devices", "", &status);
        std::string dev;
        const std::string key = std::string("\"workstation_id\":\"") + ws + "\"";
        for (size_t at = devs.find(key); at != std::string::npos && dev.empty(); at = devs.find(key, at + 1)) {
            size_t from = devs.rfind("\"device_id\":\"", at);
            std::string id = from == std::string::npos ? "" : devs.substr(from + 13, devs.find('"', from + 13) - from - 13);
            if (id.rfind("acquiring-", 0) == 0) dev = id;
        }
        http("PUT", server + "/control/v1/devices/" + dev + "/state", "{\"card_mode\":\"approve\"}", &status);
        check(status == 200, "автоодобрение для " + dev);
    }
    Result s = c.call("ПечатьКвитанцийНаТерминале", {});
    check(isFalse(s.ret), "ПечатьКвитанцийНаТерминале → Ложь: слипы печатает 1С");
    Result pay = c.call("ОплатитьПлатежнойКартой", {Value::str(term), Value::str(""), Value::integer(100), Value::str(""),
                                                    Value::str(""), Value::str(""), Value::str("")});
    check(isTrue(pay.ret) && !pay.params[4].s.empty() && contains(pay.params[6], "ЭМУЛЯТОР"),
          "ОплатитьПлатежнойКартой: RRN " + show(pay.params[4]) + " " + lastError(c));
    Result voidOp = c.call("ОтменитьПлатежПоПлатежнойКарте", {Value::str(term), pay.params[1], Value::integer(100), Value::str(""),
                                                              pay.params[4], pay.params[5], Value::str("")});
    check(isTrue(voidOp.ret), "ОтменитьПлатежПоПлатежнойКарте: " + lastError(c));
    c.call("Отключить", {Value::str(term)});

    // Принтер чеков: чек строками; дисплей: размер экрана.
    set("EquipmentType", "ПринтерЧеков");
    r = c.call("Подключить", {Value::str("")});
    check(isTrue(r.ret), "принтер чеков по старой схеме: " + lastError(c));
    const std::string prn = r.params[0].s;
    Result oc = c.call("ОткрытьЧек", {Value::str(prn), Value::boolean(false), Value::boolean(false), Value::boolean(true),
                                       Value::integer(0), Value::integer(0)});
    check(isTrue(oc.ret) && number(oc.params[4]) > 0, "ОткрытьЧек → номер " + show(oc.params[4]) + " " + lastError(c));
    check(isTrue(c.call("НапечататьНефискСтроку", {Value::str(prn), Value::str("СТАРАЯ СХЕМА")}).ret), "НапечататьНефискСтроку");
    check(isTrue(c.call("ЗакрытьЧек", {Value::str(prn), Value::integer(0), Value::integer(0), Value::integer(0), Value::integer(0)}).ret),
          "ЗакрытьЧек: " + lastError(c));
    c.call("Отключить", {Value::str(prn)});
    set("EquipmentType", "ДисплейПокупателя");
    r = c.call("Подключить", {Value::str("")});
    const std::string disp = r.params[0].s;
    Result op = c.call("ПолучитьПараметрыВывода", {Value::str(disp), Value::integer(0), Value::integer(0)});
    check(isTrue(op.ret) && number(op.params[1]) == 20 && number(op.params[2]) == 2,
          "ПолучитьПараметрыВывода: " + show(op.params[1]) + "×" + show(op.params[2]) + " " + lastError(c));
    c.call("Отключить", {Value::str(disp)});
}

// --- весы на живом сервере ---

void scales(Component& c, const std::string& server) {
    char ws[64];
    std::snprintf(ws, sizeof ws, "HARNESS-SCALES-%lld", static_cast<long long>(std::time(nullptr)));
    std::string cp = "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + server + "\"/><Parameter Name=\"WorkstationID\" Value=\"" +
                     ws + "\"/></Parameters>";
    Result r = c.call("ПодключитьОборудование", {Value::str(""), Value::str("WeighingScales"), Value::str(cp)});
    check(isTrue(r.ret), "подключение весов: " + lastError(c));
    const std::string id = r.params[0].s;
    r = c.call("ПараметрыВесов", {Value::str(id), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[1], "Calibrate=\"true\"") && contains(r.params[1], "WeighingEvent=\"false\""),
          "параметры весов: " + show(r.params[1]));

    // Вес «кладут» через Control API — как кнопкой в веб-интерфейсе.
    int status = 0;
    const std::string dev = deviceOf(server, ws, "ScaleOptions");
    http("PUT", server + "/control/v1/devices/" + dev + "/state", "{\"weight\":1.75}", &status);
    check(status == 200, "вес на платформе: HTTP " + std::to_string(status));
    r = c.call("УстановитьВесТары", {Value::str(id), Value::number(0.25)});
    check(isTrue(r.ret), "тара 0.25: " + lastError(c));
    r = c.call("ПолучитьВес", {Value::str(id), Value::number(0)});
    check(isTrue(r.ret) && number(r.params[1]) == 1.5, "вес нетто 1.5: " + show(r.params[1]) + " " + lastError(c));
    r = c.call("ПолучитьДанные", {Value::str(id), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[1], "Weight=\"1.5\"") && contains(r.params[1], "Stable=\"true\""),
          "данные весов: " + show(r.params[1]));

    // Режим ask: ПолучитьВес ждёт, пока вес введут на экране весов (как в трее).
    http("PUT", server + "/control/v1/devices/" + dev + "/state", "{\"weight_mode\":\"ask\"}", &status);
    Result w;
    std::thread weigher([&] { w = c.call("ПолучитьВес", {Value::str(id), Value::number(0)}); });
    std::string promptId;
    for (int i = 0; i < 100 && promptId.empty(); ++i) {
        pause(100);
        std::string j = http("GET", server + "/control/v1/journal?method=device.prompt&workstation_id=" + ws + "&limit=1", "", &status);
        promptId = field(j, "prompt_id");
    }
    check(!promptId.empty(), "экран весов открыл запрос «положите товар»");
    std::string ans = http("POST", server + "/control/v1/devices/" + dev + "/prompt",
                           "{\"prompt_id\":\"" + promptId + "\",\"action\":\"weigh\",\"value\":0.532}", &status);
    check(status == 200, "вес с экрана: HTTP " + std::to_string(status) + " " + ans);
    weigher.join();
    check(isTrue(w.ret) && number(w.params[1]) == 0.532, "вес с экрана весов: " + show(w.params[1]) + " " + lastError(c));
    http("PUT", server + "/control/v1/devices/" + dev + "/state", "{\"stable\":false,\"weight_mode\":\"manual\"}", &status);
    r = c.call("ПолучитьВес", {Value::str(id), Value::number(0)});
    check(isFalse(r.ret) && lastError(c).rfind("71 ", 0) == 0, "нестабильный вес — ошибка 71: " + lastError(c));
    r = c.call("ОтключитьОборудование", {Value::str(id)});
    check(isTrue(r.ret), "отключение весов");

    // Весы с печатью этикеток: выгрузка товаров, этикетка с весов.
    r = c.call("ПодключитьОборудование", {Value::str(""), Value::str("LabelPrintingScale"), Value::str(cp)});
    check(isTrue(r.ret), "подключение весов с печатью этикеток: " + lastError(c));
    const std::string lid = r.params[0].s;
    const std::string goods = "<Table><Record PLU=\"1\" Code=\"42\" IsWeightGoods=\"true\" Name=\"Сыр\" Price=\"500\" ShelfLife=\"30\"/></Table>";
    r = c.call("ВыгрузитьТовары", {Value::str(lid), Value::str(goods), Value::str("last")});
    check(isTrue(r.ret), "выгрузка товаров: " + lastError(c));
    r = c.call("ВыгрузитьТовары", {Value::str(lid), Value::str(goods), Value::str("middle")});
    check(isFalse(r.ret) && lastError(c).rfind("81 ", 0) == 0, "неверный статус пакета — 81: " + lastError(c));
    const std::string ldev = deviceOf(server, ws, "UploadGoods");
    std::string label = http("POST", server + "/control/v1/devices/" + ldev + "/actions/print_label", "{\"plu\":1,\"weight\":0.25}", &status);
    check(status == 200 && label.find("2100042002500") != std::string::npos, "этикетка с весовым штрихкодом: " + label);
    r = c.call("ОчиститьТовары", {Value::str(lid)});
    check(isTrue(r.ret), "очистка товаров: " + lastError(c));
    r = c.call("ОтключитьОборудование", {Value::str(lid)});
    check(isTrue(r.ret), "отключение весов с печатью этикеток");
}

// --- принтеры на живом сервере ---

void printers(Component& c, const std::string& server) {
    char ws[64];
    std::snprintf(ws, sizeof ws, "HARNESS-PRINTERS-%lld", static_cast<long long>(std::time(nullptr)));
    std::string cp = "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + server + "\"/><Parameter Name=\"WorkstationID\" Value=\"" +
                     ws + "\"/></Parameters>";
    Result r = c.call("ТестированиеОборудования", {Value::str("ReceiptPrinter"), Value::str(cp), Value::str(""), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[2], "принтера чеков"), "тест принтера чеков: " + show(r.params[2]));
    r = c.call("ПодключитьОборудование", {Value::str(""), Value::str("ReceiptPrinter"), Value::str(cp)});
    check(isTrue(r.ret), "подключение принтера чеков: " + lastError(c));
    const std::string id = r.params[0].s;
    r = c.call("ПолучитьШиринуСтроки", {Value::str(id), Value::integer(0)});
    check(isTrue(r.ret) && number(r.params[1]) == 42, "ширина строки принтера: " + show(r.params[1]));
    const std::string doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Document><Positions><TextString Text=\"Товарный чек\"/>"
                            "<Barcode Type=\"EAN13\" ValueBase64=\"MjAwMDAyMTI2MjE1Nw==\"/></Positions></Document>";
    r = c.call("НапечататьТекстовыйДокумент", {Value::str(id), Value::str(doc)});
    check(isTrue(r.ret), "печать на принтере чеков: " + lastError(c));
    r = c.call("ОткрытьДенежныйЯщик", {Value::str(id)});
    check(isTrue(r.ret), "денежный ящик: " + lastError(c));
    int status = 0;
    std::string j = http("GET", server + "/control/v1/journal?method=device.print&workstation_id=" + ws + "&limit=1", "", &status);
    check(j.find("[EAN13: 2000021262157]") != std::string::npos, "документ принтера чеков в журнале");
    r = c.call("СформироватьЧек", {Value::str(id), Value::boolean(false), Value::str("<CheckPackage/>"), Value::str("")});
    check(isFalse(r.ret) && lastError(c).rfind("99 ", 0) == 0, "фискальный метод на принтере чеков — 99: " + lastError(c));
    r = c.call("ОтключитьОборудование", {Value::str(id)});
    check(isTrue(r.ret), "отключение принтера чеков");

    r = c.call("ПодключитьОборудование", {Value::str(""), Value::str("LabelPrinter"), Value::str(cp)});
    check(isTrue(r.ret), "подключение принтера этикеток: " + lastError(c));
    const std::string lid = r.params[0].s;
    r = c.call("ИнициализацияПринтера", {Value::str(lid)});
    check(isTrue(r.ret), "инициализация принтера этикеток: " + lastError(c));
    const std::string first = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Data><Formatting Width=\"58\" Height=\"40\">"
                              "<Text FieldName=\"Имя\" Left=\"0\" Top=\"0\" Width=\"58\" Height=\"10\" FontSize=\"9\"/>"
                              "<Barcode FieldName=\"ШК\" Type=\"EAN13\" Left=\"0\" Top=\"12\" Height=\"15\" PrintHRI=\"true\" FontSize=\"8\"/>"
                              "</Formatting><Labels><Label Quantity=\"2\"><Record FieldName=\"Имя\" Value=\"Ручка\"/>"
                              "<Record FieldName=\"ШК\" Value=\"4600000000008\"/></Label></Labels></Data>";
    const std::string next = "<Data><Labels><Label><Record FieldName=\"Имя\" Value=\"Карандаш\"/>"
                             "<Record FieldName=\"ШК\" Value=\"4008110271538\"/></Label></Labels></Data>";
    // Как шлёт БПО 3.3.4.30: разметка только в первом пакете, второй тоже со статусом first.
    const std::string packages[] = {first, next, next};
    const char* statuses[] = {"first", "first", "last"};
    for (int i = 0; i < 3; ++i) {
        r = c.call("ПечатьЭтикеток", {Value::str(lid), Value::str(packages[i]), Value::str(statuses[i])});
        check(isTrue(r.ret), "пакет этикеток " + std::to_string(i + 1) + " (" + statuses[i] + "): " + lastError(c));
    }
    r = c.call("ПечатьЭтикеток", {Value::str(lid), Value::str(next), Value::str("regular")});
    check(isFalse(r.ret) && lastError(c).rfind("94 ", 0) == 0, "пакет без разметки после last — 94: " + lastError(c));
    j = http("GET", server + "/control/v1/journal?method=device.print&workstation_id=" + ws + "&limit=3", "", &status);
    check(j.find("\"layout\"") != std::string::npos && j.find("ШК: [EAN13: 4008110271538]") != std::string::npos,
          "этикетки в журнале с разметкой");
    r = c.call("ОтключитьОборудование", {Value::str(lid)});
    check(isTrue(r.ret), "отключение принтера этикеток");
}

// listeners — сколько компонент слушают события устройства (из /control/v1/devices).
int listeners(const std::string& server, const std::string& dev) {
    int status = 0;
    std::string j = http("GET", server + "/control/v1/devices", "", &status);
    size_t p = j.find("\"device_id\":\"" + dev + "\"");
    if (p == std::string::npos) return -1;
    size_t end = j.find("\"device_id\":", p + 1);
    std::string obj = j.substr(p, end == std::string::npos ? std::string::npos : end - p);
    size_t k = obj.find("\"event_listeners\":");
    return k == std::string::npos ? -1 : std::atoi(obj.c_str() + k + 18);
}

bool waitListeners(const std::string& server, const std::string& dev, int want) {
    for (int i = 0; i < 60; ++i) {
        if (listeners(server, dev) == want) return true;
        pause(50);
    }
    return false;
}

// --- устройства ввода: события сервера → ExternalEvent в 1С ---

void inputs(Component& c, const std::string& server) {
    char ws[64];
    std::snprintf(ws, sizeof ws, "HARNESS-INPUT-%lld", static_cast<long long>(std::time(nullptr)));
    std::string cp = "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + server + "\"/><Parameter Name=\"WorkstationID\" Value=\"" +
                     ws + "\"/></Parameters>";
    Result r = c.call("ПодключитьОборудование", {Value::str(""), Value::str("СканерШтрихкода"), Value::str(cp)});
    check(isTrue(r.ret), "подключение сканера: " + lastError(c));
    const std::string id = r.params[0].s;
    const std::string dev = deviceOf(server, ws, "ConnectEquipment");
    check(waitListeners(server, dev, 1), "драйвер слушает события сканера " + dev);
    int status = 0;
    std::string res = http("POST", server + "/control/v1/devices/" + dev + "/actions/scan", "{\"barcode\":\"4600000000008\"}", &status);
    check(status == 200, "скан в панели: HTTP " + std::to_string(status) + " " + res);
    std::string data;
    check(c.platform().waitEvent(id, "Barcode", data) && data == "4600000000008", "ExternalEvent(" + id + ", Barcode): " + data);
    // код маркировки с разделителем GS доходит как есть
    res = http("POST", server + "/control/v1/devices/" + dev + "/actions/scan", "{\"barcode\":\"0104600000000008215abc\\u001d93dGhK\"}", &status);
    check(c.platform().waitEvent(id, "Barcode", data) && data == std::string("0104600000000008215abc\x1d") + "93dGhK",
          "код маркировки с GS: " + data);

    r = c.call("ПодключитьОборудование", {Value::str(""), Value::str("СчитывательМагнитныхКарт"), Value::str(cp)});
    check(isTrue(r.ret), "подключение считывателя карт: " + lastError(c));
    const std::string cid = r.params[0].s;
    const std::string cdev = deviceOf(server, ws, "ConnectEquipment");
    check(cdev != dev && waitListeners(server, cdev, 1), "драйвер слушает события считывателя " + cdev);
    http("POST", server + "/control/v1/devices/" + cdev + "/actions/swipe", "{\"card\":\"discount\"}", &status);
    check(c.platform().waitEvent(cid, "TracksData", data) && data == "7770000000017=2912\r", "ExternalEvent(TracksData): " + data);

    r = c.call("ОтключитьОборудование", {Value::str(id)});
    check(isTrue(r.ret) && waitListeners(server, dev, 0), "после отключения сканер никто не слушает");
    r = c.call("ОтключитьОборудование", {Value::str(cid)});
    check(isTrue(r.ret), "отключение считывателя");
}

// --- дисплей, ТСД, RFID, распознавание ---

void others(Component& c, const std::string& server) {
    char ws[64];
    std::snprintf(ws, sizeof ws, "HARNESS-OTHER-%lld", static_cast<long long>(std::time(nullptr)));
    std::string cp = "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + server + "\"/><Parameter Name=\"WorkstationID\" Value=\"" +
                     ws + "\"/></Parameters>";
    int status = 0;
    auto connectType = [&](const char* type, std::string& dev) {
        Result r = c.call("ПодключитьОборудование", {Value::str(""), Value::str(type), Value::str(cp)});
        check(isTrue(r.ret), std::string("подключение ") + type + ": " + lastError(c));
        dev = deviceOf(server, ws, "ConnectEquipment");
        return r.params[0].s;
    };

    // дисплей покупателя: параметры, строки, нажатие на сенсорный экран — событие OnClick
    std::string dev;
    std::string id = connectType("ДисплейПокупателя", dev);
    Result r = c.call("ПолучитьПараметрыДисплеяПокупателя", {Value::str(id), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[1], "Columns=\"20\"") && contains(r.params[1], "DisplayHTML=\"true\""),
          "параметры дисплея: " + show(r.params[1]));
    r = c.call("ВывестиСтрокуНаДисплейПокупателя", {Value::str(id), Value::str("Молоко 89.90\nИтого: 89.90")});
    check(isTrue(r.ret) && http("GET", server + "/control/v1/devices", "", &status).find("Итого: 89.90") != std::string::npos,
          "строки на дисплее: " + lastError(c));
    r = c.call("ОтобразитьСтатусРабочегоМеста", {Value::str(id), Value::integer(3), Value::integer(1)});
    check(isTrue(r.ret), "статус рабочего места: " + lastError(c));
    http("PUT", server + "/control/v1/devices/" + dev + "/state", "{\"interactive\":true}", &status);
    check(waitListeners(server, dev, 1), "драйвер слушает события дисплея");
    http("POST", server + "/control/v1/devices/" + dev + "/actions/click", "{\"element\":\"ОплатаСБП\"}", &status);
    std::string data;
    check(c.platform().waitEvent(id, "OnClick", data) && data == "ОплатаСБП", "ExternalEvent(OnClick): " + data);
    c.call("ОтключитьОборудование", {Value::str(id)});

    // ТСД: выгрузка товаров, документ собран в панели, загрузка в 1С
    id = connectType("ТерминалСбораДанных", dev);
    const std::string goods = "<Table FullLoad=\"true\"><Record BarCodeBase64=\"NDYwMDAwMDAwMDAwOA==\" Name=\"Молоко\" Price=\"89.9\"/></Table>";
    r = c.call("ВыгрузитьТаблицу", {Value::str(id), Value::str(goods), Value::str("last")});
    check(isTrue(r.ret), "выгрузка в ТСД: " + lastError(c));
    http("POST", server + "/control/v1/devices/" + dev + "/actions/collect", "{\"barcode\":\"4600000000008\",\"quantity\":3}", &status);
    r = c.call("ЗагрузитьТаблицу", {Value::str(id), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[1], "BarCodeBase64=\"NDYwMDAwMDAwMDAwOA==\" Quantity=\"3\""), "загрузка из ТСД: " + show(r.params[1]));
    r = c.call("ОчиститьТаблицу", {Value::str(id)});
    check(isTrue(r.ret), "очистка ТСД");
    c.call("ОтключитьОборудование", {Value::str(id)});

    // RFID: сессия, метка в поле — событие RFID с пакетом, данные пакета, запись в USER
    id = connectType("СчитывательRFID", dev);
    check(waitListeners(server, dev, 1), "драйвер слушает события RFID");
    r = c.call("ОткрытьСессиюRFID", {Value::str(id)});
    check(isTrue(r.ret), "сессия RFID: " + lastError(c));
    http("POST", server + "/control/v1/devices/" + dev + "/actions/add_tag",
         "{\"tid\":\"E28011002000589616B408A1\",\"epc\":\"3074367C34001E880A6D6168\"}", &status);
    std::string pkg;
    check(c.platform().waitEvent(id, "RFID", pkg) && !pkg.empty(), "ExternalEvent(RFID): " + pkg);
    r = c.call("ПолучитьДанныеМетокRFID", {Value::str(id), Value::str(pkg), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[2], "TID=\"E28011002000589616B408A1\""), "данные меток: " + show(r.params[2]));
    r = c.call("ЗаписатьДанныеВМеткуRFID", {Value::str(id), Value::str("E28011002000589616B408A1"), Value::str("3074367C34001E880A6D6168"),
                                            Value::str("CAFE"), Value::integer(2), Value::integer(1000)});
    check(isTrue(r.ret) && r.params[1].s == "E28011002000589616B408A1", "запись в метку: " + lastError(c));
    c.call("ЗакрытьСессиюRFID", {Value::str(id)});
    c.call("ОтключитьОборудование", {Value::str(id)});

    // распознавание: кандидаты по умолчанию — 1003 (0.95) и 1023 (0.15)
    id = connectType("УстройствоРаспознавания", dev);
    r = c.call("Распознать", {Value::str(id), Value::boolean(true), Value::integer(1), Value::str("")});
    check(isTrue(r.ret) && contains(r.params[3], "Code=\"1003\"") && !contains(r.params[3], "1023"), "распознавание: " + show(r.params[3]));
    r = c.call("ПодтвердитьРаспознавание", {Value::str(id), Value::integer(1003)});
    check(isTrue(r.ret), "подтверждение распознавания: " + lastError(c));
    r = c.call("ПолучитьИзображение", {Value::str(id), Value::str("")});
    check(isTrue(r.ret) && r.params[1].s.rfind("iVBORw0KGgo", 0) == 0, "изображение PNG");
    c.call("ОтключитьОборудование", {Value::str(id)});
}

namespace fs = std::filesystem;

fs::path pathOf(const std::string& utf8) { return fs::u8path(utf8); }

void writeText(const std::string& path, const std::string& text) {
    FILE* f = nullptr;
#ifdef _WIN32
    _wfopen_s(&f, pathOf(path).c_str(), L"wb");
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

void removeTree(const std::string& dir) {
    std::error_code ec;
    fs::remove_all(pathOf(dir), ec);
}

// redirect — самообновление: в кэше лежит «версия 99.0.0» (копия этого же драйвера),
// экспорты исходного драйвера должны уйти в неё. expect=false — при EMULKON_DRIVER_NO_UPDATE=1 не уходят.
int redirect(const std::string& dll, const std::string& cacheArg, bool expect) {
    const fs::path cache = pathOf(cacheArg).make_preferred();  // CMake передаёт путь с «/»
    removeTree(cache.u8string());
    const fs::path dir = cache / "99.0.0";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path copy = dir / pathOf(dll).filename();
    check(fs::copy_file(pathOf(dll), copy, fs::copy_options::overwrite_existing, ec), "копия драйвера в кэш обновлений");
    emu::sys::setEnv("EMULKON_DRIVER_CACHE", cache.u8string().c_str());

    Component c;
    if (!c.load(dll) || !c.create(u"EmulkonDriver")) {
        std::printf("FAIL загрузка драйвера: %s\n", loadError().c_str());
        return 1;
    }
    bool redirected = isLoaded(copy.u8string());
    check(redirected == expect, expect ? "экспорты ушли в драйвер из кэша" : "переадресация выключена (EMULKON_DRIVER_NO_UPDATE)");
    check(c.classNames() == "EmulkonDriver", "GetClassNames через переадресацию");
    Result r = c.call("ПолучитьРевизиюИнтерфейса", {});
    check(r.ret.kind == Value::Int && r.ret.i == 4008, "объект из кэша работает: " + show(r.ret));
    c.destroy();
    std::printf("harness: %d passed, %d failed (самообновление)\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

int run(const std::vector<std::string>& args) {
    if (args.size() >= 5 && args[1] == "--redirect") return redirect(args[2], args[3], args[4] == "1");
    if (args.size() < 2) {
        std::printf("использование: harness <путь к драйверу> [http://сервер:порт]\n"
                    "               harness --redirect <путь к драйверу> <каталог кэша> <1|0>\n");
        return 2;
    }
    // Свой пустой кэш обновлений: стенд не должен уйти в драйвер, скачанный на этой машине,
    // и не трогает настоящий кэш пользователя.
    const std::string cache = emu::sys::tempDir() + emu::sys::kSep + "emulkon-harness-" + std::to_string(emu::sys::processId());
    removeTree(cache);
    std::error_code ec;
    fs::create_directories(pathOf(cache), ec);
    writeText(cache + emu::sys::kSep + "server.txt", "http://127.0.0.1:9");
    emu::sys::setEnv("EMULKON_DRIVER_CACHE", cache.c_str());

    Component c;
    if (!c.load(args[1])) {
        std::printf("FAIL не удалось загрузить драйвер или найти экспорты: %s\n", loadError().c_str());
        return 1;
    }
    check(c.attachType() == eCanAttachAny, "GetAttachType");
    if (!c.create(u"EmulkonDriver")) {
        std::printf("FAIL GetClassObject/Init/setMemManager\n");
        return 1;
    }
    offline(c);
    if (args.size() >= 3) {
        const std::string& server = args[2];
        online(c, server);
        acquiring(c, server);
        instances(c, server);
        workstationToken(c, server);
        legacyScheme(c, server);
        scales(c, server);
        printers(c, server);
        inputs(c, server);
        others(c, server);
    }
    c.destroy();
    removeTree(cache);
    std::printf("harness: %d passed, %d failed%s\n", g_passed, g_failed, args.size() >= 3 ? " (с сервером)" : " (без сервера)");
    return g_failed ? 1 : 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(emu::utf16to8(reinterpret_cast<const char16_t*>(argv[i])));
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
