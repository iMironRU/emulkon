#include "inventory.h"

#include <windows.h>

#include <cstdio>

#include "json.h"

namespace console {

namespace {

std::wstring str(const emu::Json& o, const char* key) {
    const emu::Json* v = o.get(key);
    return v && v->isString() ? widen(v->text) : std::wstring();
}

std::string raw(const emu::Json& o, const char* key) {
    const emu::Json* v = o.get(key);
    return v && v->isString() ? v->text : std::string();
}

long long num(const emu::Json& o, const char* key) {
    const emu::Json* v = o.get(key);
    return v && v->isNumber() ? (long long)v->asInt() : 0;
}

bool flag(const emu::Json& o, const char* key) {
    const emu::Json* v = o.get(key);
    return v && v->isBool() && v->b;
}

std::vector<std::wstring> strings(const emu::Json& o, const char* key) {
    std::vector<std::wstring> out;
    const emu::Json* a = o.get(key);
    if (a && a->isArray())
        for (const emu::Json& v : a->items)
            if (v.isString()) out.push_back(widen(v.text));
    return out;
}

// pairs — объект «ключ: значение» строками: строки как есть, остальное — JSON.
std::vector<Row> pairs(const emu::Json& o, const char* key) {
    std::vector<Row> out;
    const emu::Json* m = o.get(key);
    if (!m || !m->isObject()) return out;
    for (const auto& f : m->fields) out.push_back({widen(f.first), f.second.isString() ? widen(f.second.text) : widen(f.second.dump())});
    return out;
}

// list — массив key из ответа; err — почему его нет.
const emu::Json* list(const std::string& body, const char* key, emu::Json& doc, std::wstring& err) {
    std::string e;
    if (!emu::parseJson(body, doc, e) || !doc.isObject()) {
        err = L"непонятный ответ сервера";
        return nullptr;
    }
    const emu::Json* a = doc.get(key);
    if (!a || !a->isArray()) {
        err = L"непонятный ответ сервера";
        return nullptr;
    }
    return a;
}

}  // namespace

static std::wstring formatLocal(const std::string& t, bool seconds) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (t.size() < 19 || sscanf(t.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6 || y <= 1)
        return L"—";
    // Сдвиг пояса: Z или ±hh:mm после секунд (и долей).
    size_t i = 19;
    while (i < t.size() && (t[i] == '.' || isdigit((unsigned char)t[i]))) ++i;
    int offset = 0;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
        int oh = 0, om = 0;
        sscanf(t.c_str() + i + 1, "%2d:%2d", &oh, &om);
        offset = (t[i] == '+' ? 1 : -1) * (oh * 60 + om);
    }
    SYSTEMTIME st{(WORD)y, (WORD)mo, 0, (WORD)d, (WORD)h, (WORD)mi, (WORD)s, 0};
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return L"—";
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (long long)offset * 60 * 10000000LL;  // в UTC
    ft = {u.LowPart, u.HighPart};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"—";
    wchar_t buf[32];
    if (seconds)
        swprintf(buf, 32, L"%02u.%02u.%04u %02u:%02u:%02u", local.wDay, local.wMonth, local.wYear, local.wHour, local.wMinute,
                 local.wSecond);
    else
        swprintf(buf, 32, L"%02u.%02u.%04u %02u:%02u", local.wDay, local.wMonth, local.wYear, local.wHour, local.wMinute);
    return buf;
}

std::wstring localTime(const std::string& t) { return formatLocal(t, false); }
std::wstring localTimeSeconds(const std::string& t) { return formatLocal(t, true); }

bool parseWorkstations(const std::string& body, std::vector<Workstation>& out, std::wstring& err) {
    emu::Json doc;
    const emu::Json* a = list(body, "workstations", doc, err);
    if (!a) return false;
    out.clear();
    for (const emu::Json& o : a->items) {
        if (!o.isObject()) continue;
        Workstation w;
        w.id = raw(o, "workstation_id");
        w.alias = str(o, "alias");
        w.note = str(o, "note");
        w.firstSeen = localTime(raw(o, "first_seen"));
        w.lastSeen = localTime(raw(o, "last_seen"));
        w.infobases = strings(o, "infobases");
        w.devices = strings(o, "devices");
        w.printers = strings(o, "printers");
        w.defaultPrinter = str(o, "default_printer");
        w.sessions = (int)num(o, "sessions");
        if (const emu::Json* t = o.get("token"); t && t->isObject()) {
            w.token = true;
            w.tokenHint = str(*t, "hint");
            w.tokenIssued = localTime(raw(*t, "issued_at"));
        }
        out.push_back(std::move(w));
    }
    return true;
}

bool parseDevices(const std::string& body, std::vector<Device>& out, std::wstring& err) {
    emu::Json doc;
    const emu::Json* a = list(body, "devices", doc, err);
    if (!a) return false;
    out.clear();
    for (const emu::Json& o : a->items) {
        if (!o.isObject()) continue;
        Device d;
        d.id = raw(o, "device_id");
        d.type = raw(o, "type");
        d.name = str(o, "name");
        d.alias = str(o, "alias");
        d.note = str(o, "note");
        d.equipmentType = str(o, "equipment_type");
        d.workstation = str(o, "workstation_id");
        d.infobase = str(o, "infobase");
        d.instanceName = str(o, "instance_name");
        d.created = localTime(raw(o, "created_at"));
        d.lastSeen = localTime(raw(o, "last_seen"));
        d.commands = num(o, "commands");
        d.sessions = (int)num(o, "active_sessions");
        d.listeners = (int)num(o, "event_listeners");
        d.online = flag(o, "online");
        if (const emu::Json* e = o.get("last_error"); e && e->isObject()) {
            // Ошибка драйвера (docs/bpo-requirements/): код БПО и текст.
            d.lastError = L"код " + std::to_wstring(num(*e, "code")) + L": " + str(*e, "text");
        }
        d.state = pairs(o, "state");
        d.settings = pairs(o, "settings");
        out.push_back(std::move(d));
    }
    return true;
}

std::map<std::string, std::wstring> typeTitles(const std::string& infoBody) {
    std::map<std::string, std::wstring> out;
    emu::Json doc;
    std::string e;
    if (!emu::parseJson(infoBody, doc, e) || !doc.isObject()) return out;
    const emu::Json* a = doc.get("equipment_types");
    if (!a || !a->isArray()) return out;
    for (const emu::Json& t : a->items)
        if (t.isObject()) out[raw(t, "key")] = str(t, "title");
    return out;
}

std::wstring workstationTitle(const Workstation& w) { return w.alias.empty() ? widen(w.id) : w.alias; }
std::wstring deviceTitle(const Device& d) { return d.alias.empty() ? d.name : d.alias; }

Table rowsTable(const std::vector<Row>& rows) {
    Table t;
    t.columns = {{L"Параметр", 200}, {L"Значение", 460}};
    for (const Row& r : rows) {
        t.cells.push_back({r.name, r.value});
        t.keys.push_back({});
    }
    return t;
}

Table workstationsTable(const std::vector<Workstation>& list) {
    Table t;
    t.columns = {{L"Рабочее место", 170}, {L"Код", 150}, {L"Базы 1С", 260}, {L"Устройств", 70},
                 {L"Сессий", 60},         {L"Токен", 110}, {L"Последнее подключение", 140}};
    for (const Workstation& w : list) {
        std::wstring bases = w.infobases.empty() ? L"—" : w.infobases.front();
        if (w.infobases.size() > 1) bases += L" и ещё " + std::to_wstring(w.infobases.size() - 1);
        t.cells.push_back({workstationTitle(w), widen(w.id), bases, std::to_wstring(w.devices.size()), std::to_wstring(w.sessions),
                           w.token ? L"выдан …" + w.tokenHint : L"нет", w.lastSeen});
        t.keys.push_back(w.id);
    }
    return t;
}

Table devicesTable(const std::vector<Device>& list, const std::map<std::string, std::wstring>& titles) {
    Table t;
    t.columns = {{L"Устройство", 260}, {L"Код", 130}, {L"Класс", 170}, {L"Рабочее место", 150},
                 {L"На связи", 70},    {L"Команд", 70}, {L"Последняя активность", 140}};
    for (const Device& d : list) {
        auto it = titles.find(d.type);
        std::wstring cls = it != titles.end() && !it->second.empty() ? it->second : d.equipmentType;
        t.cells.push_back({deviceTitle(d), widen(d.id), cls, d.workstation, d.online ? L"да" : L"нет", std::to_wstring(d.commands),
                           d.lastSeen});
        t.keys.push_back(d.id);
    }
    return t;
}

std::wstring replyError(const Reply& r) {
    if (r.status == 0) return L"нет связи с сервером" + (r.error.empty() ? std::wstring() : L": " + widen(r.error));
    if (r.status == 401 || r.status == 403) return L"нужен токен управления: «Действие → Токен управления для консоли…»";
    emu::Json doc;
    std::string e;
    if (emu::parseJson(r.body, doc, e) && doc.isObject()) {
        if (const emu::Json* err = doc.get("error"); err && err->isObject()) {
            std::wstring msg = str(*err, "message");
            if (!msg.empty()) return msg;
        }
    }
    return L"ошибка сервера: HTTP " + std::to_wstring(r.status);
}

Reply apiIssueWorkstationToken(const std::string& id, std::string& token) {
    token.clear();
    Reply r = apiRequest("POST", "/control/v1/workstations/" + urlPart(id) + "/token");
    emu::Json doc;
    std::string e;
    if (r.status == 200 && emu::parseJson(r.body, doc, e) && doc.isObject())
        if (const emu::Json* t = doc.get("token"); t && t->isString()) token = t->text;
    return r;
}

Reply apiRevokeWorkstationToken(const std::string& id) {
    return apiRequest("DELETE", "/control/v1/workstations/" + urlPart(id) + "/token");
}

Reply apiSetWorkstationMeta(const std::string& id, const std::string& patch) {
    return apiRequest("PATCH", "/control/v1/workstations/" + urlPart(id), patch);
}

Reply apiSetDeviceMeta(const std::string& id, const std::string& patch) {
    return apiRequest("PATCH", "/control/v1/devices/" + urlPart(id), patch);
}

Reply apiResetDevice(const std::string& id) { return apiRequest("POST", "/control/v1/devices/" + urlPart(id) + "/settings/reset"); }

Reply apiDeleteDevice(const std::string& id) { return apiRequest("DELETE", "/control/v1/devices/" + urlPart(id)); }

std::wstring cleanText(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        const bool control = c < 0x20 || (c >= 0x7F && c < 0xA0);
        if (control) {
            if (!out.empty() && out.back() != L' ') out += L' ';
        } else {
            out += c;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(0, 1);
    return out;
}

std::string metaPatch(const std::wstring& alias0, const std::wstring& alias, const std::wstring& note0, const std::wstring& note) {
    std::string body;
    const std::wstring a = cleanText(alias), n = cleanText(note);
    if (a != alias0) body += "\"alias\":" + jsonString(a);
    if (n != note0) body += std::string(body.empty() ? "" : ",") + "\"note\":" + jsonString(n);
    return body.empty() ? body : "{" + body + "}";
}

std::string jsonString(const std::wstring& s) { return emu::Json::string(narrow(s)).dump(); }

std::string urlPart(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

}  // namespace console
