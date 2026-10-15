// Экспортируемые функции внешней компоненты (SDK Native API, ComponentBase.h).
//
// Самообновление (update.h): если в кэше пользователя есть DLL драйвера новее этой,
// все экспорты переадресуются в неё — объект создаёт и уничтожает новая версия.
// Выбор делается один раз за процесс при первом вызове экспорта.
#include <new>

#include <windows.h>

#include "driver.h"
#include "update.h"

namespace {

// Возможности платформы — пока не используются (уведомления, диалоги).
[[maybe_unused]] AppCapabilities g_capabilities = eAppCapabilitiesInvalid;

struct Target {
    HMODULE module = nullptr;  // nullptr — работает эта DLL
    GetClassObjectPtr getClassObject = nullptr;
    DestroyObjectPtr destroyObject = nullptr;
    GetClassNamesPtr getClassNames = nullptr;
    SetPlatformCapabilitiesPtr setPlatformCapabilities = nullptr;
    GetAttachTypePtr getAttachType = nullptr;
};

HMODULE selfModule() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&selfModule), &h);
    return h;
}

template <typename T>
T proc(HMODULE m, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

// resolve ищет в кэше самую новую DLL, которая загружается и экспортирует всё нужное.
// Битая или чужая DLL пропускается — тогда работает следующая или эта.
Target resolve() {
    if (emu::redirectDisabled()) return {};
    for (const std::string& version : emu::newerVersions(emu::cachedVersions(), EMULKON_VERSION)) {
        std::wstring path = emu::cachedDllPath(version);
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) continue;
        if (m == selfModule()) {  // каталог кэша назван не своей версией, а это мы и есть
            FreeLibrary(m);
            return {};
        }
        Target t;
        t.module = m;
        t.getClassObject = proc<GetClassObjectPtr>(m, "GetClassObject");
        t.destroyObject = proc<DestroyObjectPtr>(m, "DestroyObject");
        t.getClassNames = proc<GetClassNamesPtr>(m, "GetClassNames");
        t.setPlatformCapabilities = proc<SetPlatformCapabilitiesPtr>(m, "SetPlatformCapabilities");
        t.getAttachType = proc<GetAttachTypePtr>(m, "GetAttachType");
        if (t.getClassObject && t.destroyObject && t.getClassNames && t.setPlatformCapabilities && t.getAttachType) {
            return t;  // ссылку на модуль держим до конца процесса
        }
        FreeLibrary(m);
    }
    return {};
}

const Target& target() {
    static const Target t = resolve();
    return t;
}

}  // namespace

// Список классов через «|». Один объект на компоненту (требования, раздел 1).
const WCHAR_T* GetClassNames() {
    const Target& t = target();
    return t.module ? t.getClassNames() : emu::kClassName;
}

long GetClassObject(const WCHAR_T* wsName, IComponentBase** pIntf) {
    const Target& t = target();
    if (t.module) return t.getClassObject(wsName, pIntf);
    if (!pIntf || *pIntf) return 0;
    *pIntf = new (std::nothrow) emu::EmulkonDriver();
    return *pIntf ? 1 : 0;
}

long DestroyObject(IComponentBase** pIntf) {
    const Target& t = target();
    if (t.module) return t.destroyObject(pIntf);
    if (!pIntf || !*pIntf) return -1;
    delete *pIntf;
    *pIntf = nullptr;
    return 0;
}

AppCapabilities SetPlatformCapabilities(const AppCapabilities capabilities) {
    const Target& t = target();
    if (t.module) return t.setPlatformCapabilities(capabilities);
    g_capabilities = capabilities;
    return eAppCapabilitiesLast;
}

// Драйвер можно подключать и в изолированном процессе, и в процессе платформы.
AttachType GetAttachType() {
    const Target& t = target();
    return t.module ? t.getAttachType() : eCanAttachAny;
}
