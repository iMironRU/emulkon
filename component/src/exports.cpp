// Экспортируемые функции внешней компоненты (SDK Native API, ComponentBase.h).
//
// Самообновление (update.h): если в кэше пользователя есть драйвер новее этого, все экспорты
// переадресуются в него — объект создаёт и уничтожает новая версия. Выбор делается один раз за
// процесс при первом вызове экспорта. Windows — LoadLibrary, Linux — dlopen.
#include <new>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "driver.h"
#include "text.h"
#include "update.h"

namespace {

// Возможности платформы — пока не используются (уведомления, диалоги).
[[maybe_unused]] AppCapabilities g_capabilities = eAppCapabilitiesInvalid;

#ifdef _WIN32
using Module = HMODULE;

Module selfModule() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&selfModule), &h);
    return h;
}

Module openModule(const std::string& path) {
    std::u16string w = emu::utf8to16(path);
    return LoadLibraryExW(reinterpret_cast<LPCWSTR>(w.c_str()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void closeModule(Module m) { FreeLibrary(m); }

void* symbol(Module m, const char* name) { return reinterpret_cast<void*>(GetProcAddress(m, name)); }
#else
using Module = void*;

// selfModule — описатель этой библиотеки (RTLD_NOLOAD: не загружать, только найти); он же у
// dlopen() того же файла, поэтому «каталог кэша назван не своей версией» распознаётся сравнением.
Module selfModule() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&selfModule), &info) || !info.dli_fname) return nullptr;
    Module m = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (m) dlclose(m);  // счётчик ссылок не меняем
    return m;
}

Module openModule(const std::string& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }

void closeModule(Module m) { dlclose(m); }

void* symbol(Module m, const char* name) { return dlsym(m, name); }
#endif

struct Target {
    Module module = nullptr;  // nullptr — работает эта библиотека
    GetClassObjectPtr getClassObject = nullptr;
    DestroyObjectPtr destroyObject = nullptr;
    GetClassNamesPtr getClassNames = nullptr;
    SetPlatformCapabilitiesPtr setPlatformCapabilities = nullptr;
    GetAttachTypePtr getAttachType = nullptr;
};

template <typename T>
T proc(Module m, const char* name) {
    return reinterpret_cast<T>(symbol(m, name));
}

// resolve ищет в кэше самый новый драйвер, который загружается и экспортирует всё нужное.
// Битый или чужой файл пропускается — тогда работает следующий или этот.
Target resolve() {
    if (emu::redirectDisabled()) return {};
    for (const std::string& version : emu::newerVersions(emu::cachedVersions(), EMULKON_VERSION)) {
        Module m = openModule(emu::cachedDllPath(version));
        if (!m) continue;
        if (m == selfModule()) {  // каталог кэша назван не своей версией, а это мы и есть
            closeModule(m);
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
        closeModule(m);
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
