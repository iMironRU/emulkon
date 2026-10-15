// EmulkonDriver — драйвер подключаемого оборудования для БПО (Native API, ARCHITECTURE §4.1).
// Тонкий прокси: методы до подключения обслуживает сам, остальные передаёт на сервер эмуляции.
#pragma once

#include <map>
#include <memory>
#include <string>

#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

#include "device_client.h"
#include "events.h"
#include "http.h"
#include "json.h"

#ifndef EMULKON_VERSION
#define EMULKON_VERSION "0.0.0-dev"
#endif

namespace emu {

// Ревизия требований к драйверам, которую реализует компонента: 4008 = «4.8».
constexpr int kInterfaceRevision = 4008;

// Имя класса = вторая часть ProgID из INFO.XML (AddIn.EmulkonDriver) — так его ищет БПО.
constexpr const char16_t* kClassName = u"EmulkonDriver";

// Разработчик — в названии и описании драйвера (отдельного поля в требованиях нет).
constexpr const char* kDeveloper = "imiron.ru";

// Дополнительное действие БПО (GetAdditionalActions): проверить и скачать новую версию драйвера.
constexpr const char* kActionCheckUpdate = "CheckUpdate";

// Коды ошибок, которые выставляет сама компонента (ошибки устройства приходят с сервера).
constexpr int kErrNoConnection = 900;   // нет связи с сервером, сессия не открылась
constexpr int kErrProtocol = 901;       // сервер ответил не по протоколу
constexpr int kErrNotConnected = 902;   // DeviceID не подключён
constexpr int kErrNotSupported = 903;   // метод не поддерживается
constexpr int kErrBadParams = 904;      // неверные параметры вызова

class EmulkonDriver : public IComponentBase {
public:
    EmulkonDriver();
    ~EmulkonDriver() override;

    // IInitDoneBase
    bool ADDIN_API Init(void* disp) override;
    bool ADDIN_API setMemManager(void* mem) override;
    long ADDIN_API GetInfo() override;
    void ADDIN_API Done() override;

    // ILanguageExtenderBase
    bool ADDIN_API RegisterExtensionAs(WCHAR_T** wsExtensionName) override;
    long ADDIN_API GetNProps() override;
    long ADDIN_API FindProp(const WCHAR_T* wsPropName) override;
    const WCHAR_T* ADDIN_API GetPropName(long lPropNum, long lPropAlias) override;
    bool ADDIN_API GetPropVal(const long lPropNum, tVariant* pvarPropVal) override;
    bool ADDIN_API SetPropVal(const long lPropNum, tVariant* varPropVal) override;
    bool ADDIN_API IsPropReadable(const long lPropNum) override;
    bool ADDIN_API IsPropWritable(const long lPropNum) override;
    long ADDIN_API GetNMethods() override;
    long ADDIN_API FindMethod(const WCHAR_T* wsMethodName) override;
    const WCHAR_T* ADDIN_API GetMethodName(const long lMethodNum, const long lMethodAlias) override;
    long ADDIN_API GetNParams(const long lMethodNum) override;
    bool ADDIN_API GetParamDefValue(const long lMethodNum, const long lParamNum, tVariant* pvarParamDefValue) override;
    bool ADDIN_API HasRetVal(const long lMethodNum) override;
    bool ADDIN_API CallAsProc(const long lMethodNum, tVariant* paParams, const long lSizeArray) override;
    bool ADDIN_API CallAsFunc(const long lMethodNum, tVariant* pvarRetValue, tVariant* paParams, const long lSizeArray) override;

    // LocaleBase, UserLanguageBase
    void ADDIN_API SetLocale(const WCHAR_T* loc) override;
    void ADDIN_API SetUserInterfaceLanguageCode(const WCHAR_T* lang) override;

    // Тексты для тестов и описания драйвера.
    static std::string driverDescriptionXml();
    static std::string settingsXml();
    static std::string additionalActionsXml();

private:
    struct Device {
        std::string equipmentType;
        std::unique_ptr<DeviceClient> client;
        std::unique_ptr<EventListener> events;  // после client: останавливается раньше
    };

    bool call(long method, tVariant* ret, tVariant* params, long count);
    bool callLocal(int local, tVariant* ret, tVariant* params, long count);
    bool connect(tVariant* params, long count);
    bool disconnect(tVariant* params, long count);
    bool test(tVariant* params, long count);
    // Старая схема (до 4.3): параметры копятся в legacy_, Подключить и ТестУстройства вызывают connect и test.
    bool legacyOpen(tVariant* params, long count);
    bool legacyTest(tVariant* params, long count);
    std::string legacyParametersXml() const;
    void freeStrings(tVariant* v, int n);
    bool forward(const std::string& method, bool waitsForPerson, tVariant* ret, tVariant* params, long count);
    bool doAction(const std::string& name);
    void notify(const std::string& text);  // сообщение пользователю в окне сообщений 1С
    // raiseEvent — внешнее событие в 1С: Who — DeviceID для 1С (из фонового потока событий).
    void raiseEvent(const std::string& who, const std::string& name, const std::string& data);

    // Вызов на сервере и разбор ответа в параметры 1С; client — уже с открытой сессией.
    bool remote(DeviceClient& client, const std::string& method, const Json& sent, tVariant* ret, tVariant* params,
                long count, int timeoutMs = 0);

    Endpoint endpointFrom(const std::string& connectionParameters, std::string& sanitized) const;

    void setError(int code, const std::string& text);
    void clearError();

    WCHAR_T* allocString(const std::u16string& s);
    void setString(tVariant* v, const std::string& utf8);
    void replaceString(tVariant* v, const std::string& utf8);  // освобождает прежнюю строку
    Json toJson(const tVariant& v) const;
    void fromJson(tVariant* v, const Json& j);
    std::string stringParam(tVariant* params, long count, long i) const;

    IAddInDefBase* connection_ = nullptr;
    IMemoryManager* memory_ = nullptr;
    std::unique_ptr<HttpClient> http_;
    std::map<std::string, Device> devices_;  // DeviceID для 1С → подключение
    std::map<std::string, std::string> legacy_;  // УстановитьПараметр: имя → значение (старая схема)
    int nextDevice_ = 1;
    int lastErrorCode_ = 0;
    std::string lastErrorText_;
    std::string applicationName_;
    std::string applicationVersion_;
};

}  // namespace emu
