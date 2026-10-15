#include "doc.h"

#include <windows.h>

#include <cstdio>
#include <ctime>

#include "json.h"
#include "text.h"

namespace tray {

std::wstring widen(const std::string& utf8) {
    std::u16string s = emu::utf8to16(utf8);
    return std::wstring(s.begin(), s.end());
}

std::string narrow(const std::wstring& w) {
    return emu::utf16to8(std::u16string(w.begin(), w.end()));
}

bool parseJournalEntry(const std::string& json, PrintedDoc& out) {
    emu::Json j;
    std::string err;
    if (!emu::parseJson(json, j, err)) return false;
    const emu::Json* method = j.get("method");
    const emu::Json* payload = j.get("payload");
    if (!method || (method->text != "device.print" && method->text != "device.reprint") || !payload || !payload->isObject())
        return false;
    out = PrintedDoc();
    if (const emu::Json* v = j.get("seq")) out.journalSeq = v->asInt();
    if (const emu::Json* v = j.get("device_id")) out.deviceId = v->text;
    if (const emu::Json* v = j.get("workstation_id")) out.workstation = v->text;
    if (const emu::Json* v = payload->get("kind")) out.kind = v->text;
    if (const emu::Json* v = payload->get("number")) out.number = static_cast<int>(v->asInt());
    if (const emu::Json* v = payload->get("time")) out.time = v->text;
    if (const emu::Json* v = payload->get("electronic")) out.electronic = v->isBool() && v->b;
    if (const emu::Json* v = payload->get("print_error")) out.printError = v->isBool() && v->b;
    if (const emu::Json* v = payload->get("lines")) {
        for (const auto& l : v->items) out.lines.push_back(l.text);
    }
    if (const emu::Json* q = payload->get("qr")) {
        if (const emu::Json* rows = q->get("rows")) {
            for (const auto& r : rows->items) out.qrRows.push_back(r.text);
        }
    }
    out.reprint = method->text == "device.reprint";
    if (const emu::Json* o = payload->get("output")) {
        if (const emu::Json* v = o->get("screen")) out.showOnScreen = v->isBool() && v->b;
        if (const emu::Json* v = o->get("print")) out.print = v->isBool() && v->b;
        if (const emu::Json* v = o->get("printer")) out.printer = widen(v->text);
    }
    return true;
}

bool parseKeyboardEntry(const std::string& json, KeyboardEvent& out) {
    emu::Json j;
    std::string err;
    if (!emu::parseJson(json, j, err)) return false;
    const emu::Json* method = j.get("method");
    const emu::Json* payload = j.get("payload");
    if (!method || method->text != "device.keyboard" || !payload || !payload->isObject()) return false;
    out = KeyboardEvent();
    if (const emu::Json* v = j.get("seq")) out.journalSeq = v->asInt();
    if (const emu::Json* v = j.get("device_id")) out.deviceId = v->text;
    if (const emu::Json* v = payload->get("text")) out.text = widen(v->text);
    if (const emu::Json* v = payload->get("suffix")) out.suffix = v->text;
    if (const emu::Json* v = payload->get("delay_ms")) out.delayMs = static_cast<int>(v->asInt());
    return true;
}

bool parsePromptEntry(const std::string& json, PromptEvent& out) {
    emu::Json j;
    std::string err;
    if (!emu::parseJson(json, j, err)) return false;
    const emu::Json* method = j.get("method");
    const emu::Json* payload = j.get("payload");
    if (!method || !payload || !payload->isObject()) return false;
    out = PromptEvent();
    if (method->text == "device.prompt_done") {
        out.done = true;
    } else if (method->text != "device.prompt") {
        return false;
    }
    if (const emu::Json* v = j.get("seq")) out.journalSeq = v->asInt();
    if (const emu::Json* v = j.get("device_id")) out.deviceId = v->text;
    if (const emu::Json* v = j.get("workstation_id")) out.workstation = v->text;
    if (const emu::Json* v = payload->get("prompt_id")) out.promptId = v->text;
    if (const emu::Json* v = payload->get("operation")) out.operation = v->text;
    if (out.done) {
        if (const emu::Json* r = payload->get("result")) {
            if (const emu::Json* v = r->get("action")) out.action = v->text;
            if (const emu::Json* v = r->get("text")) out.resultText = widen(v->text);
            if (const emu::Json* v = r->get("value")) {
                out.hasValue = true;
                out.value = v->asDouble();
            }
        }
        return !out.promptId.empty();
    }
    if (const emu::Json* v = payload->get("title")) out.title = widen(v->text);
    if (const emu::Json* v = payload->get("text")) out.text = widen(v->text);
    if (const emu::Json* v = payload->get("amount")) out.amount = v->asDouble();
    if (const emu::Json* v = payload->get("amount_cash")) out.amountCash = v->asDouble();
    if (const emu::Json* v = payload->get("default_card")) out.defaultCard = v->text;
    if (const emu::Json* v = payload->get("deadline")) out.deadline = v->text;
    if (const emu::Json* in = payload->get("input"); in && in->isObject()) {
        out.hasInput = true;
        if (const emu::Json* v = in->get("label")) out.inputLabel = widen(v->text);
        if (const emu::Json* v = in->get("unit")) out.inputUnit = widen(v->text);
        if (const emu::Json* v = in->get("value")) out.inputValue = v->asDouble();
        if (const emu::Json* v = in->get("min")) out.inputMin = v->asDouble();
        if (const emu::Json* v = in->get("max")) out.inputMax = v->asDouble();
    }
    for (const char* key : {"actions", "cards"}) {
        const emu::Json* list = payload->get(key);
        if (!list) continue;
        for (const auto& o : list->items) {
            PromptOption opt;
            if (const emu::Json* v = o.get("id")) opt.id = v->text;
            if (const emu::Json* v = o.get("title")) opt.title = widen(v->text);
            if (const emu::Json* v = o.get("kind")) opt.kind = v->text;
            (std::string(key) == "actions" ? out.actions : out.cards).push_back(opt);
        }
    }
    return !out.promptId.empty();
}

int secondsUntil(const std::string& t) {
    int y, mo, d, h, mi, s;
    if (std::sscanf(t.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    // Сервер пишет местное время со смещением (+05:00) или UTC (Z) — переводим в UTC.
    int offsetMin = 0;
    size_t tz = t.find_first_of("Z+-", 19);
    if (tz != std::string::npos && t[tz] != 'Z') {
        int oh = 0, om = 0;
        if (std::sscanf(t.c_str() + tz + 1, "%d:%d", &oh, &om) >= 1) offsetMin = (oh * 60 + om) * (t[tz] == '-' ? -1 : 1);
    }
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(mo);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(s);
    FILETIME ft, now;
    SystemTimeToFileTime(&st, &ft);
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = ft.dwLowDateTime;
    a.HighPart = ft.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    long long deadline = static_cast<long long>(a.QuadPart / 10000000ULL) - offsetMin * 60LL;
    long long left = deadline - static_cast<long long>(b.QuadPart / 10000000ULL);
    const long long limit = 1LL << 30;  // сроки — минуты; дальние даты не переполняют int
    return static_cast<int>(left > limit ? limit : (left < -limit ? -limit : left));
}

std::wstring kindTitle(const std::string& kind) {
    static const struct { const char* k; const wchar_t* t; } names[] = {
        {"check", L"Кассовый чек"}, {"correction", L"Чек коррекции"}, {"copy", L"Копия чека"},
        {"open_shift", L"Открытие смены"}, {"close_shift", L"Закрытие смены"},
        {"settlements", L"Отчёт о расчётах"}, {"fn_report", L"Отчёт ФН"}, {"x_report", L"X-отчёт"},
        {"text", L"Текстовый документ"}, {"cash_in", L"Внесение"}, {"cash_out", L"Выплата"},
        {"slip", L"Слип терминала"}, {"settlement", L"Сверка итогов"}, {"label", L"Этикетка"},
    };
    for (const auto& n : names)
        if (kind == n.k) return n.t;
    return widen(kind);
}

std::wstring shortTime(const std::string& t) {
    // 2026-10-08T08:38:13.2472745Z → местное «08.10 13:38:13»
    int y, mo, d, h, mi, s;
    if (std::sscanf(t.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return widen(t);
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(y);
    utc.wMonth = static_cast<WORD>(mo);
    utc.wDay = static_cast<WORD>(d);
    utc.wHour = static_cast<WORD>(h);
    utc.wMinute = static_cast<WORD>(mi);
    utc.wSecond = static_cast<WORD>(s);
    SYSTEMTIME local = utc;
    if (t.find('Z') != std::string::npos) SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t buf[32];
    swprintf(buf, 32, L"%02u.%02u %02u:%02u:%02u", local.wDay, local.wMonth, local.wHour, local.wMinute, local.wSecond);
    return buf;
}

}  // namespace tray
