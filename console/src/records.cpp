#include "records.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <mutex>

#include "http.h"
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

// scalar — значение одной строкой: строка как есть, остальное — JSON.
std::wstring scalar(const emu::Json& v) { return v.isString() ? widen(v.text) : widen(v.dump()); }

std::wstring join(const std::vector<std::wstring>& v, const wchar_t* sep) {
    std::wstring s;
    for (const auto& x : v) s += (s.empty() ? L"" : sep) + x;
    return s;
}

std::wstring condition(const emu::Json* m) {
    std::vector<std::wstring> parts;
    if (m && m->isObject()) {
        if (auto v = str(*m, "device"); !v.empty()) parts.push_back(L"устройство " + v);
        if (auto v = str(*m, "type"); !v.empty()) parts.push_back(L"класс " + v);
        if (auto v = str(*m, "workstation"); !v.empty()) parts.push_back(L"место " + v);
        if (auto v = str(*m, "method"); !v.empty()) parts.push_back(L"метод " + v);
        if (const emu::Json* p = m->get("param"); p && p->isObject()) {
            std::wstring s = L"параметр №" + std::to_wstring(num(*p, "index"));
            if (auto v = str(*p, "equals"); !v.empty()) s += L" = «" + v + L"»";
            if (auto v = str(*p, "contains"); !v.empty()) s += L" содержит «" + v + L"»";
            parts.push_back(s);
        }
        if (long long n = num(*m, "nth_call")) parts.push_back(L"на " + std::to_wstring(n) + L"-м вызове");
        if (long long n = num(*m, "every")) parts.push_back(L"каждый " + std::to_wstring(n) + L"-й");
        if (const emu::Json* p = m->get("probability"); p && p->isNumber() && p->asDouble() > 0) {
            wchar_t buf[32];
            swprintf(buf, 32, L"вероятность %g%%", p->asDouble() * 100);
            parts.push_back(buf);
        }
    }
    return parts.empty() ? L"любой вызов" : join(parts, L", ");
}

std::wstring action(const emu::Json* a) {
    if (!a || !a->isObject()) return L"—";
    const std::string type = raw(*a, "type");
    const std::wstring delay = std::to_wstring(num(*a, "delay_ms")) + L" мс";
    if (type == "error") return L"ошибка " + std::to_wstring(num(*a, "error_code")) + L": " + str(*a, "error_text");
    if (type == "delay") return L"задержка " + delay;
    if (type == "drop") return L"выполнить и оборвать соединение";
    if (type == "late") return L"ответить после таймаута компоненты (" + delay + L")";
    if (type == "set_state" || type == "emit_event") {
        std::vector<std::wstring> kv;
        const emu::Json* s = a->get(type == "set_state" ? "state" : "event");
        if (s && s->isObject())
            for (const auto& f : s->fields) kv.push_back(widen(f.first) + L"=" + scalar(f.second));
        return (type == "set_state" ? L"состояние: " : L"событие: ") + join(kv, L", ");
    }
    return widen(type);
}

// pretty — JSON с отступами; строки — через dump (экранирование, UTF-8 как есть).
void pretty(const emu::Json& v, int depth, std::string& out) {
    const std::string pad((depth + 1) * 2, ' '), end(depth * 2, ' ');
    if (v.isObject() && !v.fields.empty()) {
        out += "{\n";
        for (size_t i = 0; i < v.fields.size(); ++i) {
            out += pad + emu::Json::string(v.fields[i].first).dump() + ": ";
            pretty(v.fields[i].second, depth + 1, out);
            out += i + 1 < v.fields.size() ? ",\n" : "\n";
        }
        out += end + "}";
    } else if (v.isArray() && !v.items.empty()) {
        out += "[\n";
        for (size_t i = 0; i < v.items.size(); ++i) {
            out += pad;
            pretty(v.items[i], depth + 1, out);
            out += i + 1 < v.items.size() ? ",\n" : "\n";
        }
        out += end + "]";
    } else {
        out += v.dump();
    }
}

void removeField(emu::Json& o, const char* key) {
    for (auto it = o.fields.begin(); it != o.fields.end();)
        it = it->first == key ? o.fields.erase(it) : it + 1;
}

std::wstring directionText(const std::string& d) {
    if (d == "in") return L"команда";
    if (d == "out") return L"ответ";
    if (d == "event") return L"событие";
    if (d == "system") return L"служебное";
    return widen(d);
}

std::mutex g_filterMu;
std::wstring g_filter;

}  // namespace

// --- сценарии ---

bool parseScenarios(const std::string& body, std::vector<Scenario>& out, std::wstring& err) {
    emu::Json doc;
    std::string e;
    if (!emu::parseJson(body, doc, e) || !doc.isObject() || !doc.get("rules") || !doc.get("rules")->isArray()) {
        err = L"непонятный ответ сервера";
        return false;
    }
    out.clear();
    for (const emu::Json& o : doc.get("rules")->items) {
        if (!o.isObject()) continue;
        Scenario s;
        s.id = raw(o, "id");
        s.name = str(o, "name");
        const emu::Json* en = o.get("enabled");
        s.enabled = en && en->isBool() && en->b;
        s.limit = (int)num(o, "limit");
        s.fired = (int)num(o, "fired");
        s.seen = (int)num(o, "seen");
        s.condition = condition(o.get("match"));
        s.action = action(o.get("action"));
        s.raw = o.dump();
        out.push_back(std::move(s));
    }
    return true;
}

Table scenariosTable(const std::vector<Scenario>& list) {
    Table t;
    t.columns = {{L"Правило", 220}, {L"Код", 110}, {L"Включено", 70}, {L"Условие", 280},
                 {L"Действие", 240}, {L"Ограничение", 90}, {L"Сработало", 110}};
    for (const Scenario& s : list) {
        t.cells.push_back({s.name.empty() ? widen(s.id) : s.name, widen(s.id), s.enabled ? L"да" : L"нет", s.condition, s.action,
                           s.limit ? std::to_wstring(s.limit) + L" раз" : L"—",
                           std::to_wstring(s.fired) + L" из " + std::to_wstring(s.seen)});
        t.keys.push_back(s.id);
    }
    return t;
}

std::string ruleForPut(const std::string& rawRule, bool enabled) {
    emu::Json o;
    std::string e;
    if (!emu::parseJson(rawRule, o, e) || !o.isObject()) return {};
    o.set("enabled", emu::Json::boolean(enabled));
    removeField(o, "fired");
    removeField(o, "seen");
    return o.dump();
}

std::string scenariosFile(const std::vector<Scenario>& list) {
    emu::Json doc = emu::Json::object();
    emu::Json rules = emu::Json::array();
    for (const Scenario& s : list) {
        emu::Json o;
        std::string e;
        if (!emu::parseJson(s.raw, o, e) || !o.isObject()) continue;
        removeField(o, "fired");
        removeField(o, "seen");
        rules.push(std::move(o));
    }
    doc.set("rules", std::move(rules));
    std::string out;
    pretty(doc, 0, out);
    return out + "\n";
}

ImportResult importScenarios(const std::string& text, const std::vector<Scenario>& current) {
    ImportResult r;
    emu::Json doc;
    std::string e;
    std::string body = text;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
        body.erase(0, 3);  // BOM
    if (!emu::parseJson(body, doc, e)) {
        r.errors.push_back(L"файл — не JSON: " + widen(e));
        return r;
    }
    const emu::Json* rules = doc.isArray() ? &doc : doc.isObject() ? doc.get("rules") : nullptr;
    if (!rules || !rules->isArray()) {
        r.errors.push_back(L"в файле нет списка правил: ждём {\"rules\": […]} или […]");
        return r;
    }
    for (const emu::Json& rule : rules->items) {
        if (!rule.isObject()) {
            r.errors.push_back(L"элемент списка — не объект");
            continue;
        }
        emu::Json o = rule;
        removeField(o, "fired");
        removeField(o, "seen");
        const std::string id = raw(o, "id");
        bool known = false;
        for (const Scenario& s : current) known = known || (!id.empty() && s.id == id);
        Reply rep = known ? apiPutScenario(id, o.dump()) : apiCreateScenario(o.dump());
        if (rep.status >= 200 && rep.status < 300) {
            (known ? r.replaced : r.created)++;
        } else {
            std::wstring name = str(o, "name");
            r.errors.push_back(L"правило " + (id.empty() ? (name.empty() ? L"без кода" : name) : widen(id)) + L": " + replyError(rep));
        }
    }
    return r;
}

Reply apiCreateScenario(const std::string& rule) { return apiRequest("POST", "/control/v1/scenarios", rule); }
Reply apiPutScenario(const std::string& id, const std::string& rule) {
    return apiRequest("PUT", "/control/v1/scenarios/" + urlPart(id), rule);
}
Reply apiDeleteScenario(const std::string& id) { return apiRequest("DELETE", "/control/v1/scenarios/" + urlPart(id)); }

// --- журнал ---

std::string journalPath(const std::wstring& text, int limit, bool jsonl) {
    std::string p = "/control/v1/journal?" + std::string(jsonl ? "format=jsonl" : "limit=" + std::to_string(limit));
    if (!text.empty()) p += "&q=" + urlPart(narrow(text));
    return p;
}

bool parseJournal(const std::string& body, std::vector<JournalEntry>& out, bool& hasMore, std::wstring& err) {
    emu::Json doc;
    std::string e;
    if (!emu::parseJson(body, doc, e) || !doc.isObject() || !doc.get("entries") || !doc.get("entries")->isArray()) {
        err = L"непонятный ответ сервера";
        return false;
    }
    const emu::Json* more = doc.get("has_more");
    hasMore = more && more->isBool() && more->b;
    out.clear();
    for (const emu::Json& o : doc.get("entries")->items) {
        if (!o.isObject()) continue;
        JournalEntry j;
        j.seq = num(o, "seq");
        j.time = localTimeSeconds(raw(o, "ts"));
        j.direction = directionText(raw(o, "direction"));
        j.workstation = str(o, "workstation_id");
        j.device = str(o, "device_id");
        j.method = str(o, "method");
        if (const emu::Json* l = o.get("latency_ms"); l && l->isNumber()) j.latency = widen(l->text) + L" мс";
        std::vector<std::wstring> rules;
        if (const emu::Json* a = o.get("applied_rules"); a && a->isArray())
            for (const emu::Json& r : a->items) rules.push_back(scalar(r));
        j.rules = join(rules, L", ");
        if (const emu::Json* p = o.get("payload")) {
            std::wstring s = widen(p->dump());
            if (s.size() > 160) s = s.substr(0, 160) + L"…";
            j.payload = s;
        }
        j.raw = o.dump();
        out.push_back(std::move(j));
    }
    return true;
}

Table journalTable(const std::vector<JournalEntry>& list) {
    Table t;
    t.columns = {{L"№", 70},     {L"Время", 140},        {L"Направление", 90}, {L"Рабочее место", 130},
                 {L"Устройство", 110}, {L"Метод", 170}, {L"Задержка", 70},     {L"Правила", 90}, {L"Данные", 420}};
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const JournalEntry& j = *it;
        t.cells.push_back({std::to_wstring(j.seq), j.time, j.direction, j.workstation, j.device, j.method, j.latency, j.rules, j.payload});
        t.keys.push_back(std::to_string(j.seq));
    }
    return t;
}

bool exportJournal(const std::wstring& path, const std::wstring& text, long long& lines, std::wstring& err) {
    lines = 0;
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file) {
        err = L"не открыть файл " + path;
        return false;
    }
    emu::Headers headers;
    const std::string token = consoleToken();
    if (!token.empty()) headers.push_back({"Authorization", "Bearer " + token});
    auto http = emu::makeHttpClient("EmulkonConsole/" EMULKON_VERSION);
    emu::StreamCancel cancel;
    bool writeFailed = false;
    emu::HttpResponse r = http->stream(narrow(serverBase(readServerSettings())) + journalPath(text, 0, true), headers, 30000,
                                       cancel, [&](const std::string& chunk) {
                                           file.write(chunk.data(), (std::streamsize)chunk.size());
                                           for (char c : chunk) lines += c == '\n';
                                           writeFailed = !file;
                                           return !writeFailed;
                                       });
    file.close();
    if (writeFailed) {
        err = L"не записать файл " + path;
        return false;
    }
    if (r.status != 200) {
        err = replyError({r.status, r.body, r.error});
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

void setJournalFilter(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(g_filterMu);
    g_filter = text;
}

std::wstring journalFilter() {
    std::lock_guard<std::mutex> lock(g_filterMu);
    return g_filter;
}

std::wstring prettyJson(const std::string& rawText) {
    emu::Json v;
    std::string e;
    if (!emu::parseJson(rawText, v, e)) return widen(rawText);
    std::string out;
    pretty(v, 0, out);
    // Для многострочного поля Windows — CRLF.
    std::wstring w = widen(out), crlf;
    for (wchar_t c : w) {
        if (c == L'\n') crlf += L'\r';
        crlf += c;
    }
    return crlf;
}

}  // namespace console
