// Платформенный слой компоненты — Windows (platform.h).
#include "platform.h"

#include <windows.h>

#include <memory>

#include "text.h"

namespace emu {
namespace sys {

const char kSep = '\\';

namespace {

std::wstring wide(const std::string& s) {
    std::u16string u = utf8to16(s);
    return std::wstring(u.begin(), u.end());
}

std::string utf8(const std::wstring& w) { return utf16to8(std::u16string(w.begin(), w.end())); }

struct Job {
    std::function<void()> fn;
    HMODULE self;
};

DWORD WINAPI jobThread(LPVOID arg) {
    HMODULE self = nullptr;
    {
        std::unique_ptr<Job> job(static_cast<Job*>(arg));
        self = job->self;
        job->fn();
    }
    // Ссылку на свою DLL отпускаем последним действием потока: код потока остаётся в памяти до выхода.
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

}  // namespace

std::string env(const char* name) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(wide(name).c_str(), buf, 1024);
    return n > 0 && n < 1024 ? utf8(std::wstring(buf, n)) : std::string();
}

void setEnv(const char* name, const char* value) {
    SetEnvironmentVariableW(wide(name).c_str(), value ? wide(value).c_str() : nullptr);
}

bool isDir(const std::string& path) {
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isFile(const std::string& path) {
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensureDir(const std::string& path) {
    if (isDir(path)) return true;
    std::wstring w = wide(path);
    for (size_t i = 1; i <= w.size(); ++i) {
        if (i == w.size() || w[i] == L'\\' || w[i] == L'/') {
            std::wstring part = w.substr(0, i);
            if (part.size() > 2 && GetFileAttributesW(part.c_str()) == INVALID_FILE_ATTRIBUTES) CreateDirectoryW(part.c_str(), nullptr);
        }
    }
    return isDir(path);
}

bool readFile(const std::string& path, std::string& out) {
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
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

bool writeFile(const std::string& path, const std::string& data) {
    std::wstring w = wide(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                               written == data.size());
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok) DeleteFileW(w.c_str());
    return ok;
}

bool replaceFile(const std::string& from, const std::string& to) {
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void removeFile(const std::string& path) { DeleteFileW(wide(path).c_str()); }

void removeDir(const std::string& path) { RemoveDirectoryW(wide(path).c_str()); }

std::vector<std::string> listDirs(const std::string& path) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wide(path) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") out.push_back(utf8(name));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::string tempDir() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    std::string s = n ? utf8(std::wstring(buf, n)) : std::string(".\\");
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

long processId() { return static_cast<long>(GetCurrentProcessId()); }

std::string lastError() { return "код " + std::to_string(GetLastError()); }

std::string defaultCacheRoot() {
    std::string local = env("LOCALAPPDATA");
    return local.empty() ? std::string() : local + "\\Emulkon\\driver";
}

void runDetached(std::function<void()> fn) {
    // Ссылка на свою DLL: поток держит её загруженной, пока работает.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&jobThread), &self);
    Job* job = new Job{std::move(fn), self};
    HANDLE t = CreateThread(nullptr, 0, jobThread, job, 0, nullptr);
    if (!t) {
        delete job;
        if (self) FreeLibrary(self);
        return;
    }
    CloseHandle(t);
}

}  // namespace sys
}  // namespace emu
