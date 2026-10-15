// Чтение: реестр, состояние службы, запросы к серверу.
#include <windows.h>

#include <mutex>

#include "http.h"
#include "inventory.h"
#include "summary.h"

namespace console {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

namespace {

std::mutex g_tokenMu;
std::string g_token;  // токен управления консоли: только в памяти

// readValue — строка или DWORD из ключа; DWORD — десятичной строкой.
bool readValue(HKEY key, const wchar_t* name, std::wstring& out) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
    if (type == REG_DWORD && size == sizeof(DWORD)) {
        DWORD v = 0;
        RegQueryValueExW(key, name, nullptr, nullptr, (BYTE*)&v, &size);
        out = std::to_wstring(v);
        return true;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr, (BYTE*)&s[0], &size) != ERROR_SUCCESS) return false;
    s.resize(wcsnlen(s.c_str(), s.size()));
    out = s;
    return true;
}

// readKey — значения из ключа; уже прочитанные (из политики) не перекрывает. Есть ли что-то — в any.
bool readKey(const wchar_t* path, ServerSettings& s, bool& any) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
    std::wstring v;
    any = false;
    if (s.addr.empty() && readValue(key, L"Addr", v) && !v.empty()) s.addr = v, any = true;
    if (s.network.empty() && readValue(key, L"Network", v)) s.network = v, any = true;
    if (!s.controlToken && readValue(key, L"ControlTokenHash", v) && !v.empty()) s.controlToken = any = true;
    if (!s.deviceToken && readValue(key, L"DeviceTokenHash", v) && !v.empty()) s.deviceToken = any = true;
    RegCloseKey(key);
    return true;
}

Reply get(emu::HttpClient& http, const std::wstring& base, const char* path, const emu::Headers& headers) {
    emu::HttpResponse r = http.get(narrow(base) + path, headers, 2000);
    return {r.status, r.body, r.error};
}

std::wstring startTypeText(DWORD t) {
    switch (t) {
        case SERVICE_AUTO_START: return L"автоматически";
        case SERVICE_DEMAND_START: return L"вручную";
        case SERVICE_DISABLED: return L"отключена";
        default: return L"особый (" + std::to_wstring(t) + L")";
    }
}

Service stateOf(DWORD s) {
    switch (s) {
        case SERVICE_STOPPED: return Service::Stopped;
        case SERVICE_START_PENDING: return Service::Starting;
        case SERVICE_RUNNING: return Service::Running;
        case SERVICE_STOP_PENDING: return Service::Stopping;
        default: return Service::Unknown;
    }
}

}  // namespace

std::wstring serverBase(const ServerSettings& s) {
    wchar_t buf[512];
    DWORD n = GetEnvironmentVariableW(L"EMULKON_CONSOLE_URL", buf, 512);
    if (n > 0 && n < 512) {
        std::wstring url(buf, n);
        while (!url.empty() && url.back() == L'/') url.pop_back();
        return url;
    }
    return localUrl(listenAddr(s));
}

void setConsoleToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_tokenMu);
    g_token = token;
}

std::string consoleToken() {
    std::lock_guard<std::mutex> lock(g_tokenMu);
    return g_token;
}

ServerSettings readServerSettings() {
    ServerSettings s;
    bool any = false;
    if (readKey(L"SOFTWARE\\Policies\\Emulkon", s, any)) s.policy = any;
    s.readable = readKey(L"SOFTWARE\\Emulkon\\Server", s, any);
    return s;
}

Service queryService() { return queryServiceInfo().state; }

ServiceInfo queryServiceInfo() {
    ServiceInfo info;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return info;
    SC_HANDLE svc = OpenServiceW(scm, L"Emulkon", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc && GetLastError() == ERROR_ACCESS_DENIED) svc = OpenServiceW(scm, L"Emulkon", SERVICE_QUERY_STATUS);
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) info.state = Service::Missing;
        CloseServiceHandle(scm);
        return info;
    }
    SERVICE_STATUS st{};
    if (QueryServiceStatus(svc, &st)) info.state = stateOf(st.dwCurrentState);
    DWORD need = 0;
    QueryServiceConfigW(svc, nullptr, 0, &need);
    if (need) {
        std::vector<BYTE> buf(need);
        auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
        if (QueryServiceConfigW(svc, cfg, need, &need)) {
            info.configKnown = true;
            info.startType = startTypeText(cfg->dwStartType);
            info.account = cfg->lpServiceStartName ? cfg->lpServiceStartName : L"";
            info.command = cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L"";
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return info;
}

Reply apiRequest(const char* method, const std::string& path, const std::string& body) {
    const std::string base = narrow(serverBase(readServerSettings()));
    const std::string token = consoleToken();
    emu::Headers headers;
    if (!token.empty()) headers.push_back({"Authorization", "Bearer " + token});
    auto http = emu::makeHttpClient("EmulkonConsole/" EMULKON_VERSION);
    const std::string url = base + path;
    const std::string m = method;
    emu::HttpResponse r = m == "POST"     ? http->post(url, body, headers, 5000)
                          : m == "PATCH"  ? http->patch(url, body, headers, 5000)
                          : m == "PUT"    ? http->put(url, body, headers, 5000)
                          : m == "DELETE" ? http->remove(url, headers, 5000)
                                          : http->get(url, headers, 5000);
    return {r.status, r.body, r.error};
}

Inputs collect() {
    Inputs in;
    in.service = queryService();
    in.settings = readServerSettings();
    const std::string token = consoleToken();
    in.consoleToken = !token.empty();
    emu::Headers headers;
    if (!token.empty()) headers.push_back({"Authorization", "Bearer " + token});
    const std::wstring base = serverBase(in.settings);
    auto http = emu::makeHttpClient("EmulkonConsole/" EMULKON_VERSION);
    in.info = get(*http, base, "/control/v1/info", headers);
    if (in.info.status == 0) {
        // Сервер не отвечает — остальные запросы ждали бы так же.
        in.devices = in.workstations = in.info;
        return in;
    }
    in.devices = get(*http, base, "/control/v1/devices", headers);
    in.workstations = get(*http, base, "/control/v1/workstations", headers);
    return in;
}

}  // namespace console
