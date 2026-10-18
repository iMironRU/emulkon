// Живой тест оснастки: запросы консоли против отдельного сервера (console/tests/live.ps1 поднимает его
// с временными данными и токенами). Рабочая служба этого компьютера не трогается: адрес — только из
// EMULKON_CONSOLE_URL. Проверяется то, что делают окна этапов 2–3 (docs/local-install.md §4.1): сводка
// с токеном и без, рабочие места (токен — выдать, проверить драйвером, отозвать; новое место; подпись),
// устройства (подпись, сброс настроек, удаление), сценарии и журнал, скачивание обновления (этап 5).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "http.h"
#include "inventory.h"
#include "maintenance.h"
#include "records.h"
#include "summary.h"

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

static std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

static std::wstring value(const std::vector<Row>& rows, const wchar_t* name) {
    for (const Row& r : rows)
        if (r.name == name) return r.value;
    return L"<нет строки>";
}

// openSession — как драйвер в ConnectEquipment (docs/protocol.md §1): заводит устройство на сервере.
static int openSession(const std::string& base, const std::string& workstation, const std::string& token, const char* type,
                       std::string* deviceId = nullptr) {
    auto http = emu::makeHttpClient("EmulkonConsoleLive");
    const std::string body = "{\"workstation_id\":" + jsonString(widen(workstation)) + ",\"device_token\":" +
                             jsonString(widen(token)) + ",\"equipment_type\":\"" + type +
                             "\",\"driver_version\":\"live\",\"interface_revision\":4008}";
    emu::HttpResponse r = http->post(base + "/api/v1/sessions", body, {}, 5000);
    if (deviceId) {
        size_t p = r.body.find("\"device_id\":\"");
        if (p != std::string::npos) {
            p += 13;
            *deviceId = r.body.substr(p, r.body.find('"', p) - p);
        }
    }
    return r.status;
}

static std::vector<Workstation> workstations() {
    std::vector<Workstation> list;
    std::wstring err;
    Reply r = apiRequest("GET", "/control/v1/workstations");
    CHECK(r.status == 200 && parseWorkstations(r.body, list, err));
    return list;
}

static std::vector<Device> devices() {
    std::vector<Device> list;
    std::wstring err;
    Reply r = apiRequest("GET", "/control/v1/devices");
    CHECK(r.status == 200 && parseDevices(r.body, list, err));
    return list;
}

static const Workstation* findWs(const std::vector<Workstation>& list, const std::string& id) {
    for (const auto& w : list)
        if (w.id == id) return &w;
    return nullptr;
}

int main() {
    const std::string base = env("EMULKON_CONSOLE_URL");
    const std::string controlToken = env("LIVE_CONTROL_TOKEN"), deviceToken = env("LIVE_DEVICE_TOKEN");
    if (base.empty() || controlToken.empty() || deviceToken.empty()) {
        std::printf("console_live: нужны EMULKON_CONSOLE_URL, LIVE_CONTROL_TOKEN, LIVE_DEVICE_TOKEN — запускайте live.ps1\n");
        return 2;
    }
    CHECK(narrow(serverBase(ServerSettings{})) == base);  // адрес — из окружения, не служба

    // 1. Сводка без токена консоли: управление закрыто — так и пишет.
    setConsoleToken("");
    std::vector<Row> rows = summaryRows(collect());
    CHECK(value(rows, L"Версия сервера").find(L"нужен токен управления") == 0);
    CHECK(value(rows, L"Устройств").find(L"нужен токен управления") == 0);

    // Неверный токен — тоже отказ.
    setConsoleToken("emk_wrong");
    CHECK(apiRequest("GET", "/control/v1/info").status == 401);

    // 2. С токеном: пустой сервер.
    setConsoleToken(controlToken);
    rows = summaryRows(collect());
    CHECK(value(rows, L"Версия сервера") == widen(EMULKON_VERSION));
    CHECK(value(rows, L"Доступ к управлению") == L"по токену управления");
    CHECK(value(rows, L"Доступ к устройствам") == L"по токену рабочего места");
    CHECK(value(rows, L"Устройств") == L"0");

    // 3. Драйвер заводит устройства: без токена нельзя, с общим токеном устройств — можно.
    const std::string ws = "LIVE-KASSA-1";
    CHECK(openSession(base, ws, "", "KKT") == 401);
    std::string kkt, scanner;
    CHECK(openSession(base, ws, deviceToken, "KKT", &kkt) == 200);
    CHECK(openSession(base, ws, deviceToken, "BarcodeScanner", &scanner) == 200);
    CHECK(!kkt.empty() && !scanner.empty());
    rows = summaryRows(collect());
    CHECK(value(rows, L"Устройств") == L"2");
    CHECK(value(rows, L"Рабочих мест") == L"1");

    std::vector<Device> ds = devices();
    CHECK(ds.size() == 2);
    auto titles = typeTitles(apiRequest("GET", "/control/v1/info").body);
    Table dt = devicesTable(ds, titles);
    CHECK(dt.cells.size() == 2 && dt.cells[0][3] == widen(ws));
    bool kktTitle = false;
    for (const auto& row : dt.cells) kktTitle = kktTitle || row[2] == L"Контрольно-кассовая техника";
    CHECK(kktTitle);

    // 4. Токен рабочего места: выдать — драйвер с ним подключается, отозвать — нет.
    std::string wsToken;
    Reply r = apiIssueWorkstationToken(ws, wsToken);
    CHECK(r.status == 200 && wsToken.rfind("emk_ws_", 0) == 0);
    std::vector<Workstation> list = workstations();
    const Workstation* w = findWs(list, ws);
    CHECK(w && w->token && !w->tokenHint.empty() && wsToken.size() > 4 &&
          widen(wsToken.substr(wsToken.size() - w->tokenHint.size())) == w->tokenHint);
    CHECK(r.body.find(wsToken) != std::string::npos);  // токен — только в ответе на выдачу
    CHECK(apiRequest("GET", "/control/v1/workstations").body.find(wsToken) == std::string::npos);
    CHECK(openSession(base, ws, wsToken, "KKT") == 200);
    CHECK(apiRevokeWorkstationToken(ws).status == 200);
    list = workstations();
    w = findWs(list, ws);
    CHECK(w && !w->token);
    CHECK(openSession(base, ws, wsToken, "KKT") == 401);

    // 5. Новое рабочее место заранее: токен выдаётся до первого подключения.
    std::string newToken;
    CHECK(apiIssueWorkstationToken("LIVE-KASSA-NEW", newToken).status == 200 && !newToken.empty());
    list = workstations();
    w = findWs(list, "LIVE-KASSA-NEW");
    CHECK(w && w->token && w->devices.empty());
    std::string slashToken, rejected;
    CHECK(apiIssueWorkstationToken("касса 2/окно", slashToken).status == 200);  // «/» в коде — экранируется в URL
    CHECK(apiIssueWorkstationToken(std::string(200, 'x'), rejected).status == 400);  // длиннее 128 байт — сервер не примет

    // 6. Подпись и заметка рабочего места: только изменившееся.
    CHECK(metaPatch(L"", L"", L"", L"").empty());
    const std::wstring alias = L"Касса «1» у окна", note = L"Строка 1\r\nСтрока 2 \"кавычки\"";
    CHECK(apiSetWorkstationMeta(ws, metaPatch(L"", alias, L"", note)).status == 200);
    list = workstations();
    w = findWs(list, ws);
    const std::wstring stored = L"Строка 1 Строка 2 \"кавычки\"";  // переводы строк — пробелами (cleanText)
    CHECK(w && w->alias == alias && w->note == stored);
    CHECK(workstationsTable(list).cells.size() == 3);
    CHECK(metaPatch(alias, L"", stored, note) == "{\"alias\":\"\"}");  // заметка после очистки та же
    CHECK(apiSetWorkstationMeta(ws, metaPatch(alias, L"", stored, note)).status == 200);
    list = workstations();
    w = findWs(list, ws);
    CHECK(w && w->alias.empty() && w->note == stored);

    // 7. Устройство: подпись, сброс настроек, удаление.
    CHECK(apiSetDeviceMeta(kkt, metaPatch(L"", L"ККТ у окна", L"", L"")).status == 200);
    ds = devices();
    bool aliased = false;
    for (const auto& d : ds) aliased = aliased || (d.id == kkt && d.alias == L"ККТ у окна" && deviceTitle(d) == L"ККТ у окна");
    CHECK(aliased);
    CHECK(apiResetDevice(kkt).status == 200);
    CHECK(apiDeleteDevice(scanner).status == 200);
    ds = devices();
    CHECK(ds.size() == 1 && ds[0].id == kkt);
    r = apiDeleteDevice(scanner);
    CHECK(r.status == 404 && replyError(r).find(L"не найдено") != std::wstring::npos);
    CHECK(apiResetDevice("нет-такого").status == 404);

    // 8. Журнал: действия консоли видны, токенов в нём нет (правило «Всё в журнале», токены — нет).
    Reply j = apiRequest("GET", "/control/v1/journal?limit=500");
    CHECK(j.status == 200);
    CHECK(j.body.find("control.token_issue") != std::string::npos && j.body.find("control.token_revoke") != std::string::npos);
    CHECK(j.body.find("control.delete_device") != std::string::npos && j.body.find("control.reset_settings") != std::string::npos);
    CHECK(j.body.find(wsToken) == std::string::npos && j.body.find(newToken) == std::string::npos &&
          j.body.find(slashToken) == std::string::npos && !slashToken.empty() && rejected.empty());
    CHECK(j.body.find(controlToken) == std::string::npos && j.body.find(deviceToken) == std::string::npos);

    // 9. Сценарии: создать, выключить и включить, выгрузить, удалить, загрузить обратно.
    const std::string paper = R"({"id":"live-paper","name":"Нет бумаги","enabled":true,)"
                              R"("match":{"type":"kkt","method":"PrintTextDocument"},)"
                              R"("action":{"type":"error","error_code":3,"error_text":"Нет бумаги"},"limit":2})";
    r = apiCreateScenario(paper);
    if (r.status != 200) std::printf("create: %d %s\n", r.status, r.body.c_str());
    CHECK(r.status == 200);
    CHECK(apiCreateScenario(R"({"name":"Медленно","enabled":false,"match":{"every":2},"action":{"type":"delay","delay_ms":100}})")
              .status == 200);
    CHECK(apiCreateScenario(paper).status == 400);  // тот же код — сервер не примет
    std::vector<Scenario> sc;
    std::wstring err;
    r = apiRequest("GET", "/control/v1/scenarios");
    CHECK(r.status == 200 && parseScenarios(r.body, sc, err) && sc.size() == 2);
    const Scenario* p = nullptr;
    for (const auto& s : sc)
        if (s.id == "live-paper") p = &s;
    CHECK(p && p->enabled && p->action == L"ошибка 3: Нет бумаги" && p->condition == L"класс kkt, метод PrintTextDocument");
    CHECK(p && apiPutScenario(p->id, ruleForPut(p->raw, false)).status == 200);
    r = apiRequest("GET", "/control/v1/scenarios");
    CHECK(parseScenarios(r.body, sc, err) && sc.size() == 2 && !sc[0].enabled && sc[0].id == "live-paper");
    CHECK(apiPutScenario("live-paper", ruleForPut(sc[0].raw, true)).status == 200);
    r = apiRequest("GET", "/control/v1/scenarios");
    CHECK(parseScenarios(r.body, sc, err) && sc[0].enabled);

    const std::string file = scenariosFile(sc);
    for (const auto& s : sc) CHECK(apiDeleteScenario(s.id).status == 200);
    CHECK(apiDeleteScenario("live-paper").status == 404);
    ImportResult imp = importScenarios(file, {});
    CHECK(imp.created == 2 && imp.replaced == 0 && imp.errors.empty());
    r = apiRequest("GET", "/control/v1/scenarios");
    std::vector<Scenario> back;
    CHECK(parseScenarios(r.body, back, err) && back.size() == 2 && back[0].id == "live-paper" && back[0].enabled);
    imp = importScenarios(file, back);  // повтор — замена по коду
    CHECK(imp.created == 0 && imp.replaced == 2 && imp.errors.empty());
    imp = importScenarios(R"([{"id":"bad id!","match":{},"action":{"type":"error"}}])", back);
    CHECK(imp.created == 0 && imp.errors.size() == 1);
    CHECK(importScenarios("не json", back).errors.size() == 1);
    CHECK(importScenarios(R"({"x":1})", back).errors.size() == 1);

    // 10. Журнал: последние записи, отбор, выгрузка файлом.
    std::vector<JournalEntry> js;
    bool more = false;
    r = apiRequest("GET", journalPath(L""));
    CHECK(r.status == 200 && parseJournal(r.body, js, more, err) && js.size() > 10);
    Table jt = journalTable(js);
    CHECK(jt.cells.size() == js.size() && std::stoll(jt.keys.front()) > std::stoll(jt.keys.back()));
    r = apiRequest("GET", journalPath(L"control.scenario"));
    CHECK(r.status == 200 && parseJournal(r.body, js, more, err) && !js.empty());
    bool onlyScenario = true;
    for (const auto& j : js) onlyScenario = onlyScenario && j.method == L"control.scenario";
    CHECK(onlyScenario);
    r = apiRequest("GET", journalPath(L"нет-такого-текста-в-журнале"));
    CHECK(r.status == 200 && parseJournal(r.body, js, more, err) && js.empty());

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring all = std::wstring(tmp) + L"emulkon-console-live-journal.jsonl";
    long long lines = 0, filtered = 0;
    bool exported = exportJournal(all, L"", lines, err);
    if (!exported) std::printf("export: %s\n", narrow(err).c_str());
    CHECK(exported && lines > 20);
    CHECK(exportJournal(all, L"control.scenario", filtered, err) && filtered > 0 && filtered < lines);
    std::string first;
    {
        FILE* f = _wfopen(all.c_str(), L"rb");
        char buf[4096] = {};
        if (f) {
            fgets(buf, sizeof buf, f);
            fclose(f);
        }
        first = buf;
    }
    CHECK(first.rfind("{\"seq\":", 0) == 0 && first.find("control.scenario") != std::string::npos);
    DeleteFileW(all.c_str());
    setConsoleToken("");
    CHECK(!exportJournal(all, L"", lines, err) && err.find(L"нужен токен") == 0);  // без токена — отказ, файла нет
    CHECK(GetFileAttributesW(all.c_str()) == INVALID_FILE_ATTRIBUTES);

    // Обновления (этап 5): описания выпуска на сервере эмуляции нет — понятная ошибка; установщик live.ps1
    // положил в каталог загрузок: скачивается с проверкой SHA-256, подменённая сумма — файла нет.
    SetEnvironmentVariableW(L"EMULKON_UPDATE_URL", widen(base + "/downloads/latest.json").c_str());
    Release rel;
    CHECK(!fetchRelease(rel, err) && err.find(L"описания выпуска нет") == 0);
    const std::string setupBody = env("LIVE_SETUP_BODY");
    rel.version = "99.0.0";
    rel.url = base + "/downloads/EmulkonSetup_99.0.0.exe";
    rel.sha256 = sha256Hex(setupBody);
    rel.size = static_cast<long long>(setupBody.size());
    const std::wstring updates = std::wstring(tmp) + L"emulkon-console-live-updates";
    std::wstring setupPath;
    const bool downloaded = downloadRelease(rel, updates, setupPath, err);
    if (!downloaded) std::printf("download: %s\n", narrow(err).c_str());
    CHECK(downloaded && setupPath == updates + L"\\EmulkonSetup_99.0.0.exe");
    {
        std::string got;
        std::wstring rerr;
        HANDLE f = CreateFileW(setupPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) got.append(buf, n);
            CloseHandle(f);
        }
        CHECK(!setupBody.empty() && got == setupBody);
        HANDLE z = CreateFileW((setupPath + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        std::string zone;
        if (z != INVALID_HANDLE_VALUE) {
            char buf[512];
            DWORD n = 0;
            if (ReadFile(z, buf, sizeof buf, &n, nullptr)) zone.assign(buf, n);
            CloseHandle(z);
        }
        CHECK(zone.find("ZoneId=3") != std::string::npos && zone.find(rel.url) != std::string::npos);
    }
    CHECK(!userDownloadsDir().empty());
    DeleteFileW(setupPath.c_str());
    rel.sha256 = std::string(64, '0');
    CHECK(!downloadRelease(rel, updates, setupPath, err) && err.find(L"контрольная сумма") != std::wstring::npos);
    CHECK(GetFileAttributesW(setupPath.c_str()) == INVALID_FILE_ATTRIBUTES);
    rel.sha256 = sha256Hex(setupBody);
    rel.size = 1;
    CHECK(!downloadRelease(rel, updates, setupPath, err) && err.find(L"размер") == 0);
    RemoveDirectoryW(updates.c_str());
    SetEnvironmentVariableW(L"EMULKON_UPDATE_URL", nullptr);

    std::printf("console_live: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
