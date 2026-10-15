// Самообновление драйвера: кэш, скачивание и фоновый поток (Windows).
#include "update.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <set>

#include "text.h"

// Имя DLL этой сборки (EmulkonDriver_win64.dll / _win32.dll) — задаёт CMake.
#ifndef EMULKON_DLL_FILE
#define EMULKON_DLL_FILE "EmulkonDriver.dll"
#endif

namespace emu {

namespace {

std::wstring env(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

std::wstring widen(const std::string& s) {
    std::u16string u = utf8to16(s);
    return std::wstring(u.begin(), u.end());
}

std::string narrow(const std::wstring& w) { return utf16to8(std::u16string(w.begin(), w.end())); }

std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

bool isDir(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ensureDir создаёт каталог со всеми родителями.
bool ensureDir(const std::wstring& path) {
    if (isDir(path)) return true;
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') {
            std::wstring part = path.substr(0, i);
            if (part.size() > 2 && !isDir(part)) CreateDirectoryW(part.c_str(), nullptr);
        }
    }
    return isDir(path);
}

bool readFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64LL << 20);
    if (ok) {
        out.assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        ok = out.empty() || (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool writeFile(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                               written == data.size());
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

std::string winError(DWORD code) { return "код " + std::to_string(code); }

// cleanupOlder удаляет из кэша DLL этой архитектуры старше version. Загруженную
// (её использует другой процесс 1С) Windows удалить не даст — она останется до следующего раза.
void cleanupOlder(const std::string& version) {
    std::wstring root = updateCacheRoot();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> old;
    do {
        std::string name = narrow(fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && validVersion(name) && compareVersions(name, version) < 0) {
            old.push_back(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const auto& v : old) {
        DeleteFileW(cachedDllPath(v).c_str());
        RemoveDirectoryW((root + L"\\" + widen(v)).c_str());  // только если каталог опустел
    }
}

// --- фоновое скачивание ---

std::mutex g_mu;
std::set<std::string> g_started;  // версии, за которыми уже ходили в этом процессе

struct Job {
    std::string server;
    UpdateOffer offer;
    int timeoutMs;
    std::string workstation;
    HMODULE self;
};

DWORD WINAPI updateThread(LPVOID arg) {
    HMODULE self = nullptr;
    {
        std::unique_ptr<Job> job(static_cast<Job*>(arg));
        self = job->self;
        std::unique_ptr<HttpClient> http = makeHttpClient(std::string("Emulkon-Driver/") + EMULKON_VERSION);
        std::string message;
        installUpdate(*http, job->server, job->offer, job->timeoutMs, job->workstation, message);
    }
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

}  // namespace

std::wstring updateCacheRoot() {
    std::wstring custom = env(L"EMULKON_DRIVER_CACHE");
    if (!custom.empty()) return custom;
    std::wstring local = env(L"LOCALAPPDATA");
    return local.empty() ? std::wstring() : local + L"\\Emulkon\\driver";
}

std::wstring cachedDllPath(const std::string& version) {
    return updateCacheRoot() + L"\\" + widen(version) + L"\\" + widen(EMULKON_DLL_FILE);
}

std::vector<std::string> cachedVersions() {
    std::vector<std::string> out;
    std::wstring root = updateCacheRoot();
    if (root.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string name = narrow(fd.cFileName);
        if (!validVersion(name)) continue;
        if (GetFileAttributesW(cachedDllPath(name).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

bool updateCached(const UpdateOffer& offer) {
    std::string data;
    return offer.valid() && readFile(cachedDllPath(offer.version), data) && sha256Hex(data) == offer.sha256;
}

bool redirectDisabled() { return env(L"EMULKON_DRIVER_NO_UPDATE") == L"1"; }

void rememberServer(const std::string& url) {
    std::wstring root = updateCacheRoot();
    if (root.empty() || url.empty()) return;
    if (rememberedServer() == url) return;
    if (ensureDir(root)) writeFile(root + L"\\server.txt", url);
}

std::string rememberedServer() {
    std::string s;
    std::wstring root = updateCacheRoot();
    if (root.empty() || !readFile(root + L"\\server.txt", s)) return {};
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

bool installUpdate(HttpClient& http, const std::string& serverUrl, const UpdateOffer& offer, int timeoutMs,
                   const std::string& workstation, std::string& message) {
    if (!offer.valid()) {
        message = "сервер прислал неверное описание обновления";
        return false;
    }
    std::wstring root = updateCacheRoot();
    if (root.empty()) {
        message = "не задан каталог LOCALAPPDATA";
        return false;
    }
    if (updateCached(offer)) return true;

    Headers headers;
    if (!workstation.empty()) headers.emplace_back("X-Emulkon-Workstation", workstation);
    HttpResponse r = http.get(trimSlash(serverUrl) + offer.url, headers, timeoutMs > 0 ? timeoutMs : 60000);
    if (!r.error.empty()) {
        message = "скачать " + offer.url + ": " + r.error;
        return false;
    }
    if (r.status != 200) {
        message = "скачать " + offer.url + ": HTTP " + std::to_string(r.status);
        return false;
    }
    if (offer.size > 0 && static_cast<int64_t>(r.body.size()) != offer.size) {
        message = "скачано " + std::to_string(r.body.size()) + " байт вместо " + std::to_string(offer.size);
        return false;
    }
    if (sha256Hex(r.body) != offer.sha256) {
        message = "контрольная сумма скачанного файла не совпала с объявленной сервером";
        return false;
    }
    if (r.body.size() < 2 || r.body[0] != 'M' || r.body[1] != 'Z') {
        message = "скачанный файл — не DLL";
        return false;
    }

    std::wstring dir = root + L"\\" + widen(offer.version);
    if (!ensureDir(dir)) {
        message = "создать каталог " + narrow(dir) + ": " + winError(GetLastError());
        return false;
    }
    // Сначала во временный файл, потом переименование: недокачанная DLL не получит рабочее имя.
    std::wstring path = cachedDllPath(offer.version);
    std::wstring tmp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    if (!writeFile(tmp, r.body)) {
        message = "записать " + narrow(tmp) + ": " + winError(GetLastError());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD code = GetLastError();
        DeleteFileW(tmp.c_str());
        if (updateCached(offer)) return true;  // параллельно скачал другой процесс 1С
        message = "сохранить " + narrow(path) + ": " + winError(code);
        return false;
    }
    cleanupOlder(offer.version);
    return true;
}

UpdateCheck checkForUpdate(HttpClient& http, const std::string& serverUrl, int timeoutMs, const std::string& workstation,
                           std::string& message) {
    const std::string server = trimSlash(serverUrl);
    HttpResponse r = http.get(server + "/api/v1/driver/update?version=" EMULKON_VERSION "&arch=" + driverArch(), {},
                              timeoutMs);
    if (!r.error.empty()) {
        message = "Нет связи с сервером эмуляции " + server + ": " + r.error;
        return UpdateCheck::Failed;
    }
    if (r.status == 404) {
        message = "Сервер эмуляции " + server + " не раздаёт обновления драйвера (сервер старше версии 0.2.0)";
        return UpdateCheck::Failed;
    }
    Json resp;
    std::string err;
    if (r.status != 200 || !parseJson(r.body, resp, err)) {
        message = "Сервер эмуляции ответил на проверку обновления не по протоколу (HTTP " + std::to_string(r.status) + ")";
        return UpdateCheck::Failed;
    }
    const Json* avail = resp.get("update_available");
    UpdateOffer offer = parseUpdateOffer(resp.get("latest"));
    if (!(avail && avail->isBool() && avail->b)) {
        message = "Установлена последняя версия драйвера: " EMULKON_VERSION ".";
        if (!offer.valid()) message += " На сервере " + server + " файлы драйвера не опубликованы.";
        return UpdateCheck::UpToDate;
    }
    if (!offer.valid()) {
        message = "Сервер эмуляции прислал неверное описание обновления";
        return UpdateCheck::Failed;
    }
    if (updateCached(offer)) {
        message = "Версия драйвера " + offer.version + " уже загружена и начнёт работать после перезапуска 1С (сейчас " +
                  EMULKON_VERSION + ").";
        return UpdateCheck::Pending;
    }
    std::string why;
    if (!installUpdate(http, server, offer, timeoutMs, workstation, why)) {
        message = "Не удалось загрузить версию драйвера " + offer.version + ": " + why;
        return UpdateCheck::Failed;
    }
    message = "Загружена версия драйвера " + offer.version + ". Она начнёт работать после перезапуска 1С (сейчас " +
              EMULKON_VERSION + ").";
    return UpdateCheck::Downloaded;
}

void startBackgroundUpdate(const std::string& serverUrl, const UpdateOffer& offer, int timeoutMs,
                           const std::string& workstation) {
    if (!offer.valid()) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_started.insert(offer.version).second) return;
    }
    if (updateCached(offer)) return;
    // Ссылка на свою DLL: 1С может выгрузить компоненту, пока идёт скачивание.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&updateThread), &self);
    Job* job = new Job{serverUrl, offer, timeoutMs, workstation, self};
    HANDLE t = CreateThread(nullptr, 0, updateThread, job, 0, nullptr);
    if (!t) {
        delete job;
        if (self) FreeLibrary(self);
        return;
    }
    CloseHandle(t);
}

}  // namespace emu
