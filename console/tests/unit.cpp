// Модульные тесты оснастки: адрес сервера и строки сводки (без MMC и сети).
#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

#include "inventory.h"
#include "maintenance.h"
#include "records.h"
#include "summary.h"
#include "taskdialog.h"

using namespace console;

static int g_passed = 0, g_failed = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_passed;                                                   \
        } else {                                                          \
            ++g_failed;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

static std::wstring value(const std::vector<Row>& rows, const wchar_t* name) {
    for (const Row& r : rows)
        if (r.name == name) return r.value;
    return L"<нет строки>";
}

static void testAddress() {
    ServerSettings s;
    CHECK(listenAddr(s) == L"127.0.0.1:8737");
    s.network = L"1";
    CHECK(listenAddr(s) == L"0.0.0.0:8737");
    s.addr = L"192.168.1.5:9000";
    CHECK(listenAddr(s) == L"192.168.1.5:9000");  // Addr важнее Network, как у сервера

    CHECK(localUrl(L"0.0.0.0:8737") == L"http://127.0.0.1:8737");
    CHECK(localUrl(L":8738") == L"http://127.0.0.1:8738");
    CHECK(localUrl(L"[::]:8737") == L"http://127.0.0.1:8737");
    CHECK(localUrl(L"192.168.1.5:9000") == L"http://192.168.1.5:9000");
}

static void testSummary() {
    Inputs in;
    in.service = Service::Running;
    in.settings.readable = true;
    in.info = {200, R"({"version":"0.8.0","device_open_mode":true,"control_open_mode":false})", ""};
    in.devices = {200, R"({"devices":[{"device_id":"a"},{"device_id":"b"}]})", ""};
    in.workstations = {200, R"({"workstations":[]})", ""};
    auto rows = summaryRows(in);
    CHECK(value(rows, L"Служба Emulkon") == L"работает");
    CHECK(value(rows, L"Версия сервера") == L"0.8.0");
    CHECK(value(rows, L"Адрес") == L"127.0.0.1:8737 — только этот компьютер");
    CHECK(value(rows, L"Веб-интерфейс") == L"http://127.0.0.1:8737/");
    CHECK(value(rows, L"Устройств") == L"2");
    CHECK(value(rows, L"Рабочих мест") == L"0");
    CHECK(value(rows, L"Доступ к устройствам") == L"открытый, без токена");
    CHECK(value(rows, L"Доступ к управлению") == L"по токену управления");

    // Сеть, ключ не читается, сервер молчит.
    Inputs off;
    off.service = Service::Stopped;
    off.info = off.devices = off.workstations = {0, "", "connection refused"};
    rows = summaryRows(off);
    CHECK(value(rows, L"Служба Emulkon") == L"остановлена");
    CHECK(value(rows, L"Версия сервера").find(L"нет связи с сервером") == 0);
    CHECK(value(rows, L"Устройств").find(L"нет связи") == 0);
    CHECK(value(rows, L"Адрес").find(L"видны администратору") != std::wstring::npos);
    CHECK(value(rows, L"Доступ к устройствам") == L"<нет строки>");

    // Управление по токену: 401.
    Inputs locked = in;
    locked.settings.network = L"1";
    locked.info = locked.devices = locked.workstations = {401, R"({"error":"unauthorized"})", ""};
    rows = summaryRows(locked);
    CHECK(value(rows, L"Версия сервера").find(L"нужен токен управления") == 0);
    CHECK(value(rows, L"Адрес") == L"0.0.0.0:8737 — кассы в сети");

    CHECK(serviceText(Service::Missing) == L"не установлена");
}

static void testService() {
    ServiceInfo svc;
    svc.state = Service::Running;
    svc.configKnown = true;
    svc.startType = L"автоматически";
    svc.account = L"NT AUTHORITY\\LocalService";
    svc.command = L"\"C:\\Program Files\\Emulkon\\bin\\emulkon.exe\" -service";
    ServerSettings s;
    s.readable = true;
    s.network = L"1";
    s.controlToken = true;
    auto rows = serviceRows(svc, s, true, L"C:\\ProgramData\\Emulkon");
    CHECK(value(rows, L"Состояние") == L"работает");
    CHECK(value(rows, L"Учётная запись") == L"NT AUTHORITY\\LocalService");
    CHECK(value(rows, L"Адрес") == L"0.0.0.0:8737");
    CHECK(value(rows, L"Кассы в сети").find(L"да") == 0);
    CHECK(value(rows, L"Токен управления").find(L"задан") == 0);
    CHECK(value(rows, L"Токен устройств").find(L"не задан") == 0);
    CHECK(value(rows, L"Токен в этой консоли").find(L"введён") == 0);
    CHECK(value(rows, L"Журнал службы") == L"C:\\ProgramData\\Emulkon\\logs\\service.log");

    // Обычный пользователь: настройки службы и её конфигурация закрыты.
    ServiceInfo hidden;
    hidden.state = Service::Stopped;
    rows = serviceRows(hidden, ServerSettings{}, false, L"C:\\ProgramData\\Emulkon");
    CHECK(value(rows, L"Тип запуска") == L"видно администратору");
    CHECK(value(rows, L"Токен управления") == L"видно администратору");
    CHECK(value(rows, L"Кассы в сети") == L"<нет строки>");
    CHECK(value(rows, L"Токен в этой консоли") == L"нет");

    ServiceInfo missing;
    missing.state = Service::Missing;
    CHECK(value(serviceRows(missing, s, false, L"x"), L"Тип запуска") == L"<нет строки>");

    CHECK(isLocalOnly(L"127.0.0.1:8737") && isLocalOnly(L"localhost:1") && !isLocalOnly(L"0.0.0.0:8737"));
}

static void testTokens() {
    // Хеш — как у сервера (server/internal/secret): SHA-256 в hex, нижний регистр.
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const unsigned char bytes[] = {0xfb, 0xff, 0x00, 0x10};
    CHECK(base64url(bytes, 4) == "-_8AEA");
    CHECK(base64url(bytes, 3) == "-_8A");
    std::string a = newToken(), b = newToken();
    CHECK(a.size() == 4 + 43 && a.rfind("emk_", 0) == 0);
    CHECK(a != b);
    CHECK(a.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 4) == std::string::npos);
}

static void testInventory() {
    std::vector<Workstation> ws;
    std::wstring err;
    CHECK(parseWorkstations(R"({"workstations":[{"workstation_id":"KASSA-1","alias":"Касса 1","first_seen":"0001-01-01T00:00:00Z",
        "last_seen":"2026-10-14T16:31:13.46Z","infobases":["Розница · C:\\b1","УТ · C:\\b2"],"devices":["kkt-01"],"sessions":2,
        "printers":["PDF","HP"],"default_printer":"HP","token":{"issued_at":"2026-10-15T08:00:00Z","hint":"a1b2"}},
        {"workstation_id":"KASSA-2","infobases":[],"devices":[],"printers":[]}]})", ws, err));
    CHECK(ws.size() == 2);
    CHECK(ws[0].id == "KASSA-1" && ws[0].alias == L"Касса 1" && ws[0].token && ws[0].tokenHint == L"a1b2");
    CHECK(ws[0].firstSeen == L"—" && ws[0].lastSeen.size() == 16);  // «14.10.2026 19:31» в поясе машины
    CHECK(ws[0].infobases.size() == 2 && ws[0].defaultPrinter == L"HP" && ws[0].sessions == 2);
    CHECK(!ws[1].token && workstationTitle(ws[1]) == L"KASSA-2");
    Table t = workstationsTable(ws);
    CHECK(t.cells.size() == 2 && t.keys[0] == "KASSA-1");
    CHECK(t.cells[0][2] == L"Розница · C:\\b1 и ещё 1");
    CHECK(t.cells[0][5] == L"выдан …a1b2" && t.cells[1][5] == L"нет");
    CHECK(!parseWorkstations("{}", ws, err) && !err.empty());

    std::vector<Device> ds;
    CHECK(parseDevices(R"({"devices":[{"device_id":"kkt-01","type":"kkt","equipment_type":"KKT","name":"ЭМУЛЯТОР: ККТ",
        "workstation_id":"KASSA-1","created_at":"2026-10-09T11:29:22Z","commands":12,"active_sessions":1,"online":true,
        "last_error":{"code":-3,"text":"Нет бумаги"},"state":{"paper":false,"shift":{"open":true}},"settings":{"rate":1}}]})",
                       ds, err));
    CHECK(ds.size() == 1 && ds[0].id == "kkt-01" && ds[0].online && ds[0].commands == 12);
    CHECK(ds[0].lastError == L"код -3: Нет бумаги");
    CHECK(ds[0].state.size() == 2 && ds[0].state[0].name == L"paper" && ds[0].state[0].value == L"false");
    CHECK(ds[0].state[1].value.find(L"open") != std::wstring::npos);
    auto titles = typeTitles(R"({"equipment_types":[{"key":"kkt","title":"Контрольно-кассовая техника"}]})");
    Table dt = devicesTable(ds, titles);
    CHECK(dt.cells[0][2] == L"Контрольно-кассовая техника" && dt.cells[0][4] == L"да");
    CHECK(devicesTable(ds, {}).cells[0][2] == L"KKT");  // без названий классов — тип БПО

    CHECK(localTime("") == L"—" && localTime("garbage") == L"—");
    CHECK(localTime("2026-10-15T08:00:00+03:00") == localTime("2026-10-15T05:00:00Z"));
    CHECK(replyError({0, "", "timeout"}).find(L"нет связи") == 0);
    CHECK(replyError({404, R"({"error":{"code":"not_found","message":"устройство x не найдено"}})", ""}) == L"устройство x не найдено");
    CHECK(replyError({500, "oops", ""}) == L"ошибка сервера: HTTP 500");
    CHECK(urlPart("KASSA 1/ä") == "KASSA%201%2F%C3%A4");
    CHECK(cleanText(L"  Касса\r\n1\tу окна  ") == L"Касса 1 у окна");
    CHECK(metaPatch(L"a", L"a", L"n", L"n").empty());
    CHECK(metaPatch(L"a", L"a\r\n", L"n", L"n").empty());  // после очистки — то же
    CHECK(metaPatch(L"", L"b", L"n", L"m") == "{\"alias\":\"b\",\"note\":\"m\"}");
    CHECK(jsonString(L"Касса \"1\"") == "\"Касса \\\"1\\\"\"");
}

static void testRecords() {
    std::vector<Scenario> sc;
    std::wstring err;
    CHECK(parseScenarios(R"({"rules":[
        {"id":"paper","name":"Нет бумаги","enabled":true,"match":{"type":"kkt","method":"PrintCheck","nth_call":3},
         "action":{"type":"error","error_code":-3,"error_text":"Нет бумаги"},"limit":2,"fired":1,"seen":5},
        {"id":"slow","enabled":false,"match":{"probability":0.25,"param":{"index":2,"contains":"ИНН"}},
         "action":{"type":"delay","delay_ms":1500},"fired":0,"seen":0},
        {"id":"st","enabled":true,"match":{},"action":{"type":"set_state","state":{"paper":false}},"fired":0,"seen":0}]})",
                         sc, err));
    CHECK(sc.size() == 3);
    CHECK(sc[0].condition == L"класс kkt, метод PrintCheck, на 3-м вызове");
    CHECK(sc[0].action == L"ошибка -3: Нет бумаги");
    CHECK(sc[1].condition == L"параметр №2 содержит «ИНН», вероятность 25%");
    CHECK(sc[1].action == L"задержка 1500 мс");
    CHECK(sc[2].condition == L"любой вызов" && sc[2].action == L"состояние: paper=false");
    Table t = scenariosTable(sc);
    CHECK(t.cells[0][2] == L"да" && t.cells[0][5] == L"2 раз" && t.cells[0][6] == L"1 из 5");
    CHECK(t.cells[1][0] == L"slow" && t.cells[1][5] == L"—");  // без имени — код

    const std::string put = ruleForPut(sc[0].raw, false);
    CHECK(put.find("\"enabled\":false") != std::string::npos);
    CHECK(put.find("fired") == std::string::npos && put.find("seen") == std::string::npos);
    CHECK(put.find("Нет бумаги") != std::string::npos);  // UTF-8 как есть

    const std::string file = scenariosFile(sc);
    CHECK(file.find("\n  \"rules\": [") != std::string::npos && file.find("fired") == std::string::npos);
    std::vector<Scenario> again;
    CHECK(parseScenarios(file, again, err) && again.size() == 3 && again[1].action == sc[1].action);

    std::vector<JournalEntry> js;
    bool more = false;
    CHECK(parseJournal(R"({"entries":[
        {"seq":10,"ts":"2026-10-15T10:00:01Z","direction":"in","device_id":"kkt-01","workstation_id":"K1","method":"PrintCheck",
         "payload":{"params":["<CheckPackage/>"]}},
        {"seq":11,"ts":"2026-10-15T10:00:02Z","direction":"out","device_id":"kkt-01","method":"PrintCheck","latency_ms":12,
         "applied_rules":["paper"],"payload":{"result":false}}],"last_seq":11,"has_more":true})",
                       js, more, err));
    CHECK(js.size() == 2 && more);
    CHECK(js[0].direction == L"команда" && js[1].direction == L"ответ" && js[1].latency == L"12 мс" && js[1].rules == L"paper");
    CHECK(js[0].payload.find(L"<CheckPackage/>") != std::wstring::npos);
    CHECK(js[0].time.size() == 19);  // с секундами
    Table jt = journalTable(js);
    CHECK(jt.keys[0] == "11" && jt.keys[1] == "10");  // новые сверху

    CHECK(journalPath(L"") == "/control/v1/journal?limit=500");
    CHECK(journalPath(L"Касса 1", 50) == "/control/v1/journal?limit=50&q=%D0%9A%D0%B0%D1%81%D1%81%D0%B0%201");
    CHECK(journalPath(L"", 0, true) == "/control/v1/journal?format=jsonl");
    CHECK(prettyJson(R"({"a":[1,{"b":"в"}],"c":{}})") == L"{\r\n  \"a\": [\r\n    1,\r\n    {\r\n      \"b\": \"в\"\r\n    }\r\n  ],\r\n  \"c\": {}\r\n}");
    setJournalFilter(L"x");
    CHECK(journalFilter() == L"x");
    setJournalFilter(L"");
}

static void testMaintenance() {
    // Версии: числа, а не строки; предварительная младше выпуска.
    CHECK(compareVersions("0.10.0", "0.9.9") > 0);
    CHECK(compareVersions("0.9.0", "0.9.0") == 0);
    CHECK(compareVersions("0.9.0-rc1", "0.9.0") < 0);
    CHECK(compareVersions("0.9.0", "0.9.0-rc1") > 0);
    CHECK(compareVersions("1.0.0", "0.99.99") > 0);
    CHECK(compareVersions("мусор", "0.0.1") < 0);

    const std::string sha(64, 'a');
    const std::string good = R"({"version":"0.9.0","published":"2026-10-20","size":123,)"
                             R"("url":"https://github.com/iMironRU/emulkon/releases/download/v0.9.0/EmulkonSetup_0.9.0.exe",)"
                             R"("sha256":")" + std::string(64, 'A') + R"j(","notes":"### Добавлено\n- **Драйвер** для `Linux` — [tests](tests/linux)"})j";
    Release r;
    std::wstring err;
    CHECK(parseRelease(good, false, r, err));
    CHECK(r.version == "0.9.0" && r.size == 123 && r.sha256 == sha && r.published == "2026-10-20");
    CHECK(releasePage(r) == L"https://github.com/iMironRU/emulkon/releases/tag/v0.9.0");  // «Что нового» (#21)
    // http — только для живого теста; файл — установщик той же версии; сумма — 64 hex.
    auto with = [&](const std::string& from, const std::string& to) {
        std::string s = good;
        s.replace(s.find(from), from.size(), to);
        return s;
    };
    CHECK(!parseRelease(with("https://github.com", "http://github.com"), false, r, err));
    CHECK(parseRelease(with("https://github.com", "http://github.com"), true, r, err));
    CHECK(!parseRelease(with("EmulkonSetup_0.9.0.exe", "EmulkonSetup_0.8.0.exe"), false, r, err));
    CHECK(!parseRelease(with("EmulkonSetup_0.9.0.exe", "evil.exe"), false, r, err));
    CHECK(!parseRelease(with(std::string(64, 'A'), "abc"), false, r, err));
    CHECK(!parseRelease(with("\"0.9.0\"", "\"0.9\""), false, r, err));
    CHECK(!parseRelease("не json", false, r, err));
    Release own;
    own.version = "0.9.0";
    own.url = "http://127.0.0.1:8799/downloads/EmulkonSetup_0.9.0.exe";
    CHECK(releasePage(own).empty());  // не выпуск GitHub — страницы нет

    // Настройки: выгрузка и разбор туда и обратно.
    std::vector<Setting> in = {{L"Addr", L"0.0.0.0:8737"}, {L"Network", L"", 1, true}, {L"LogLevel", L"debug"},
                               {L"ControlTokenHash", widen(sha)}};
    const std::string file = settingsFile(in, "KASSA-1", "2026-10-15T12:00:00");
    std::vector<Setting> out;
    std::vector<std::wstring> warnings;
    CHECK(parseSettingsFile(file, out, warnings, err));
    CHECK(out.size() == 4 && warnings.empty());
    CHECK(settingsSummary(out) == settingsSummary(in));
    CHECK(settingsSummary(out).find(L"ControlTokenHash = задан (SHA-256)") != std::wstring::npos);
    CHECK(settingsSummary(out).find(L"DataDir = по умолчанию") != std::wstring::npos);
    CHECK(file.find("KASSA-1") != std::string::npos && file.find("emulkon-settings") != std::string::npos);
    // С BOM, незнакомое значение — предупреждение; ошибки — отказ целиком.
    CHECK(parseSettingsFile("\xEF\xBB\xBF{\"format\":\"emulkon-settings\",\"version\":1,\"server\":{\"Foo\":\"1\",\"LogLevel\":\"INFO\"}}",
                            out, warnings, err));
    CHECK(out.size() == 1 && out[0].text == L"info" && warnings.size() == 1);
    const char* bad[] = {
        R"({"format":"emulkon-settings","version":2,"server":{}})",
        R"({"format":"other","version":1,"server":{}})",
        R"({"format":"emulkon-settings","version":1,"server":{"Network":2}})",
        R"({"format":"emulkon-settings","version":1,"server":{"Network":"1"}})",
        R"({"format":"emulkon-settings","version":1,"server":{"LogLevel":"trace"}})",
        R"({"format":"emulkon-settings","version":1,"server":{"DeviceTokenHash":"emk_секрет"}})",
        R"({"format":"emulkon-settings","version":1,"server":{"Addr":"8737"}})",
        R"({"format":"emulkon-settings","version":1,"server":{"DataDir":"C:\\a\nb"}})",
        R"({"format":"emulkon-settings","version":1,"server":{"UpdateMode":"auto"}})",
        R"({"format":"emulkon-settings","version":1,"server":{"UpdateURL":"ftp://x/latest.json"}})",
    };
    for (const char* b : bad) CHECK(!parseSettingsFile(b, out, warnings, err));
    // Обновления (issue #8): режим и свой latest.json переносятся вместе с остальными настройками.
    CHECK(parseSettingsFile(R"({"format":"emulkon-settings","version":1,"server":{"UpdateMode":"OFF","UpdateURL":"https://x/latest.json"}})",
                            out, warnings, err));
    CHECK(out.size() == 2 && out[0].text == L"off" && out[1].text == L"https://x/latest.json" && warnings.empty());
}

// saveBmp — снимок окна (PrintWindow) в BMP: посмотреть окно новой версии глазами (EMULKON_TASKDIALOG_SHOT).
static void saveBmp(HWND hwnd, const std::wstring& path) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(hwnd, dc, 2);  // PW_RENDERFULLCONTENT
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        const DWORD size = static_cast<DWORD>(w) * h * 4;
        BITMAPFILEHEADER fh{0x4D42, static_cast<DWORD>(sizeof fh + sizeof bi.bmiHeader + size), 0, 0,
                            static_cast<DWORD>(sizeof fh + sizeof bi.bmiHeader)};
        std::fwrite(&fh, sizeof fh, 1, f);
        std::fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        std::fwrite(bits, size, 1, f);
        std::fclose(f);
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

// Окно новой версии (#25): TaskDialog с кликабельной ссылкой показывается (манифест ресурсом 3 активируется на
// время окна) и закрывается «Отменой» — answer = IDCANCEL. Окно закрывает фоновый поток.
static void testTaskDialog() {
    struct Closer {
        HWND found = nullptr;
        bool direct = false;  // внутри DirectUIHWND — это TaskDialog, а не MessageBox
    } c;
    std::thread closer([&c] {
        for (int i = 0; i < 100 && !c.found; ++i) {
            Sleep(100);
            HWND w = FindWindowW(L"#32770", L"Эмулкон");
            DWORD pid = 0;
            if (w) GetWindowThreadProcessId(w, &pid);
            if (w && pid == GetCurrentProcessId()) c.found = w;
        }
        if (!c.found) {
            std::printf("FAIL: окно новой версии не появилось\n");
            ExitProcess(1);  // окно, которого не видно, не закрыть — иначе тест повиснет
        }
        Sleep(300);
        c.direct = FindWindowExW(c.found, nullptr, L"DirectUIHWND", nullptr) != nullptr;
        wchar_t shot[MAX_PATH];
        if (GetEnvironmentVariableW(L"EMULKON_TASKDIALOG_SHOT", shot, MAX_PATH)) saveBmp(c.found, shot);
        PostMessageW(c.found, WM_COMMAND, IDCANCEL, 0);
    });
    int answer = 0;
    const bool shown = console::newVersionTaskDialog(
        nullptr, L"Вышла версия 0.9.2 от 2026-10-25", L"Установлена 0.9.1.",
        L"https://github.com/iMironRU/emulkon/releases/tag/v0.9.2",
        L"Установщик проверяется по SHA-256 из описания выпуска; откроется мастер обновления — он перезапустит службу.",
        answer);
    closer.join();
    CHECK(shown && answer == IDCANCEL && c.direct);
}

int main() {
    testTaskDialog();
    testMaintenance();
    testRecords();
    testInventory();
    testAddress();
    testSummary();
    testService();
    testTokens();
    std::printf("console_unit: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
