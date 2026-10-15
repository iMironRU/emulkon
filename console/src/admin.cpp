// Действия администратора из оснастки (docs/local-install.md §4.1, этап 2): служба, токены, сеть.
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "summary.h"

namespace console {

namespace {

std::wstring winError(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s + L" (код " + std::to_wstring(code) + L")";
}

// waitState ждёт состояния службы до 30 с.
bool waitState(SC_HANDLE svc, DWORD want, std::wstring& error) {
    for (int i = 0; i < 300; ++i) {
        SERVICE_STATUS st{};
        if (!QueryServiceStatus(svc, &st)) {
            error = winError(GetLastError());
            return false;
        }
        if (st.dwCurrentState == want) return true;
        if (want == SERVICE_RUNNING && st.dwCurrentState == SERVICE_STOPPED) {
            error = L"служба остановилась при запуске: см. журнал службы и журнал событий «Приложение»";
            return false;
        }
        Sleep(100);
    }
    error = L"служба не ответила за 30 секунд";
    return false;
}

std::wstring moduleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleDir), &self);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
}

}  // namespace

bool controlService(bool stop, bool start, std::wstring& error) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = winError(GetLastError());
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"Emulkon", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    bool ok = svc != nullptr;
    if (!ok) error = L"служба Emulkon не открывается: " + winError(GetLastError());
    if (ok && stop) {
        SERVICE_STATUS st{};
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED) {
            if (!ControlService(svc, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
                error = L"остановка: " + winError(GetLastError());
                ok = false;
            } else {
                ok = waitState(svc, SERVICE_STOPPED, error);
            }
        }
    }
    if (ok && start) {
        if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            error = L"запуск: " + winError(GetLastError());
            ok = false;
        } else {
            ok = waitState(svc, SERVICE_RUNNING, error);
        }
    }
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

std::string base64url(const unsigned char* data, size_t n) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out += abc[(v >> 18) & 63];
        out += abc[(v >> 12) & 63];
        if (i + 1 < n) out += abc[(v >> 6) & 63];
        if (i + 2 < n) out += abc[v & 63];
    }
    return out;
}

std::string sha256Hex(const std::string& s) {
    unsigned char sum[32] = {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    NTSTATUS st = BCryptHash(alg, nullptr, 0, (PUCHAR)s.data(), (ULONG)s.size(), sum, sizeof sum);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) return {};
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", sum[i]);
    return std::string(hex, 64);
}

std::string newToken() {
    unsigned char buf[32];
    if (BCryptGenRandom(nullptr, buf, sizeof buf, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    std::string t = "emk_" + base64url(buf, sizeof buf);
    SecureZeroMemory(buf, sizeof buf);
    return t;
}

bool writeTokenHash(const wchar_t* value, const std::string& hash, std::wstring& error) {
    HKEY key;
    LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Emulkon\\Server", 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (rc != ERROR_SUCCESS) {
        error = rc == ERROR_ACCESS_DENIED ? L"нужны права администратора: откройте консоль от имени администратора"
                                          : L"HKLM\\Software\\Emulkon\\Server: " + winError(rc);
        return false;
    }
    if (hash.empty()) {
        rc = RegDeleteValueW(key, value);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    } else {
        const std::wstring w = widen(hash);
        rc = RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()), DWORD((w.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) error = std::wstring(value) + L": " + winError(rc);
    return rc == ERROR_SUCCESS;
}

std::wstring installDir() {
    std::wstring dir = moduleDir();  // …\Emulkon\console
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

std::wstring machineDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    std::wstring base = n && n < MAX_PATH ? std::wstring(buf, n) : L"C:\\ProgramData";
    return base + L"\\Emulkon";
}

bool runModify(bool network, std::wstring& error) {
    const std::wstring exe = installDir() + L"\\bin\\EmulkonSetup.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"нет мастера " + exe + L" — восстановите Эмулкон";
        return false;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring log = std::wstring(tmp) + L"emulkon-console-modify.log";
    DeleteFileW(log.c_str());
    const std::wstring args = std::wstring(L"/modify ") + (network ? L"/network" : L"/nonetwork") + L" /quiet /log \"" + log + L"\"";
    // ShellExecuteEx: у мастера манифест requireAdministrator — из консоли без прав Windows спросит UAC.
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        error = L"мастер не запустился: " + winError(GetLastError());
        return false;
    }
    WaitForSingleObject(sei.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code == 0) return true;
    // Причина — строки «[FAIL]» отчёта мастера.
    std::ifstream in(log.c_str(), std::ios::binary);
    std::string line, fails;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == 0x0D) line.pop_back();
        if (line.find("[FAIL]") != std::string::npos) fails += (fails.empty() ? "" : "; ") + line.substr(line.find("]") + 2);
    }
    error = L"мастер «Изменить» завершился с кодом " + std::to_wstring(code) +
            (fails.empty() ? L", отчёт — " + log : L": " + widen(fails));
    return false;
}

}  // namespace console
