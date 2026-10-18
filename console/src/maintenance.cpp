// Обслуживание: обновления из релизов GitHub и выгрузка и загрузка настроек службы (maintenance.h).
#include "maintenance.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cctype>
#include <cstdio>

#include "http.h"
#include "json.h"
#include "summary.h"

namespace console {

namespace {

const char kFeed[] = "https://github.com/iMironRU/emulkon/releases/latest/download/latest.json";

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool hasControl(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

// Версия: X.Y.Z и необязательная предварительная часть после «-» (буквы, цифры, точки).
bool splitVersion(const std::string& v, unsigned long long core[3], std::string& pre) {
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        if (pos >= v.size() || !std::isdigit(static_cast<unsigned char>(v[pos]))) return false;
        unsigned long long n = 0;
        size_t start = pos;
        while (pos < v.size() && std::isdigit(static_cast<unsigned char>(v[pos]))) {
            n = n * 10 + static_cast<unsigned>(v[pos] - '0');
            if (pos - start > 9) return false;
            ++pos;
        }
        core[i] = n;
        if (i < 2) {
            if (pos >= v.size() || v[pos] != '.') return false;
            ++pos;
        }
    }
    pre.clear();
    if (pos == v.size()) return true;
    if (v[pos] != '-' || pos + 1 == v.size()) return false;
    pre = v.substr(pos + 1);
    for (char c : pre)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.') return false;
    return true;
}

const emu::Json* field(const emu::Json& o, const char* name, emu::Json::Type type) {
    const emu::Json* v = o.get(name);
    return v && v->type == type ? v : nullptr;
}

bool envSet(const wchar_t* name, std::wstring* value = nullptr) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    if (n == 0 || n >= 1024) return false;
    if (value) value->assign(buf, n);
    return true;
}

std::wstring lastError(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"ошибка " + std::to_wstring(code);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

const wchar_t kServerKey[] = L"SOFTWARE\\Emulkon\\Server";

}  // namespace

// --- Обновления ---

bool parseRelease(const std::string& json, bool allowHttp, Release& r, std::wstring& err) {
    emu::Json doc;
    std::string perr;
    if (!emu::parseJson(json, doc, perr) || !doc.isObject()) {
        err = L"описание выпуска — не JSON: " + widen(perr);
        return false;
    }
    const emu::Json* version = field(doc, "version", emu::Json::Type::String);
    const emu::Json* url = field(doc, "url", emu::Json::Type::String);
    const emu::Json* sha = field(doc, "sha256", emu::Json::Type::String);
    if (!version || !url || !sha) {
        err = L"в описании выпуска нет version, url или sha256";
        return false;
    }
    unsigned long long core[3];
    std::string pre;
    if (!splitVersion(version->text, core, pre)) {
        err = L"версия выпуска не по образцу X.Y.Z: " + widen(version->text);
        return false;
    }
    const std::string& u = url->text;
    const bool https = u.rfind("https://", 0) == 0, http = u.rfind("http://", 0) == 0;
    if (!https && !(allowHttp && http)) {
        err = L"адрес установщика не https: " + widen(u);
        return false;
    }
    const std::string file = "EmulkonSetup_" + version->text + ".exe";
    if (u.size() < file.size() + 1 || u.compare(u.size() - file.size() - 1, std::string::npos, "/" + file) != 0) {
        err = L"адрес установщика не ведёт на " + widen(file);
        return false;
    }
    if (!isHex64(sha->text)) {
        err = L"sha256 в описании выпуска — не 64 шестнадцатеричных символа";
        return false;
    }
    r = Release{};
    r.version = version->text;
    r.url = u;
    r.sha256 = lower(sha->text);
    if (const emu::Json* p = field(doc, "published", emu::Json::Type::String)) r.published = p->text;
    if (const emu::Json* n = field(doc, "notes", emu::Json::Type::String)) r.notes = n->text;
    if (const emu::Json* s = doc.get("size"); s && s->isInteger() && s->asInt() > 0) r.size = s->asInt();
    return true;
}

int compareVersions(const std::string& a, const std::string& b) {
    unsigned long long ca[3], cb[3];
    std::string pa, pb;
    const bool oka = splitVersion(a, ca, pa), okb = splitVersion(b, cb, pb);
    if (!oka || !okb) return oka == okb ? a.compare(b) : (oka ? 1 : -1);  // неразборчивая — младше
    for (int i = 0; i < 3; ++i)
        if (ca[i] != cb[i]) return ca[i] < cb[i] ? -1 : 1;
    if (pa == pb) return 0;
    if (pa.empty()) return 1;
    if (pb.empty()) return -1;
    return pa < pb ? -1 : 1;
}

std::wstring releasePage(const Release& r) {
    const std::string marker = "/releases/download/";
    const size_t i = r.url.find(marker);
    if (r.url.rfind("https://", 0) != 0 || i == std::string::npos) return {};
    return widen(r.url.substr(0, i) + "/releases/tag/v" + r.version);
}

std::wstring updateFeedUrl(bool* overridden) {
    std::wstring url;
    const bool o = envSet(L"EMULKON_UPDATE_URL", &url);
    if (overridden) *overridden = o;
    return o ? url : widen(kFeed);
}

bool fetchRelease(Release& r, std::wstring& err) {
    bool overridden = false;
    const std::wstring feed = updateFeedUrl(&overridden);
    auto http = emu::makeHttpClient("EmulkonConsole/" EMULKON_VERSION);
    emu::HttpResponse resp = http->get(narrow(feed), {}, 15000);
    if (resp.status == 0) {
        err = L"нет связи с " + feed + L": " + widen(resp.error);
        return false;
    }
    if (resp.status == 404) {
        err = L"описания выпуска нет (" + feed + L"): выпуски до 0.9.0 его не публиковали";
        return false;
    }
    if (resp.status != 200) {
        err = L"HTTP " + std::to_wstring(resp.status) + L" от " + feed;
        return false;
    }
    return parseRelease(resp.body, overridden, r, err);
}

bool downloadRelease(const Release& r, const std::wstring& dir, std::wstring& path, std::wstring& err) {
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(dir.substr(0, slash).c_str(), nullptr);
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        err = L"каталог " + dir + L": " + lastError(GetLastError());
        return false;
    }
    auto http = emu::makeHttpClient("EmulkonConsole/" EMULKON_VERSION);
    emu::HttpResponse resp = http->get(r.url, {}, 300000);
    if (resp.status != 200) {
        err = resp.status == 0 ? L"не скачать " + widen(r.url) + L": " + widen(resp.error)
                               : L"HTTP " + std::to_wstring(resp.status) + L" от " + widen(r.url);
        return false;
    }
    if (r.size > 0 && static_cast<long long>(resp.body.size()) != r.size) {
        err = L"размер установщика " + std::to_wstring(resp.body.size()) + L" байт, в описании выпуска — " +
              std::to_wstring(r.size);
        return false;
    }
    if (sha256Hex(resp.body) != r.sha256) {
        err = L"контрольная сумма SHA-256 установщика не совпала с описанием выпуска — файл не сохранён";
        return false;
    }
    path = dir + L"\\EmulkonSetup_" + widen(r.version) + L".exe";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = path + L": " + lastError(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(f, resp.body.data(), static_cast<DWORD>(resp.body.size()), &written, nullptr) &&
                    written == resp.body.size();
    const DWORD code = GetLastError();
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(path.c_str());
        err = path + L": " + lastError(code);
        return false;
    }
    // Метка «из Интернета» (Zone.Identifier), как у файла из браузера: Windows проверяет его при запуске
    // (SmartScreen, антивирус) как обычную загрузку, а не как exe, тихо появившийся на диске.
    const std::string zone = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=" + r.url + "\r\n";
    HANDLE z = CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (z != INVALID_HANDLE_VALUE) {
        WriteFile(z, zone.data(), static_cast<DWORD>(zone.size()), &written, nullptr);
        CloseHandle(z);
    }
    return true;
}

std::wstring userDownloadsDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir;
}

bool runInstaller(const std::wstring& path, std::wstring& err) {
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        err = path + L": " + lastError(GetLastError());
        return false;
    }
    return true;
}

// --- Настройки службы ---

const std::vector<std::wstring>& settingNames() {
    static const std::vector<std::wstring> names = {L"Addr",     L"Network",          L"DataDir",         L"DownloadsDir",
                                                    L"LogLevel", L"ControlTokenHash", L"DeviceTokenHash", L"UpdateMode",
                                                    L"UpdateURL"};
    return names;
}

std::string settingsFile(const std::vector<Setting>& values, const std::string& computer, const std::string& exported) {
    emu::Json doc = emu::Json::object();
    doc.set("format", emu::Json::string("emulkon-settings"));
    doc.set("version", emu::Json::number(int64_t{1}));
    doc.set("product_version", emu::Json::string(EMULKON_VERSION));
    doc.set("computer", emu::Json::string(computer));
    doc.set("exported", emu::Json::string(exported));
    emu::Json server = emu::Json::object();
    for (const std::wstring& name : settingNames())
        for (const Setting& s : values)
            if (s.name == name)
                server.set(narrow(name), s.dword ? emu::Json::number(static_cast<int64_t>(s.number))
                                                 : emu::Json::string(narrow(s.text)));
    doc.set("server", server);
    return doc.dump() + "\n";
}

bool parseSettingsFile(const std::string& text, std::vector<Setting>& out, std::vector<std::wstring>& warnings,
                       std::wstring& err) {
    emu::Json doc;
    std::string perr;
    std::string body = text.rfind("\xEF\xBB\xBF", 0) == 0 ? text.substr(3) : text;
    if (!emu::parseJson(body, doc, perr) || !doc.isObject()) {
        err = L"файл — не JSON: " + widen(perr);
        return false;
    }
    const emu::Json* format = field(doc, "format", emu::Json::Type::String);
    const emu::Json* version = doc.get("version");
    const emu::Json* server = field(doc, "server", emu::Json::Type::Object);
    if (!format || format->text != "emulkon-settings" || !server) {
        err = L"это не выгрузка настроек Эмулкона (нет \"format\": \"emulkon-settings\" и \"server\")";
        return false;
    }
    if (!version || !version->isInteger() || version->asInt() != 1) {
        err = L"версия формата выгрузки не 1 — файл от более новой консоли";
        return false;
    }
    out.clear();
    warnings.clear();
    for (const auto& kv : server->fields) {
        const std::wstring name = widen(kv.first);
        const emu::Json& v = kv.second;
        bool known = false;
        for (const std::wstring& n : settingNames()) known = known || n == name;
        if (!known) {
            warnings.push_back(L"пропущено незнакомое значение " + name);
            continue;
        }
        Setting s;
        s.name = name;
        if (name == L"Network") {
            if (!v.isInteger() || (v.asInt() != 0 && v.asInt() != 1)) {
                err = L"Network — 0 или 1";
                return false;
            }
            s.dword = true;
            s.number = static_cast<unsigned long>(v.asInt());
            out.push_back(s);
            continue;
        }
        if (!v.isString() || hasControl(v.text)) {
            err = name + L" — строка без управляющих символов";
            return false;
        }
        if (name == L"ControlTokenHash" || name == L"DeviceTokenHash") {
            if (!v.text.empty() && !isHex64(v.text)) {
                err = name + L" — SHA-256 токена: 64 шестнадцатеричных символа";
                return false;
            }
            s.text = widen(lower(v.text));
        } else if (name == L"LogLevel") {
            const std::string l = lower(v.text);
            if (l != "debug" && l != "info" && l != "warn" && l != "error") {
                err = L"LogLevel — debug, info, warn или error";
                return false;
            }
            s.text = widen(l);
        } else if (name == L"UpdateMode") {
            const std::string m = lower(v.text);
            if (m != "off" && m != "notify") {
                err = L"UpdateMode — off или notify";
                return false;
            }
            s.text = widen(m);
        } else if (name == L"UpdateURL") {
            if (!v.text.empty() && v.text.rfind("https://", 0) != 0 && v.text.rfind("http://", 0) != 0) {
                err = L"UpdateURL — адрес latest.json (http:// или https://)";
                return false;
            }
            s.text = widen(v.text);
        } else if (name == L"Addr") {
            if (v.text.find(':') == std::string::npos || v.text.find(' ') != std::string::npos) {
                err = L"Addr — адрес вида 127.0.0.1:8737";
                return false;
            }
            s.text = widen(v.text);
        } else {
            s.text = widen(v.text);
        }
        out.push_back(s);
    }
    return true;
}

std::wstring settingsSummary(const std::vector<Setting>& values) {
    std::wstring s;
    for (const std::wstring& name : settingNames()) {
        const Setting* v = nullptr;
        for (const Setting& x : values)
            if (x.name == name) v = &x;
        std::wstring text;
        if (!v)
            text = L"по умолчанию";
        else if (v->dword)
            text = name == L"Network" ? (v->number ? L"1 (кассы в сети)" : L"0 (только этот компьютер)") : std::to_wstring(v->number);
        else if (name.size() > 9 && name.compare(name.size() - 9, 9, L"TokenHash") == 0)
            text = v->text.empty() ? L"не задан" : L"задан (SHA-256)";
        else
            text = v->text.empty() ? L"(пусто)" : v->text;
        s += name + L" = " + text + L"\n";
    }
    return s;
}

bool readSettings(std::vector<Setting>& out, std::wstring& err) {
    HKEY key;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServerKey, 0, KEY_READ | KEY_WOW64_64KEY, &key);
    out.clear();
    if (rc == ERROR_FILE_NOT_FOUND) return true;  // ключа нет — всё по умолчанию
    if (rc != ERROR_SUCCESS) {
        err = L"HKLM\\" + std::wstring(kServerKey) + L": " + lastError(static_cast<DWORD>(rc));
        return false;
    }
    for (const std::wstring& name : settingNames()) {
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS) continue;
        Setting s;
        s.name = name;
        if (type == REG_DWORD && size == sizeof(DWORD)) {
            DWORD v = 0;
            RegQueryValueExW(key, name.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size);
            s.dword = true;
            s.number = v;
        } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
            std::wstring v(size / sizeof(wchar_t) + 1, L'\0');
            RegQueryValueExW(key, name.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&v[0]), &size);
            v.resize(wcsnlen(v.c_str(), v.size()));
            s.text = v;
        } else {
            continue;
        }
        out.push_back(s);
    }
    RegCloseKey(key);
    return true;
}

bool applySettings(const std::vector<Setting>& values, std::wstring& err) {
    std::vector<Setting> current;
    if (!readSettings(current, err)) return false;
    auto find = [](const std::vector<Setting>& list, const std::wstring& name) -> const Setting* {
        for (const Setting& s : list)
            if (s.name == name) return &s;
        return nullptr;
    };
    const Setting* curNet = find(current, L"Network");
    const Setting* newNet = find(values, L"Network");
    const bool netWas = curNet && curNet->number == 1, netWill = newNet && newNet->number == 1;

    HKEY key;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kServerKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                              nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        err = L"HKLM\\" + std::wstring(kServerKey) + L": " + lastError(static_cast<DWORD>(rc));
        return false;
    }
    for (const std::wstring& name : settingNames()) {
        if (name == L"Network" && netWas != netWill) continue;  // меняет мастер «Изменить» вместе с брандмауэром
        const Setting* v = find(values, name);
        if (!v) {
            rc = RegDeleteValueW(key, name.c_str());
            if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
        } else if (v->dword) {
            DWORD n = v->number;
            rc = RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&n), sizeof n);
        } else {
            rc = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(v->text.c_str()),
                                static_cast<DWORD>((v->text.size() + 1) * sizeof(wchar_t)));
        }
        if (rc != ERROR_SUCCESS) {
            RegCloseKey(key);
            err = name + L": " + lastError(static_cast<DWORD>(rc));
            return false;
        }
    }
    RegCloseKey(key);
    // Служба читает настройки при запуске.
    if (netWas != netWill) return runModify(netWill, err);
    const Service state = queryService();
    if (state == Service::Running || state == Service::Starting) return controlService(true, true, err);
    return true;
}

}  // namespace console
