// Пути и мелкие проверки строк — без побочных эффектов (их проверяют модульные тесты).
#include <windows.h>
#include <shlobj.h>

#include <cwctype>

#include "doc.h"  // widen
#include "setup.h"

namespace setup {

namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : s) {
        if (c == L'/') c = L'\\';
    }
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

}  // namespace

std::wstring version() { return widen(EMULKON_VERSION); }

void Tally::add(Status s) {
    switch (s) {
        case Status::Ok: ++ok; break;
        case Status::Info: ++info; break;
        case Status::Warn: ++warn; break;
        case Status::Fail: ++fail; break;
    }
}

void Tally::add(const Item& it) {
    add(it.status);
    if (!it.fix.empty() && (it.status == Status::Fail || it.status == Status::Warn)) {
        ++fixable;
        if (it.status == Status::Fail) ++fixableFail;
    }
}

const wchar_t* statusText(Status s) {
    switch (s) {
        case Status::Ok: return L"✔ готово";
        case Status::Info: return L"• сведения";
        case Status::Warn: return L"⚠ внимание";
        case Status::Fail: return L"✖ ошибка";
    }
    return L"";
}

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? std::wstring(buf, n) : std::wstring();
}

std::wstring defaultInstallDir() {
    std::wstring base = knownFolder(FOLDERID_ProgramFilesX64);
    if (base.empty()) base = envVar(L"ProgramW6432");
    if (base.empty()) base = envVar(L"ProgramFiles");
    return base + L"\\Emulkon";
}

std::wstring machineDataDir() {
    std::wstring base = knownFolder(FOLDERID_ProgramData);
    if (base.empty()) base = envVar(L"ProgramData");
    return base + L"\\Emulkon";
}

std::wstring binDir(const std::wstring& dir) { return dir + L"\\bin"; }
std::wstring driverDir(const std::wstring& dir) { return dir + L"\\1C\\driver"; }
std::wstring epfDir(const std::wstring& dir) { return dir + L"\\1C\\epf"; }
std::wstring setupDir(const std::wstring& dir) { return dir + L"\\setup"; }
std::wstring consoleDir(const std::wstring& dir) { return dir + L"\\console"; }
std::wstring snapinDll(const std::wstring& dir) { return consoleDir(dir) + L"\\EmulkonSnapIn.dll"; }
std::wstring consoleFile(const std::wstring& dir) { return consoleDir(dir) + L"\\Emulkon.msc"; }
std::wstring serverExe(const std::wstring& dir) { return binDir(dir) + L"\\emulkon.exe"; }
std::wstring trayExe(const std::wstring& dir) { return binDir(dir) + L"\\EmulkonTray.exe"; }

std::wstring userDataDir() {
    std::wstring base = knownFolder(FOLDERID_LocalAppData);
    if (base.empty()) base = envVar(L"LOCALAPPDATA");
    return base + L"\\Emulkon";
}

std::wstring userConfigDir() {
    std::wstring base = knownFolder(FOLDERID_RoamingAppData);
    if (base.empty()) base = envVar(L"APPDATA");
    return base + L"\\Emulkon";
}

std::wstring startMenuDir() { return knownFolder(FOLDERID_CommonPrograms) + L"\\Эмулкон"; }
std::wstring userStartMenuDir() { return knownFolder(FOLDERID_Programs) + L"\\Эмулкон"; }

std::wstring selfPath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

std::wstring parentDir(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::wstring fileName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

bool samePath(const std::wstring& a, const std::wstring& b) { return !a.empty() && lower(a) == lower(b); }

bool isInside(const std::wstring& path, const std::wstring& dir) {
    if (path.empty() || dir.empty()) return false;
    std::wstring p = lower(path), d = lower(dir);
    return p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == L'\\';
}

bool fileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring driverPackageName() { return L"EmulkonDriver_" + version() + L".zip"; }
std::wstring driverDllName(bool x64) { return L"EmulkonDriver_" + version() + (x64 ? L"_win64.dll" : L"_win32.dll"); }
std::wstring setupDownloadName() { return L"EmulkonSetup_" + version() + L".exe"; }

bool isLocalUrl(const std::wstring& url) {
    std::wstring u = url;  // не lower(): та нормализует путь и заменила бы «/» на «\»
    for (auto& c : u) c = static_cast<wchar_t>(std::towlower(c));
    while (u.size() > 7 && u.back() == L'/') u.pop_back();
    for (const wchar_t* p : {L"http://127.0.0.1:", L"http://localhost:", L"http://[::1]:", L"http://127.0.0.1/",
                             L"http://localhost/"}) {
        if (u.rfind(p, 0) == 0) return true;
    }
    return u == L"http://127.0.0.1" || u == L"http://localhost";
}

bool isPlatformVersion(const std::wstring& s) {
    int dots = 0;
    bool digit = false;
    for (wchar_t c : s) {
        if (c == L'.') {
            if (!digit) return false;
            ++dots;
            digit = false;
        } else if (c >= L'0' && c <= L'9') {
            digit = true;
        } else {
            return false;
        }
    }
    return dots == 3 && digit && s.rfind(L"8.", 0) == 0;
}

}  // namespace setup
