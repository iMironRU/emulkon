// Служба Windows «Emulkon», журнал событий, брандмауэр и права (docs/local-install.md §2.2).
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <string>

#include "setup.h"

namespace setup {

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

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE v = nullptr) : h(v) {}
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    ~ScHandle() {
        if (h) CloseServiceHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

// Права на службу (SDDL): система и администраторы — как по умолчанию; пользователи (BU) — ещё запуск (RP)
// и остановка (WP), чтобы трей мог перезапустить сервер без UAC (§1.9); менять настройки — нет.
const wchar_t* const kServiceSddl =
    L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPWPLOCRRC;;;BU)"
    L"(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)";

// Данные службы: система и администраторы — полный доступ, LocalService (LS) — изменение, пользователи — чтение.
// Наследование от ProgramData отключено (P): там пользователи могут создавать файлы.
const wchar_t* const kDataSddl = L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;LS)(A;OICI;0x1200a9;;;BU)";

// Ключ настроек службы: система и администраторы — полный доступ, LocalService — чтение, пользователи — нет
// (там хеши токенов).
const wchar_t* const kServerKeySddl = L"D:PAI(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KR;;;LS)";

const wchar_t* const kEventLogKey = L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\Emulkon";
// Файл сообщений .NET (часть Windows): сообщение любого кода — «%1», свой файл сообщений не нужен.
const wchar_t* const kEventMessageFile = L"%SystemRoot%\\Microsoft.NET\\Framework64\\v4.0.30319\\EventLogMessages.dll";
const wchar_t* const kFirewallRule = L"Emulkon (TCP 8737)";

bool waitState(SC_HANDLE svc, DWORD want, DWORD ms) {
    for (DWORD waited = 0;; waited += 250) {
        SERVICE_STATUS st{};
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == want) return true;
        if (waited >= ms) return false;
        Sleep(250);
    }
}

// runHidden — консольная утилита без окна; код возврата.
DWORD runHidden(std::wstring cmd) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return GetLastError() | 0x80000000u;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

std::wstring system32(const wchar_t* exe) {
    wchar_t buf[MAX_PATH];
    UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return std::wstring(buf, n) + L"\\" + exe;
}

}  // namespace

ServiceState serviceState() {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) return ServiceState::Unknown;
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_QUERY_STATUS));
    if (!svc) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? ServiceState::Missing : ServiceState::Unknown;
    SERVICE_STATUS st{};
    if (!QueryServiceStatus(svc.h, &st)) return ServiceState::Unknown;
    switch (st.dwCurrentState) {
        case SERVICE_STOPPED: return ServiceState::Stopped;
        case SERVICE_START_PENDING: return ServiceState::Starting;
        case SERVICE_RUNNING: return ServiceState::Running;
        case SERVICE_STOP_PENDING: return ServiceState::Stopping;
    }
    return ServiceState::Unknown;
}

std::wstring serviceStateText(ServiceState s) {
    switch (s) {
        case ServiceState::Missing: return L"не установлена";
        case ServiceState::Stopped: return L"остановлена";
        case ServiceState::Starting: return L"запускается";
        case ServiceState::Running: return L"работает";
        case ServiceState::Stopping: return L"останавливается";
        case ServiceState::Unknown: break;
    }
    return L"состояние неизвестно";
}

bool installService(const std::wstring& exe, std::wstring& error) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm) {
        error = L"диспетчер служб: " + winError(GetLastError());
        return false;
    }
    const std::wstring cmd = L"\"" + exe + L"\" -service";
    const wchar_t* display = L"Эмулкон — сервер эмуляции оборудования";
    const wchar_t* account = L"NT AUTHORITY\\LocalService";
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_ALL_ACCESS));
    if (svc) {
        if (!ChangeServiceConfigW(svc.h, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(),
                                  nullptr, nullptr, nullptr, account, L"", display)) {
            error = L"перенастроить службу: " + winError(GetLastError());
            return false;
        }
    } else {
        svc.h = CreateServiceW(scm.h, kServiceName, display, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                               SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr, account, L"");
        if (!svc) {
            error = L"создать службу: " + winError(GetLastError());
            return false;
        }
    }
    std::wstring description =
        L"Эмулятор подключаемого оборудования для 1С:БПО: модели устройств, журнал обменов, веб-интерфейс на "
        L"http://127.0.0.1:8737. Только тестовые данные.";
    SERVICE_DESCRIPTIONW desc{&description[0]};
    ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Перезапуск при сбое: через 5 с, 10 с, затем раз в минуту; счётчик сбоев сбрасывается за сутки.
    SC_ACTION actions[] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 60000}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};  // и при выходе с ошибкой, а не только при падении
    ChangeServiceConfig2W(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kServiceSddl, SDDL_REVISION_1, &sd, nullptr) ||
        !SetServiceObjectSecurity(svc.h, DACL_SECURITY_INFORMATION, sd)) {
        error = L"права на службу: " + winError(GetLastError());
        if (sd) LocalFree(sd);
        return false;
    }
    LocalFree(sd);
    return true;
}

bool startService(std::wstring& error) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    ScHandle svc(scm ? OpenServiceW(scm.h, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS) : nullptr);
    if (!svc) {
        error = L"открыть службу: " + winError(GetLastError());
        return false;
    }
    if (!StartServiceW(svc.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        error = L"запустить службу: " + winError(GetLastError());
        return false;
    }
    if (!waitState(svc.h, SERVICE_RUNNING, 30000)) {
        SERVICE_STATUS st{};
        QueryServiceStatus(svc.h, &st);
        error = L"служба не перешла в «работает» за 30 с (состояние " + std::to_wstring(st.dwCurrentState) +
                L", код выхода " + std::to_wstring(st.dwServiceSpecificExitCode) +
                L"); причина — в журнале событий Windows (источник Emulkon) и в " + machineDataDir() + L"\\logs\\service.log";
        return false;
    }
    return true;
}

bool stopService(std::wstring& error) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        error = L"диспетчер служб: " + winError(GetLastError());
        return false;
    }
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        error = L"открыть службу: " + winError(GetLastError());
        return false;
    }
    SERVICE_STATUS st{};
    if (!ControlService(svc.h, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        error = L"остановить службу: " + winError(GetLastError());
        return false;
    }
    if (!waitState(svc.h, SERVICE_STOPPED, 30000)) {
        error = L"служба не остановилась за 30 с";
        return false;
    }
    return true;
}

bool deleteService(std::wstring& error) {
    if (!stopService(error)) return false;
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    ScHandle svc(scm ? OpenServiceW(scm.h, kServiceName, DELETE) : nullptr);
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        error = L"открыть службу: " + winError(GetLastError());
        return false;
    }
    if (!DeleteService(svc.h) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
        error = L"удалить службу: " + winError(GetLastError());
        return false;
    }
    return true;
}

bool registerEventSource(std::wstring& error) {
    HKEY key;
    LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kEventLogKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        error = winError(static_cast<DWORD>(rc));
        return false;
    }
    std::wstring file = kEventMessageFile;
    DWORD types = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
    bool ok = RegSetValueExW(key, L"EventMessageFile", 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(file.c_str()),
                             static_cast<DWORD>((file.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS &&
              RegSetValueExW(key, L"TypesSupported", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&types), sizeof types) ==
                  ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok) error = L"не записаны значения " + std::wstring(kEventLogKey);
    return ok;
}

void unregisterEventSource() { RegDeleteTreeW(HKEY_LOCAL_MACHINE, kEventLogKey); }

bool setFirewallRule(bool on, const std::wstring& exe, std::wstring& error) {
    const std::wstring netsh = L"\"" + system32(L"netsh.exe") + L"\" advfirewall firewall ";
    // Прежнее правило убираем всегда: путь к программе мог измениться. Нет правила — netsh вернёт 1, это не ошибка.
    runHidden(netsh + L"delete rule name=\"" + kFirewallRule + L"\"");
    if (!on) return true;
    DWORD code = runHidden(netsh + L"add rule name=\"" + kFirewallRule +
                           L"\" dir=in action=allow protocol=TCP localport=" + std::to_wstring(kPort) + L" program=\"" +
                           exe + L"\" profile=any description=\"Emulkon: кассы в сети\"");
    if (code != 0) {
        error = L"netsh вернул код " + std::to_wstring(code);
        return false;
    }
    return true;
}

bool secureMachineData(const std::wstring& dir, std::wstring& error) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kDataSddl, SDDL_REVISION_1, &sd, nullptr)) {
        error = winError(GetLastError());
        return false;
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    // SetNamedSecurityInfo раздаёт наследуемые записи и уже лежащим внутри файлам (перенесённым данным).
    std::wstring path = dir;
    DWORD rc = SetNamedSecurityInfoW(&path[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, dacl, nullptr);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) {
        error = winError(rc);
        return false;
    }
    return true;
}

bool secureServerKey(std::wstring& error) {
    HKEY key;
    LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kServerKey, 0, nullptr, 0, WRITE_DAC, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        error = winError(static_cast<DWORD>(rc));
        return false;
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(kServerKeySddl, SDDL_REVISION_1, &sd, nullptr) &&
              RegSetKeySecurity(key, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd) == ERROR_SUCCESS;
    if (!ok) error = winError(GetLastError());
    if (sd) LocalFree(sd);
    RegCloseKey(key);
    return ok;
}

// Элемент Панели управления: CLSID — постоянный (по нему Windows узнаёт элемент при обновлении).
namespace {
const wchar_t* const kCplClsid = L"{57A099B6-A55F-4338-89B9-64CF1BED2518}";

bool setString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value, DWORD type = REG_SZ) {
    HKEY key;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
                             static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}
}  // namespace

bool registerControlPanel(const std::wstring& icon, const std::wstring& command, std::wstring& error) {
    const std::wstring cls = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kCplClsid;
    const std::wstring ns = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ControlPanel\\NameSpace\\") + kCplClsid;
    // Категории Панели управления: 2 — «Оборудование и звук», 8 — «Программы».
    bool ok = setString(HKEY_LOCAL_MACHINE, cls, nullptr, L"Эмулкон") &&
              setString(HKEY_LOCAL_MACHINE, cls, L"LocalizedString", L"Эмулкон", REG_EXPAND_SZ) &&
              setString(HKEY_LOCAL_MACHINE, cls, L"InfoTip",
                        L"Эмулятор подключаемого оборудования для 1С:БПО: устройства, журнал, сценарии, токены", REG_EXPAND_SZ) &&
              setString(HKEY_LOCAL_MACHINE, cls, L"System.ApplicationName", L"Imiron.Emulkon") &&
              setString(HKEY_LOCAL_MACHINE, cls, L"System.ControlPanel.Category", L"2,8") &&
              setString(HKEY_LOCAL_MACHINE, cls + L"\\DefaultIcon", nullptr, icon) &&
              setString(HKEY_LOCAL_MACHINE, cls + L"\\Shell\\Open\\Command", nullptr, command, REG_EXPAND_SZ) &&
              setString(HKEY_LOCAL_MACHINE, ns, nullptr, L"Эмулкон");
    if (!ok) error = L"не записаны ключи " + cls;
    return ok;
}

void unregisterControlPanel() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                   (std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ControlPanel\\NameSpace\\") + kCplClsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kCplClsid).c_str());
}

}  // namespace setup
