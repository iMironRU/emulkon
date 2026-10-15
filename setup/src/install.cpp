// Установка и удаление на машину (docs/local-install.md): файлы в Program Files, служба, данные в ProgramData,
// «Установленные приложения», ярлыки, автозапуск трея, перенос прежней установки из профиля пользователя.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <ctime>
#include <string>
#include <vector>

#include "config.h"  // tray::Config — настройки трея
#include "doc.h"
#include "setup.h"

namespace setup {

namespace {

void report(const Sink& sink, Status s, const std::wstring& title, const std::wstring& detail = {}) {
    if (sink) sink(Item{s, title, detail});
}

std::wstring winError(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s + L" (код " + std::to_wstring(code) + L")";
}

struct ComInit {
    bool ok;
    ComInit() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ~ComInit() {
        if (ok) CoUninitialize();
    }
};

bool ensureDir(const std::wstring& dir) {
    if (dirExists(dir)) return true;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dirExists(dir);
}

std::wstring stamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t buf[32];
    wcsftime(buf, 32, L"%Y%m%d-%H%M%S", &tm);
    return buf;
}

// writeFileAtomic пишет во временный файл и подменяет им прежний. Пока прежний занят (процесс ещё
// завершается) — повторяет до 10 секунд. Если exe так и держит чужой процесс (трей другого пользователя):
// запущенный файл можно переименовать — он уходит в «.old-…» и удаляется при перезагрузке.
bool writeFileAtomic(const std::wstring& path, const void* data, DWORD size, std::wstring& error) {
    std::wstring tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = winError(GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, nullptr) && written == size;
    DWORD code = GetLastError();
    CloseHandle(h);
    if (!ok) {
        error = winError(code);
        DeleteFileW(tmp.c_str());
        return false;
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        code = GetLastError();
        error = winError(code);
        if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION) break;
        Sleep(500);
    }
    std::wstring old = path + L".old-" + stamp();
    if (MoveFileExW(path.c_str(), old.c_str(), 0) && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return true;
    }
    DeleteFileW(tmp.c_str());
    return false;
}

bool readFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (512LL << 20);
    if (ok) {
        out.assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        ok = out.empty() || (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool extract(const wchar_t* resource, const std::wstring& path, std::wstring& error) {
    HRSRC r = FindResourceW(nullptr, resource, MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!r) {
        error = L"в установщике нет файла " + std::wstring(resource);
        return false;
    }
    HGLOBAL g = LoadResource(nullptr, r);
    const void* data = g ? LockResource(g) : nullptr;
    DWORD size = SizeofResource(nullptr, r);
    if (!data || !size) {
        error = L"файл " + std::wstring(resource) + L" в установщике пуст";
        return false;
    }
    return writeFileAtomic(path, data, size, error);
}

bool copyFileAtomic(const std::wstring& from, const std::wstring& to, std::wstring& error) {
    std::string data;
    if (!readFile(from, data)) {
        error = L"не прочитать " + from + L": " + winError(GetLastError());
        return false;
    }
    return writeFileAtomic(to, data.data(), static_cast<DWORD>(data.size()), error);
}

// copyTree копирует каталог целиком; возвращает, сколько файлов скопировать не удалось.
int copyTree(const std::wstring& from, const std::wstring& to) {
    if (!ensureDir(to)) return 1;
    int failed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            failed += copyTree(from + L"\\" + name, to + L"\\" + name);
        } else {
            std::wstring error;
            if (!copyFileAtomic(from + L"\\" + name, to + L"\\" + name, error)) ++failed;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return failed;
}

// --- реестр ---

bool regSet(HKEY key, const wchar_t* name, const std::wstring& v) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                          static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool regSetDword(HKEY key, const wchar_t* name, DWORD v) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v) == ERROR_SUCCESS;
}

std::wstring regGet(HKEY root, const wchar_t* path, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

std::wstring unquote(std::wstring s) {
    if (!s.empty() && s[0] == L'"') {
        size_t e = s.find(L'"', 1);
        s = s.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
    }
    return s;
}

// --- файлы ---

unsigned long long treeSize(const std::wstring& dir) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += treeSize(dir + L"\\" + name);
        } else {
            total += (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

// removeTree удаляет каталог со всем содержимым, кроме файла keep (каталоги на пути к нему
// остаются). Возвращает, сколько файлов и каталогов удалить не удалось.
int removeTree(const std::wstring& dir, const std::wstring& keep = {}) {
    if (!dirExists(dir)) return 0;
    int left = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                left += removeTree(path, keep);
            } else if (!keep.empty() && samePath(path, keep)) {
                continue;  // удалится после выхода (scheduleSelfDelete)
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(path.c_str())) ++left;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    bool holdsKeep = !keep.empty() && isInside(keep, dir);
    if (left == 0 && !holdsKeep && !RemoveDirectoryW(dir.c_str())) ++left;
    return left;
}

// removeStale убирает из каталога файлы прежних версий: сервер и так раздаёт самые новые.
void removeStale(const std::wstring& dir) {
    const std::wstring keep[] = {driverPackageName(), driverDllName(true), driverDllName(false), setupDownloadName()};
    for (const wchar_t* mask : {L"EmulkonDriver_*", L"EmulkonSetup_*", L"*.old-*"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + mask).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring name = fd.cFileName;
            bool current = false;
            for (const auto& k : keep) current = current || samePath(name, k);
            if (!current) DeleteFileW((dir + L"\\" + name).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// --- ярлыки ---

bool createShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args,
                    const std::wstring& description, const std::wstring& icon) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetArguments(args.c_str());
    link->SetDescription(description.c_str());
    link->SetWorkingDirectory(dirExists(target) ? target.c_str() : parentDir(target).c_str());
    link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(lnk.c_str(), TRUE));
    if (file) file->Release();
    link->Release();
    return ok;
}

// writeUrlShortcut — ярлык на веб-страницу (.url — INI, в UTF-16 с BOM: путь может быть не ASCII).
bool writeUrlShortcut(const std::wstring& path, const std::wstring& url, const std::wstring& icon) {
    std::wstring text = L"\xFEFF[InternetShortcut]\r\nURL=" + url + L"\r\nIconFile=" + icon + L"\r\nIconIndex=0\r\n";
    std::wstring error;
    return writeFileAtomic(path, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), error);
}

// --- процессы ---

struct Proc {
    DWORD pid;
    std::wstring path;
};

std::wstring processPath(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out = QueryFullProcessImageNameW(p, 0, buf, &n) ? std::wstring(buf, n) : std::wstring();
    CloseHandle(p);
    return out;
}

std::vector<Proc> processesIn(const std::wstring& dir) {
    std::vector<Proc> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    DWORD self = GetCurrentProcessId();
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
        std::wstring path = processPath(pe.th32ProcessID);
        if (isInside(path, dir)) out.push_back({pe.th32ProcessID, path});
    }
    CloseHandle(snap);
    return out;
}

bool waitExit(DWORD pid, DWORD ms) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return true;
    bool done = WaitForSingleObject(p, ms) == WAIT_OBJECT_0;
    CloseHandle(p);
    return done;
}

// enableDebugPrivilege — администратору нужна SeDebugPrivilege, чтобы завершить трей другого пользователя
// (его процесс не даёт доступа группе «Администраторы»).
void enableDebugPrivilege() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    }
    CloseHandle(token);
}

struct OwnWindows {
    std::wstring dir;
    std::vector<DWORD> pids;
};

// closeOwnWindows закрывает штатно трей (он уберёт значок) и окна установщика, запущенные из каталога, —
// кроме своего процесса. Окна видны только в своём сеансе; трей других пользователей завершается принудительно.
BOOL CALLBACK closeOwnWindows(HWND hwnd, LPARAM lp) {
    auto* st = reinterpret_cast<OwnWindows*>(lp);
    wchar_t cls[64];
    if (!GetClassNameW(hwnd, cls, 64)) return TRUE;
    std::wstring c = cls;
    if (c != L"EmulkonTrayMain" && c != L"EmulkonSetupWnd") return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() && isInside(processPath(pid), st->dir)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        st->pids.push_back(pid);
    }
    return TRUE;
}

void writeUninstallEntry(const std::wstring& dir) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    std::wstring setupExe = binDir(dir) + L"\\EmulkonSetup.exe";
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t date[16];
    wcsftime(date, 16, L"%Y%m%d", &tm);
    regSet(key, L"DisplayName", L"Эмулкон");
    regSet(key, L"DisplayVersion", version());
    regSet(key, L"Publisher", L"imiron.ru");
    regSet(key, L"HelpLink", L"https://imiron.ru");
    regSet(key, L"Comments", L"Эмулятор подключаемого оборудования для 1С:БПО — служба эмуляции, трей и драйвер");
    regSet(key, L"DisplayIcon", trayExe(dir) + L",0");
    regSet(key, L"InstallLocation", dir);
    regSet(key, L"InstallDate", date);
    regSet(key, L"UninstallString", L"\"" + setupExe + L"\" /uninstall");
    regSet(key, L"QuietUninstallString", L"\"" + setupExe + L"\" /uninstall /quiet");
    // «Изменить» открывает мастер обслуживания: изменить, восстановить, удалить (docs/local-install.md §3).
    // Отдельную кнопку «Восстановить» Windows показывает только MSI-пакетам — она внутри мастера.
    regSet(key, L"ModifyPath", L"\"" + setupExe + L"\" /maintain");
    RegDeleteValueW(key, L"NoModify");
    regSetDword(key, L"NoRepair", 1);
    regSet(key, L"URLInfoAbout", std::wstring(kServerUrl) + L"/downloads");
    regSetDword(key, L"EstimatedSize", static_cast<DWORD>(treeSize(dir) / 1024));
    RegCloseKey(key);

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kAppKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        regSet(key, L"InstallDir", dir);
        regSet(key, L"Version", version());
        RegCloseKey(key);
    }
}

// writeServerSettings — HKLM\Software\Emulkon\Server: сервер для касс в сети (Network); остальное не трогаем —
// это настройки администратора (мастер «Изменить», оснастка).
bool writeServerSettings(const Options& o, std::wstring& error) {
    if (!secureServerKey(error)) return false;
    HKEY key;
    LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServerKey, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        error = winError(static_cast<DWORD>(rc));
        return false;
    }
    bool ok = regSetDword(key, L"Network", o.network ? 1 : 0);
    RegCloseKey(key);
    if (!ok) error = L"не записано значение Network";
    return ok;
}

// warnOpenNetwork — сервер для касс в сети: предупредить о токенах, которых не хватает.
void warnOpenNetwork(const Sink& sink) {
    const bool control = !regGet(HKEY_LOCAL_MACHINE, kServerKey, L"ControlTokenHash").empty();
    const bool device = !regGet(HKEY_LOCAL_MACHINE, kServerKey, L"DeviceTokenHash").empty();
    if (control && device) return;
    std::wstring missing = !control && !device ? L"токенов управления и устройств"
                           : !control         ? L"токена управления"
                                              : L"токена устройств";
    report(sink, Status::Warn, L"Кассы в сети",
           L"сервер открыт в сеть без " + missing + L": " +
               (!control ? L"управлять может любой в сети. " : L"подключиться может любой в сети. ") +
               L"Токены выдаются в консоли: «Эмулкон → Служба → Свойства»");
}

// startTrayAsUser — трей запускается через проводник: он работает от имени пользователя без повышенных прав,
// а установщик — с правами администратора.
bool startTrayAsUser(const std::wstring& exe) {
    std::wstring args = L"\"" + exe + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

// migrateLegacy переносит прежнюю установку в профиле (до 0.8.0, docs/local-install.md §1.8): данные сервера —
// в ProgramData (если там ещё пусто), копия — в backup; программа, ярлыки и записи в реестре удаляются.
void migrateLegacy(const Installed& legacy, const Sink& sink) {
    const std::wstring from = userDataDir() + L"\\data";
    const std::wstring to = machineDataDir() + L"\\data";
    if (dirExists(from)) {
        std::wstring backup = machineDataDir() + L"\\backup\\from-profile-" + stamp() + L"\\data";
        int failed = copyTree(from, backup);
        if (fileExists(to + L"\\registry.json") || fileExists(to + L"\\scenarios.json")) {
            report(sink, failed ? Status::Warn : Status::Info, L"Данные прежней установки",
                   L"в " + to + L" уже есть данные службы — не заменены; данные из профиля сохранены в " + backup);
        } else {
            failed += copyTree(from, to);
            report(sink, failed ? Status::Warn : Status::Ok, L"Данные прежней установки",
                   failed ? L"перенесены не все файлы (" + std::to_wstring(failed) + L") из " + from
                          : L"устройства, рабочие места, сценарии и журнал перенесены из " + from + L" в " + to +
                                L"; копия — " + backup);
        }
    }
    if (!legacy.present) return;

    if (isInside(unquote(regGet(HKEY_CURRENT_USER, kRunKey, kRunValue)), legacy.dir)) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
    removeTree(userStartMenuDir());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);
    bool ours = fileExists(legacy.dir + L"\\EmulkonTray.exe") || fileExists(legacy.dir + L"\\emulkon.exe");
    int left = ours ? removeTree(legacy.dir) : 0;
    if (ours && dirExists(from)) removeTree(from);  // перенесено и сохранено в backup
    report(sink, left ? Status::Warn : Status::Ok, L"Прежняя установка " + legacy.version,
           left ? L"удалена не полностью: " + std::to_wstring(left) + L" файлов заняты — удалите " + legacy.dir + L" вручную"
                : L"удалена из профиля: " + legacy.dir + L", ярлыки, автозапуск и запись в «Установленных приложениях»");
}

}  // namespace

Installed findInstalled() {
    Installed r;
    r.dir = regGet(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation");
    r.version = regGet(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion");
    r.present = !r.dir.empty();
    return r;
}

Options currentOptions() {
    Options o;
    Installed inst = findInstalled();
    o.dir = inst.dir;
    o.autostart = !regGet(HKEY_LOCAL_MACHINE, kRunKey, kRunValue).empty();
    DWORD v = 0, size = sizeof v;
    o.network = RegGetValueW(HKEY_LOCAL_MACHINE, kServerKey, L"Network", RRF_RT_REG_DWORD, nullptr, &v, &size) ==
                    ERROR_SUCCESS &&
                v == 1;
    return o;
}

bool modify(const Options& o, const Sink& sink) {
    Installed inst = findInstalled();
    if (!inst.present) {
        report(sink, Status::Fail, L"Эмулкон не найден", L"нет записи об установке — сначала установите");
        return false;
    }
    const Options was = currentOptions();
    std::wstring error;

    HKEY run;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &run, nullptr) == ERROR_SUCCESS) {
        if (o.autostart) {
            regSet(run, kRunValue, L"\"" + trayExe(inst.dir) + L"\"");
        } else {
            RegDeleteValueW(run, kRunValue);
        }
        RegCloseKey(run);
    }
    report(sink, Status::Ok, L"Автозапуск трея",
           std::wstring(o.autostart ? L"у каждого пользователя при входе в Windows" : L"выключен") +
               (o.autostart != was.autostart ? L" (изменено)" : L""));

    if (!writeServerSettings(o, error)) {
        report(sink, Status::Fail, L"Настройки службы", L"не записаны: " + error);
        return false;
    }
    report(sink, Status::Ok, L"Сервер",
           std::wstring(o.network ? L"для касс в сети — 0.0.0.0:8737" : L"только этот компьютер — 127.0.0.1:8737") +
               (o.network != was.network ? L" (изменено)" : L""));
    if (setFirewallRule(o.network, serverExe(inst.dir), error)) {
        report(sink, Status::Ok, L"Брандмауэр",
               o.network ? L"входящие TCP 8737 для emulkon.exe разрешены" : L"правила нет — сервер только для этого компьютера");
    } else {
        report(sink, Status::Warn, L"Брандмауэр", L"правило не изменено: " + error);
    }
    if (o.network) warnOpenNetwork(sink);

    // Адрес прослушивания служба читает при запуске.
    if (o.network != was.network) {
        if (stopService(error) && startService(error)) {
            report(sink, Status::Ok, L"Служба Emulkon", L"перезапущена с новым адресом");
        } else {
            report(sink, Status::Fail, L"Служба Emulkon", error);
            return false;
        }
    }
    return true;
}

Installed findLegacyInstalled() {
    Installed r;
    r.dir = regGet(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
    r.version = regGet(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion");
    r.present = !r.dir.empty();
    return r;
}

void stopInstance(const std::wstring& dir, const Sink& sink) {
    if (dir.empty() || !dirExists(dir)) return;
    OwnWindows st{dir, {}};
    EnumWindows(closeOwnWindows, reinterpret_cast<LPARAM>(&st));
    for (DWORD pid : st.pids) waitExit(pid, 10000);
    enableDebugPrivilege();
    int killed = 0;
    for (const Proc& p : processesIn(dir)) {
        // Принудительно — только сервер и трей. Установщик из этого же каталога может
        // ждать нас (удаление из «Параметров») — его не трогаем.
        std::wstring name = fileName(p.path);
        if (_wcsicmp(name.c_str(), L"emulkon.exe") != 0 && _wcsicmp(name.c_str(), L"EmulkonTray.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p.pid);
        if (!h) continue;
        if (TerminateProcess(h, 0)) {
            WaitForSingleObject(h, 5000);
            ++killed;
        }
        CloseHandle(h);
    }
    if (!st.pids.empty() || killed) {
        report(sink, Status::Info, L"Запущенный Эмулкон",
               L"остановлен: окон закрыто — " + std::to_wstring(st.pids.size()) + L", процессов завершено — " +
                   std::to_wstring(killed));
    }
}

bool install(const Options& o, const Sink& sink) {
    ComInit com;
    Installed prev = findInstalled();
    Installed legacy = findLegacyInstalled();
    std::wstring dir = !o.dir.empty() ? o.dir : (prev.present ? prev.dir : defaultInstallDir());
    report(sink, Status::Info, L"Каталог установки",
           dir + (prev.present ? L" — обновление версии " + prev.version + L" до " + version() : std::wstring()));

    std::wstring error;
    if (serviceState() != ServiceState::Missing) {
        if (!stopService(error)) report(sink, Status::Warn, L"Служба Emulkon", L"не остановлена: " + error);
    }
    if (prev.present && !samePath(prev.dir, dir)) stopInstance(prev.dir, sink);
    stopInstance(dir, sink);
    if (legacy.present) stopInstance(legacy.dir, sink);

    // 1. Файлы программы (docs/local-install.md §2).
    for (const std::wstring& d : {binDir(dir), driverDir(dir), epfDir(dir), setupDir(dir), consoleDir(dir)}) {
        if (!ensureDir(d)) {
            report(sink, Status::Fail, L"Каталог установки", L"не удалось создать " + d + L": " + winError(GetLastError()));
            return false;
        }
    }
    const struct {
        const wchar_t* resource;
        std::wstring path;
    } files[] = {
        {L"SERVER", serverExe(dir)},
        {L"TRAY", trayExe(dir)},
        {L"DRIVERZIP", driverDir(dir) + L"\\" + driverPackageName()},
        {L"DRIVER64", driverDir(dir) + L"\\" + driverDllName(true)},
        {L"DRIVER32", driverDir(dir) + L"\\" + driverDllName(false)},
        {L"SNAPIN", snapinDll(dir)},
    };
    for (const auto& f : files) {
        if (!extract(f.resource, f.path, error)) {
            report(sink, Status::Fail, L"Файлы программы", f.path + L": " + error);
            return false;
        }
    }
    // Установщик — он же мастер обслуживания и «Проверка работы»; его же раздаёт страница загрузок.
    const std::wstring self = selfPath();
    for (const std::wstring& to : {binDir(dir) + L"\\EmulkonSetup.exe", setupDir(dir) + L"\\" + setupDownloadName()}) {
        if (samePath(self, to)) continue;
        if (!copyFileAtomic(self, to, error)) {
            report(sink, Status::Fail, L"Файлы программы", to + L": " + error);
            return false;
        }
    }
    removeStale(driverDir(dir));
    removeStale(setupDir(dir));
    removeStale(binDir(dir));
    removeStale(consoleDir(dir));
    report(sink, Status::Ok, L"Файлы программы",
           dir + L": bin (служба, трей, мастер), console (оснастка MMC), 1C\\driver (драйвер x64, x86), setup (установщик для других компьютеров)");

    // 2. Данные службы — ProgramData, прежняя установка — переносится.
    const std::wstring data = machineDataDir();
    for (const wchar_t* sub : {L"\\data", L"\\logs", L"\\backup", L"\\updates"}) ensureDir(data + sub);
    migrateLegacy(legacy, sink);
    if (secureMachineData(data, error)) {
        report(sink, Status::Ok, L"Данные службы", data + L": data, logs, backup, updates — пишут служба и администраторы");
    } else {
        report(sink, Status::Warn, L"Данные службы", L"права на " + data + L" не выставлены: " + error);
    }

    // 3. Настройки, журнал событий, служба, брандмауэр.
    if (writeServerSettings(o, error)) {
        report(sink, Status::Ok, L"Настройки службы",
               L"HKLM\\Software\\Emulkon\\Server: " +
                   std::wstring(o.network ? L"кассы в сети — 0.0.0.0:8737" : L"только этот компьютер — 127.0.0.1:8737"));
    } else {
        report(sink, Status::Warn, L"Настройки службы", L"не записаны: " + error);
    }
    if (o.network) warnOpenNetwork(sink);
    if (registerEventSource(error)) {
        report(sink, Status::Ok, L"Журнал событий", L"источник Emulkon в журнале «Приложение»: запуск, остановка, сбои");
    } else {
        report(sink, Status::Warn, L"Журнал событий", L"источник не зарегистрирован: " + error);
    }
    if (!installService(serverExe(dir), error)) {
        report(sink, Status::Fail, L"Служба Emulkon", error);
        return false;
    }
    report(sink, Status::Ok, L"Служба Emulkon",
           L"«Эмулкон — сервер эмуляции оборудования»: автозапуск, LocalService, перезапуск при сбое; пользователи "
           L"могут запускать и останавливать");
    if (setFirewallRule(o.network, serverExe(dir), error)) {
        report(sink, Status::Ok, L"Брандмауэр",
               o.network ? L"входящие TCP 8737 для emulkon.exe разрешены" : L"правило не нужно — сервер только для этого компьютера");
    } else {
        report(sink, Status::Warn, L"Брандмауэр", L"правило не создано: " + error);
    }

    // 4. «Установленные приложения», меню «Пуск», автозапуск трея.
    writeUninstallEntry(dir);
    report(sink, Status::Ok, L"«Установленные приложения»",
           L"Эмулкон " + version() + L" — для всех пользователей, удаление через «Параметры → Приложения»");

    // Оснастка MMC и готовая консоль (docs/local-install.md §4–§5).
    const std::wstring msc = consoleFile(dir);
    bool console = registerSnapIn(snapinDll(dir), error) && createConsoleFile(msc, error);
    report(sink, console ? Status::Ok : Status::Warn, L"Консоль управления",
           console ? msc + L" — оснастка «Эмулкон» в MMC" : L"не создана: " + error);

    const std::wstring menu = startMenuDir();
    const std::wstring tray = trayExe(dir);
    removeTree(menu);  // ярлыки прежних версий
    bool links = ensureDir(menu) &&
                 (!console || createShortcut(menu + L"\\Эмулкон — консоль.lnk", msc, L"",
                                             L"Консоль управления Эмулконом (MMC): служба, рабочие места, устройства", tray)) &&
                 createShortcut(menu + L"\\Эмулкон.lnk", tray, L"", L"Трей Эмулкона: состояние службы, всплывающие чеки, печать",
                                tray) &&
                 writeUrlShortcut(menu + L"\\Эмулкон — веб-интерфейс.url", std::wstring(kServerUrl) + L"/", tray) &&
                 createShortcut(menu + L"\\Эмулкон — загрузки для 1С.lnk", dir + L"\\1C", L"",
                                L"Пакет драйвера для «Драйверов оборудования» и обработки для 1С", tray) &&
                 createShortcut(menu + L"\\Эмулкон — проверка работы.lnk", binDir(dir) + L"\\EmulkonSetup.exe", L"/check",
                                L"Проверить, что служба, сервер и драйвер работают", tray);
    report(sink, links ? Status::Ok : Status::Warn, L"Меню «Пуск»",
           links ? L"папка «Эмулкон» для всех пользователей: консоль, трей, веб-интерфейс, загрузки для 1С, проверка работы"
                 : L"не все ярлыки созданы в " + menu);

    // Панель управления открывает консоль; без неё — веб-интерфейс (docs/local-install.md §4).
    const std::wstring cplCommand = console ? L"\"%SystemRoot%\\System32\\mmc.exe\" \"" + msc + L"\""
                                            : L"\"%SystemRoot%\\explorer.exe\" " + std::wstring(kServerUrl) + L"/";
    if (registerControlPanel(tray + L",0", cplCommand, error)) {
        report(sink, Status::Ok, L"Панель управления",
               std::wstring(L"элемент «Эмулкон» в «Оборудование и звук» и «Программы» — открывает ") +
                   (console ? L"консоль" : L"веб-интерфейс"));
    } else {
        report(sink, Status::Warn, L"Панель управления", error);
    }

    HKEY run;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &run,
                        nullptr) == ERROR_SUCCESS) {
        if (o.autostart) {
            regSet(run, kRunValue, L"\"" + tray + L"\"");
        } else {
            RegDeleteValueW(run, kRunValue);
        }
        RegCloseKey(run);
    }
    report(sink, Status::Ok, L"Автозапуск трея",
           o.autostart ? L"у каждого пользователя при входе в Windows" : L"выключен");

    // Трей этого пользователя больше не поднимает сервер сам — это служба.
    tray::Config cfg;
    const std::wstring cfgPath = tray::defaultConfigPath();
    tray::loadConfig(cfgPath, cfg);
    cfg.serverAutostart = false;
    cfg.serverExe.clear();
    cfg.serverData.clear();
    cfg.serverListen.clear();
    tray::saveConfig(cfgPath, cfg);

    // 5. Запуск.
    if (startService(error)) {
        report(sink, Status::Ok, L"Запуск службы", L"служба Emulkon работает");
    } else {
        report(sink, Status::Fail, L"Запуск службы", error);
        return false;
    }
    if (o.run) {
        if (startTrayAsUser(tray)) {
            report(sink, Status::Ok, L"Трей", L"запущен — значок «Э» у часов");
        } else {
            report(sink, Status::Warn, L"Трей", L"не запустился: " + winError(GetLastError()));
        }
    }
    return true;
}

bool uninstall(const Options& o, const Sink& sink) {
    Installed inst = findInstalled();
    std::wstring dir = !o.dir.empty() ? o.dir : inst.dir;
    if (dir.empty()) {
        report(sink, Status::Fail, L"Эмулкон не найден", L"в реестре нет записи об установке — укажите каталог: /dir");
        return false;
    }
    std::wstring error;
    if (deleteService(error)) {
        report(sink, Status::Ok, L"Служба Emulkon", L"остановлена и удалена");
    } else {
        report(sink, Status::Warn, L"Служба Emulkon", error);
    }
    setFirewallRule(false, {}, error);
    unregisterEventSource();
    unregisterControlPanel();
    unregisterSnapIn();
    stopInstance(dir, sink);

    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kRunKey, kRunValue);
    removeTree(startMenuDir());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kAppKey);
    report(sink, Status::Ok, L"Ярлыки и регистрация",
           L"удалены: папка «Эмулкон» в меню «Пуск», элемент Панели управления, оснастка MMC, автозапуск трея, правило брандмауэра, источник журнала событий, "
           L"запись в «Установленных приложениях»");

    // Каталог удаляем целиком, только если в нём программа Эмулкона: путь пришёл из реестра.
    bool ours = fileExists(trayExe(dir)) || fileExists(serverExe(dir)) || fileExists(binDir(dir) + L"\\EmulkonSetup.exe");
    if (!ours) {
        report(sink, Status::Warn, L"Файлы программы", L"в " + dir + L" нет файлов Эмулкона — каталог не тронут");
    } else if (int left = removeTree(dir, selfPath())) {
        report(sink, Status::Warn, L"Файлы программы",
               L"не удалено файлов: " + std::to_wstring(left) + L" — они заняты; удалите " + dir + L" вручную");
    } else {
        report(sink, Status::Ok, L"Файлы программы",
               L"удалены: " + dir + (isInside(selfPath(), dir) ? L" (сам мастер — сразу после выхода)" : L""));
    }

    if (o.purge) {
        int left = removeTree(machineDataDir()) + removeTree(userDataDir()) + removeTree(userConfigDir());
        if (left) {
            report(sink, Status::Warn, L"Данные",
                   L"часть файлов занята (кэш драйвера используется запущенной 1С) — удалите " + userDataDir() +
                       L" после её закрытия");
        } else {
            report(sink, Status::Ok, L"Данные",
                   L"удалены: данные службы (" + machineDataDir() + L"), настройки трея и кэш драйвера этого пользователя");
        }
    } else {
        report(sink, Status::Info, L"Данные оставлены",
               L"данные службы, журнал и копии — " + machineDataDir() + L"; настройки трея — " + userConfigDir());
    }
    report(sink, Status::Info, L"Драйвер в базах 1С", L"удаляется в самой 1С: справочник «Драйверы оборудования»");
    return true;
}

void scheduleSelfDelete(const std::wstring& dir) {
    // ping — пауза ~2 с, пока процесс завершится; rmdir без /s удаляет только пустые каталоги: bin, затем корень.
    std::wstring cmd = L"cmd.exe /d /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + selfPath() + L"\" & rmdir \"" +
                       binDir(dir) + L"\" & rmdir \"" + dir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

}  // namespace setup
