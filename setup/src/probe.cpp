// Сквозная проверка драйвера: загрузить DLL, как это делает платформа 1С (SDK Native API),
// и выполнить «Тест устройства» (EquipmentTest) на сервере. Если это прошло, драйвер
// на этом компьютере грузится, WinHTTP работает и сервер отвечает по Device API.
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "AddInDefBase.h"
#include "ComponentBase.h"
#include "IMemoryManager.h"
#include "doc.h"
#include "setup.h"
#include "text.h"

namespace setup {

namespace {

class Memory : public IMemoryManager {
public:
    bool ADDIN_API AllocMemory(void** p, unsigned long size) override {
        *p = std::malloc(size);
        return *p != nullptr;
    }
    void ADDIN_API FreeMemory(void** p) override {
        std::free(*p);
        *p = nullptr;
    }
};

// Platform — то, что драйвер видит как платформу 1С. Сообщения (AddError) собираем.
class Platform : public IAddInDefBaseEx {
public:
    std::string messages;
    bool ADDIN_API AddError(unsigned short, const WCHAR_T*, const WCHAR_T* descr, long) override {
        if (descr) messages += emu::utf16to8(reinterpret_cast<const char16_t*>(descr)) + "\n";
        return true;
    }
    bool ADDIN_API Read(WCHAR_T*, tVariant*, long*, WCHAR_T**) override { return false; }
    bool ADDIN_API Write(WCHAR_T*, tVariant*) override { return false; }
    bool ADDIN_API RegisterProfileAs(WCHAR_T*) override { return true; }
    bool ADDIN_API SetEventBufferDepth(long) override { return true; }
    long ADDIN_API GetEventBufferDepth() override { return 1; }
    bool ADDIN_API ExternalEvent(WCHAR_T*, WCHAR_T*, WCHAR_T*) override { return true; }
    void ADDIN_API CleanEventBuffer() override {}
    bool ADDIN_API SetStatusLine(WCHAR_T*) override { return true; }
    void ADDIN_API ResetStatusLine() override {}
    IInterface* ADDIN_API GetInterface(Interfaces) override { return nullptr; }
};

struct Call {
    bool ok = false;           // CallAsFunc вернул true
    tVariant ret{};
    std::vector<std::string> out;  // строковые параметры после вызова
};

std::string str(const tVariant& v) {
    if (TV_VT(&v) == VTYPE_PWSTR && v.pwstrVal) {
        return emu::utf16to8(std::u16string(reinterpret_cast<const char16_t*>(v.pwstrVal), v.wstrLen));
    }
    return {};
}

// call вызывает метод с параметрами-строками (как передаёт их БПО).
Call call(IComponentBase* obj, Memory& mem, const char16_t* name, const std::vector<std::string>& args) {
    Call c;
    long m = obj->FindMethod(reinterpret_cast<const WCHAR_T*>(name));
    if (m < 0) return c;
    long n = obj->GetNParams(m);
    std::vector<tVariant> params(static_cast<size_t>(n));
    for (long k = 0; k < n; ++k) {
        tVarInit(&params[k]);
        if (k >= static_cast<long>(args.size())) continue;
        std::u16string s = emu::utf8to16(args[k]);
        void* p = nullptr;
        mem.AllocMemory(&p, static_cast<unsigned long>((s.size() + 1) * sizeof(char16_t)));
        std::memcpy(p, s.c_str(), (s.size() + 1) * sizeof(char16_t));
        TV_VT(&params[k]) = VTYPE_PWSTR;
        params[k].pwstrVal = static_cast<WCHAR_T*>(p);
        params[k].wstrLen = static_cast<uint32_t>(s.size());
    }
    tVarInit(&c.ret);
    c.ok = obj->CallAsFunc(m, &c.ret, n ? &params[0] : nullptr, n);
    for (auto& p : params) {
        c.out.push_back(str(p));
        if (TV_VT(&p) == VTYPE_PWSTR && p.pwstrVal) mem.FreeMemory(reinterpret_cast<void**>(&p.pwstrVal));
    }
    return c;
}

std::string attr(const std::string& xml, const std::string& name) {
    std::string key = " " + name + "=\"";
    size_t p = xml.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    size_t e = xml.find('"', p);
    return e == std::string::npos ? std::string() : xml.substr(p, e - p);
}

std::wstring firstLine(const std::string& s) {
    std::string line = s.substr(0, s.find('\n'));
    if (line.size() > 220) line = line.substr(0, 220) + "…";
    return widen(line);
}

}  // namespace

Item probeDriver(const std::wstring& dll, const std::wstring& serverUrl) {
    Item it;
    it.title = L"Драйвер для 1С (64-бит)";
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        it.status = Status::Fail;
        it.detail = L"DLL не загружается (код " + std::to_wstring(GetLastError()) + L"): " + dll;
        return it;
    }
    auto getClassObject = reinterpret_cast<GetClassObjectPtr>(reinterpret_cast<void*>(GetProcAddress(m, "GetClassObject")));
    auto destroyObject = reinterpret_cast<DestroyObjectPtr>(reinterpret_cast<void*>(GetProcAddress(m, "DestroyObject")));
    auto setCaps = reinterpret_cast<SetPlatformCapabilitiesPtr>(
        reinterpret_cast<void*>(GetProcAddress(m, "SetPlatformCapabilities")));
    IComponentBase* obj = nullptr;
    if (!getClassObject || !destroyObject || !setCaps) {
        it.status = Status::Fail;
        it.detail = L"в DLL нет экспортов Native API: " + dll;
        FreeLibrary(m);
        return it;
    }
    setCaps(eAppCapabilities3);
    Memory mem;
    Platform platform;
    if (!getClassObject(reinterpret_cast<const WCHAR_T*>(u"EmulkonDriver"), &obj) || !obj ||
        !obj->Init(static_cast<IAddInDefBase*>(&platform)) || !obj->setMemManager(&mem)) {
        it.status = Status::Fail;
        it.detail = L"драйвер не создал объект EmulkonDriver";
        if (obj) destroyObject(&obj);
        FreeLibrary(m);
        return it;
    }

    // Версия, которая реально работает: с учётом самообновления из кэша.
    Call d = call(obj, mem, u"GetDescription", {""});
    std::string ver = d.out.empty() ? std::string() : attr(d.out[0], "DriverVersion");

    std::string cp = "<Parameters><Parameter Name=\"ServerURL\" Value=\"" + emu::xmlAttr(narrow(serverUrl)) +
                     "\"/><Parameter Name=\"TimeoutMs\" Value=\"10000\"/></Parameters>";
    Call t = call(obj, mem, u"EquipmentTest", {"KKT", cp, "", ""});
    bool passed = t.ok && TV_VT(&t.ret) == VTYPE_BOOL && TV_BOOL(&t.ret);
    std::string description = t.out.size() > 2 ? t.out[2] : std::string();
    if (passed) {
        it.status = Status::Ok;
        it.detail = L"версия " + widen(ver) + L" загружается и проходит «Тест устройства» на " + serverUrl + L": " +
                    firstLine(description);
    } else {
        Call e = call(obj, mem, u"GetLastError", {""});
        std::string why = !e.out.empty() && !e.out[0].empty() ? e.out[0] : description;
        it.status = Status::Fail;
        it.detail = L"«Тест устройства» не прошёл: " + firstLine(why);
    }
    obj->Done();
    destroyObject(&obj);
    FreeLibrary(m);
    return it;
}

}  // namespace setup
