// Проверки «может ли Эмулкон работать»: до установки и для установленного (/check).
#include <winsock2.h>
#include <ws2tcpip.h>  // до iphlpapi.h: таблицы TCP для IPv6
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "doc.h"
#include "http.h"
#include "json.h"
#include "setup.h"

namespace setup {

namespace {

struct Checker {
    const Sink& sink;
    Tally tally;
    void add(Status s, const std::wstring& title, const std::wstring& detail) {
        tally.add(s);
        if (sink) sink(Item{s, title, detail});
    }
};

std::wstring processPath(DWORD pid) {
    if (pid == 4) return L"System (служба HTTP Windows)";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"процесс " + std::to_wstring(pid);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out = QueryFullProcessImageNameW(p, 0, buf, &n) ? std::wstring(buf, n) : L"процесс " + std::to_wstring(pid);
    CloseHandle(p);
    return out;
}

// portOwner — кто слушает порт (IPv4 или IPv6); 0 — свободен.
DWORD portOwner(int port) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<char> buf(size + 1024);
    size = static_cast<ULONG>(buf.size());
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            if (ntohs(static_cast<u_short>(t->table[i].dwLocalPort)) == port) return t->table[i].dwOwningPid;
        }
    }
    size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0);
    buf.assign(size + 1024, 0);
    size = static_cast<ULONG>(buf.size());
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            if (ntohs(static_cast<u_short>(t->table[i].dwLocalPort)) == port) return t->table[i].dwOwningPid;
        }
    }
    return 0;
}

bool processRunningIn(const std::wstring& dir, const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe) == 0 && isInside(processPath(pe.th32ProcessID), dir)) found = true;
    }
    CloseHandle(snap);
    return found;
}

struct Platform1C {
    std::wstring version;
    bool x64;
};

int compareDotted(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && a[i] != L'.') x = x * 10 + (a[i++] - L'0');
        while (j < b.size() && b[j] != L'.') y = y * 10 + (b[j++] - L'0');
        if (x != y) return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    return 0;
}

// findPlatforms — установленные 1С:Предприятие 8 (стандартные каталоги Program Files).
std::vector<Platform1C> findPlatforms() {
    std::vector<Platform1C> out;
    std::wstring pf64 = envVar(L"ProgramW6432");
    if (pf64.empty()) pf64 = envVar(L"ProgramFiles");
    const struct {
        std::wstring base;
        bool x64;
    } roots[] = {{pf64, true}, {envVar(L"ProgramFiles(x86)"), false}};
    for (const auto& r : roots) {
        if (r.base.empty()) continue;
        for (const wchar_t* sub : {L"\\1cv8", L"\\1cv8t"}) {
            std::wstring root = r.base + sub;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring name = fd.cFileName;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !isPlatformVersion(name)) continue;
                std::wstring bin = root + L"\\" + name + L"\\bin\\";
                if (fileExists(bin + L"1cv8.exe") || fileExists(bin + L"1cv8c.exe")) out.push_back({name, r.x64});
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::sort(out.begin(), out.end(), [](const Platform1C& a, const Platform1C& b) {
        int c = compareDotted(a.version, b.version);
        return c != 0 ? c > 0 : a.x64 > b.x64;
    });
    return out;
}

void checkPlatforms(Checker& c) {
    auto list = findPlatforms();
    if (list.empty()) {
        c.add(Status::Warn, L"1С:Предприятие",
              L"не найдено в Program Files. Сервер и трей работают и без неё; драйвер ставят в БПО на компьютерах с 1С");
        return;
    }
    std::wstring s;
    bool x64 = false, x86 = false;
    for (size_t i = 0; i < list.size() && i < 6; ++i) {
        s += (i ? L", " : L"") + list[i].version + (list[i].x64 ? L" (64-бит)" : L" (32-бит)");
        (list[i].x64 ? x64 : x86) = true;
    }
    if (list.size() > 6) s += L" и ещё " + std::to_wstring(list.size() - 6);
    s += L". В пакете драйвера есть DLL для ";
    s += x64 && x86 ? L"обеих разрядностей" : (x64 ? L"64-бит" : L"32-бит");
    c.add(Status::Ok, L"1С:Предприятие", s);
}

void checkWindows(Checker& c) {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (fn) fn(&v);
    std::wstring name = v.dwMajorVersion >= 10 ? (v.dwBuildNumber >= 22000 ? L"Windows 11" : L"Windows 10")
                                               : L"Windows " + std::to_wstring(v.dwMajorVersion) + L"." +
                                                     std::to_wstring(v.dwMinorVersion);
    USHORT process = 0, native = 0;
    std::wstring arch = L"x64";
    if (IsWow64Process2(GetCurrentProcess(), &process, &native) && native == IMAGE_FILE_MACHINE_ARM64) {
        arch = L"ARM64, программы x64 работают в эмуляции";
    }
    std::wstring detail = name + L" (сборка " + std::to_wstring(v.dwBuildNumber) + L"), " + arch;
    if (v.dwMajorVersion < 10) {
        c.add(Status::Warn, L"Windows", detail + L". Проверено на Windows 10 и 11");
    } else {
        c.add(Status::Ok, L"Windows", detail);
    }
}

void checkPort(Checker& c, const std::wstring& dir, const std::wstring& legacyDir) {
    DWORD pid = portOwner(kPort);
    std::wstring port = std::to_wstring(kPort);
    if (!pid) {
        c.add(Status::Ok, L"Порт " + port, L"свободен — служба Эмулкона сможет его занять");
        return;
    }
    std::wstring path = processPath(pid);
    if (!dir.empty() && isInside(path, dir)) {
        c.add(Status::Ok, L"Порт " + port, L"занят службой Эмулкона — она будет перезапущена");
    } else if (!legacyDir.empty() && isInside(path, legacyDir)) {
        c.add(Status::Ok, L"Порт " + port, L"занят сервером прежней установки — он будет остановлен, порт займёт служба");
    } else if (_wcsicmp(fileName(path).c_str(), L"emulkon.exe") == 0) {
        c.add(Status::Warn, L"Порт " + port,
              L"занят другим сервером Эмулкона: " + path + L". Служба не запустится, пока он работает — остановите его");
    } else {
        c.add(Status::Warn, L"Порт " + port,
              L"занят: " + path + L" (PID " + std::to_wstring(pid) +
                  L"). Служба Эмулкона не запустится — освободите порт");
    }
}

// get — GET JSON с сервера.
bool getJson(emu::HttpClient& http, const std::wstring& url, emu::Json& out, std::wstring& error, int timeoutMs) {
    emu::HttpResponse r = http.get(narrow(url), {}, timeoutMs);
    if (!r.error.empty()) {
        error = widen(r.error);
        return false;
    }
    if (r.status != 200) {
        error = L"HTTP " + std::to_wstring(r.status);
        return false;
    }
    std::string err;
    if (!emu::parseJson(r.body, out, err)) {
        error = L"ответ не JSON: " + widen(err);
        return false;
    }
    return true;
}

std::wstring jsonText(const emu::Json& j, const char* key) {
    const emu::Json* v = j.get(key);
    return v && v->isString() ? widen(v->text) : std::wstring();
}

}  // namespace

Tally preChecks(const Options& o, const Sink& sink) {
    Checker c{sink, {}};
    checkWindows(c);

    Installed inst = findInstalled();
    std::wstring dir = !o.dir.empty() ? o.dir : (inst.present ? inst.dir : defaultInstallDir());
    // Каталог: можно ли писать (создаём и удаляем пробный файл).
    std::wstring probeDir = dir;
    while (!probeDir.empty() && !dirExists(probeDir)) probeDir = parentDir(probeDir);
    std::wstring probe = probeDir + L"\\emulkon-setup-probe.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        c.add(Status::Fail, L"Каталог установки", L"нет прав на запись: " + dir + L" — выберите другой каталог");
    } else {
        CloseHandle(h);
        ULARGE_INTEGER freeBytes{};
        GetDiskFreeSpaceExW(probeDir.c_str(), &freeBytes, nullptr, nullptr);
        unsigned long long mb = freeBytes.QuadPart >> 20;
        if (mb < 100) {
            c.add(Status::Fail, L"Каталог установки",
                  dir + L": свободно " + std::to_wstring(mb) + L" МБ, нужно около 100 МБ с запасом под журнал");
        } else {
            c.add(Status::Ok, L"Каталог установки",
                  dir + L" — запись разрешена, свободно " + std::to_wstring(mb / 1024) + L" ГБ" +
                      (inst.present ? L"; установлена версия " + inst.version + L" — будет обновлена" : std::wstring()));
        }
    }
    Installed legacy = findLegacyInstalled();
    checkPort(c, dir, legacy.dir);
    if (legacy.present) {
        c.add(Status::Info, L"Прежняя установка",
              L"Эмулкон " + legacy.version + L" в профиле пользователя (" + legacy.dir +
                  L"): данные будут перенесены в " + machineDataDir() + L", сама установка — удалена");
    }
    checkPlatforms(c);
    if (o.network) {
        c.add(Status::Info, L"Кассы в сети",
              L"служба будет слушать 0.0.0.0:" + std::to_wstring(kPort) +
                  L", установщик разрешит порт в брандмауэре. Задайте токены — хешами в HKLM\\Software\\Emulkon\\Server");
    }
    return c.tally;
}

Tally workChecks(const std::wstring& dir, const Sink& sink) {
    Checker c{sink, {}};
    if (dir.empty() || !dirExists(dir)) {
        c.add(Status::Fail, L"Установка", L"Эмулкон не установлен: нет записи в реестре или каталога " + dir);
        return c.tally;
    }
    // 1. Файлы
    std::vector<std::wstring> missing;
    for (const std::wstring& f : {std::wstring(L"bin\\emulkon.exe"), std::wstring(L"bin\\EmulkonTray.exe"),
                                  std::wstring(L"console\\EmulkonSnapIn.dll"), std::wstring(L"console\\Emulkon.msc"),
                                  L"1C\\driver\\" + driverPackageName(), L"1C\\driver\\" + driverDllName(true),
                                  L"1C\\driver\\" + driverDllName(false)}) {
        if (!fileExists(dir + L"\\" + f)) missing.push_back(f);
    }
    if (missing.empty()) {
        c.add(Status::Ok, L"Файлы программы", dir + L" — версия " + version());
    } else {
        std::wstring s;
        for (const auto& m : missing) s += (s.empty() ? L"" : L", ") + m;
        c.add(Status::Fail, L"Файлы программы", L"нет файлов: " + s + L" — восстановите Эмулкон");
    }

    // 2. Служба
    ServiceState svc = serviceState();
    if (svc == ServiceState::Running) {
        c.add(Status::Ok, L"Служба Emulkon", L"работает");
    } else {
        c.add(Status::Fail, L"Служба Emulkon",
              serviceStateText(svc) + L" — запустите её (трей → «Перезапустить сервер» или «Службы» Windows); причина "
                                      L"остановки — в журнале событий Windows (источник Emulkon) и в " +
                  machineDataDir() + L"\\logs\\service.log");
    }

    // 3. Трей и куда он смотрит
    tray::Config cfg;
    tray::loadConfig(tray::defaultConfigPath(), cfg);
    std::wstring server = cfg.serverUrl;
    while (!server.empty() && server.back() == L'/') server.pop_back();
    // Трей после установки запускает проводник — даём ему до 10 с.
    bool trayUp = processRunningIn(dir, L"EmulkonTray.exe");
    for (int i = 0; i < 20 && !trayUp; ++i) {
        Sleep(500);
        trayUp = processRunningIn(dir, L"EmulkonTray.exe");
    }
    if (trayUp) {
        c.add(Status::Ok, L"Трей", L"запущен; сервер — " + server);
    } else {
        c.add(Status::Warn, L"Трей", L"не запущен — запустите «Эмулкон» из меню «Пуск»");
    }

    // 4. Сервер: ждём до 15 с — служба могла только что запуститься.
    std::unique_ptr<emu::HttpClient> http = emu::makeHttpClient("Emulkon-Setup/" EMULKON_VERSION);
    emu::Json health;
    std::wstring error;
    bool up = false;
    for (int attempt = 0; attempt < 15 && !up; ++attempt) {
        up = getJson(*http, server + L"/api/v1/health", health, error, 2000);
        if (!up) Sleep(1000);
    }
    if (!up) {
        std::wstring detail = L"не отвечает: " + server + L" (" + error + L")";
        if (isLocalUrl(server)) {
            DWORD pid = portOwner(kPort);
            detail += pid ? L". Порт " + std::to_wstring(kPort) + L" занят: " + processPath(pid)
                          : L". Порт свободен — служба Emulkon не слушает его";
        }
        c.add(Status::Fail, L"Сервер эмуляции", detail);
    } else {
        std::wstring v = jsonText(health, "version");
        if (v == version()) {
            c.add(Status::Ok, L"Сервер эмуляции", L"отвечает: " + server + L", версия " + v);
        } else {
            c.add(Status::Warn, L"Сервер эмуляции",
                  L"отвечает сервер версии " + v + L", а установлена " + version() +
                      L" — на порту работает другой экземпляр (например, из каталога разработки)");
        }
    }

    // 5. Страница загрузок
    if (up) {
        emu::Json cat;
        if (!getJson(*http, server + L"/api/v1/downloads", cat, error, 5000)) {
            c.add(Status::Warn, L"Страница загрузок", L"нет ответа: " + error + L" (сервер старше 0.2.0?)");
        } else {
            const emu::Json* pkg = cat.get("driver_package");
            if (pkg && pkg->isObject()) {
                c.add(Status::Ok, L"Страница загрузок",
                      server + L"/downloads — драйвер " + jsonText(*pkg, "version") + L" для скачивания на кассах");
            } else {
                c.add(Status::Warn, L"Страница загрузок",
                      L"драйвер не опубликован: у работающего сервера нет каталога 1C\\driver рядом с bin");
            }
        }
    }

    // 6. Драйвер: настоящая загрузка DLL и «Тест устройства» на сервере.
    std::wstring dll64 = driverDir(dir) + L"\\" + driverDllName(true);
    if (!fileExists(dll64)) {
        c.add(Status::Fail, L"Драйвер для 1С (64-бит)", L"нет файла " + dll64);
    } else if (!up) {
        c.add(Status::Warn, L"Драйвер для 1С (64-бит)", L"не проверен: сервер не отвечает");
    } else {
        Item it = probeDriver(dll64, server);
        c.add(it.status, it.title, it.detail);
    }
    c.add(fileExists(driverDir(dir) + L"\\" + driverDllName(false)) ? Status::Ok : Status::Fail,
          L"Драйвер для 1С (32-бит)", L"в пакете; проверяется в 32-битной 1С «Тестом устройства»");

    // 7. 1С и автозапуск — сведения.
    checkPlatforms(c);
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    bool autorun = RegGetValueW(HKEY_LOCAL_MACHINE, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
    c.add(Status::Info, L"Автозапуск",
          autorun ? L"трей запускается при входе в Windows у каждого пользователя"
                  : L"выключен — включается в мастере «Изменить» (Параметры → Приложения → Эмулкон)");
    return c.tally;
}

}  // namespace setup
