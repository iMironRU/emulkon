// Модульные тесты трея: поток SSE, запись журнала device.print, настройки, фильтр видов.
#include <cstdio>
#include <string>

#include "config.h"
#include "doc.h"
#include "sse.h"
#include "stream.h"
#include "updates.h"

using namespace tray;

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            ++g_passed;                                                 \
        } else {                                                        \
            ++g_failed;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static void testSse() {
    SseParser p;
    // Событие приходит кусками, с CRLF, комментарием-ping и многострочным data.
    auto ev = p.feed(": ping\r\n\r\nid: 15\nevent: entry\ndata: {\"a\":");
    CHECK(ev.empty());
    ev = p.feed("1}\n\nevent: ready\ndata: {\"seq\":15}\n\ndata: x\ndata: y\n\n");
    CHECK(ev.size() == 3);
    CHECK(ev[0].event == "entry" && ev[0].id == "15" && ev[0].data == "{\"a\":1}");
    CHECK(ev[1].event == "ready" && ev[1].data == "{\"seq\":15}");
    CHECK(ev[2].event == "message" && ev[2].data == "x\ny");
    CHECK(p.feed("event: lagged\ndata: {}\n").empty());  // без пустой строки — ещё не событие
    ev = p.feed("\n");
    CHECK(ev.size() == 1 && ev[0].event == "lagged");
}

static void testDoc() {
    const std::string entry =
        R"({"seq":101,"ts":"2026-10-08T09:00:38.1Z","correlation_id":"r-1","workstation_id":"malmala","device_id":"kkt-05",)"
        R"("direction":"system","method":"device.print","payload":{"seq":2,"kind":"check","number":3,)"
        R"("time":"2026-10-08T09:00:38Z","print_error":true,"lines":["*** ЭМУЛЯТОР ***","ИТОГ =100.00"]}})";
    PrintedDoc d;
    CHECK(parseJournalEntry(entry, d));
    CHECK(d.journalSeq == 101 && d.deviceId == "kkt-05" && d.workstation == "malmala");
    CHECK(d.kind == "check" && d.number == 3 && d.printError && !d.electronic);
    CHECK(d.lines.size() == 2 && d.lines[0] == "*** ЭМУЛЯТОР ***");
    CHECK(kindTitle("check") == L"Кассовый чек");
    CHECK(kindTitle("unknown") == L"unknown");
    CHECK(!parseJournalEntry(R"({"seq":1,"method":"OpenShift","payload":{}})", d));
    CHECK(!parseJournalEntry("не json", d));
    CHECK(shortTime("2026-10-08T09:00:38Z").size() == 14);
}

// Показ и печать решает сервер (output записи, docs/protocol.md §2.3); без output — показать, не печатать.
static void testOutput() {
    PrintedDoc d;
    CHECK(parseJournalEntry(R"({"seq":7,"device_id":"kkt-01","method":"device.print","payload":{"kind":"check","lines":[],)"
                            R"("output":{"screen":false,"print":true,"printer":"Microsoft Print to PDF"}}})", d));
    CHECK(!d.showOnScreen && d.print && d.printer == L"Microsoft Print to PDF" && !d.reprint);
    PrintedDoc old;
    CHECK(parseJournalEntry(R"({"seq":8,"method":"device.print","payload":{"kind":"check"}})", old));
    CHECK(old.showOnScreen && !old.print && old.printer.empty());
    KeyboardEvent kb;
    CHECK(parseKeyboardEntry(R"({"seq":10,"device_id":"scanner-01","method":"device.keyboard","payload":{"text":"4601234567893","suffix":"enter","delay_ms":1500}})", kb));
    CHECK(kb.text == L"4601234567893" && kb.suffix == "enter" && kb.delayMs == 1500 && kb.journalSeq == 10);
    CHECK(!parseKeyboardEntry(R"({"seq":11,"method":"device.print","payload":{"kind":"check"}})", kb));
    PrintedDoc notDoc;
    CHECK(!parseJournalEntry(R"({"seq":10,"method":"device.keyboard","payload":{"text":"1"}})", notDoc));
    PrintedDoc again;
    CHECK(parseJournalEntry(R"({"seq":9,"method":"device.reprint","payload":{"kind":"check","output":{"screen":false,"print":true,"printer":""}}})", again));
    CHECK(again.reprint && again.print && !again.showOnScreen);
}

static void testConfig() {
    Config c;
    c.serverUrl = L"https://эмулкон.local:8443";
    c.workstation = L"КАССА-01";
    c.popupSeconds = 7;
    c.serverListen = L"0.0.0.0:8737";
    c.serverAutostart = true;
    Config back;
    parseConfig("\xEF\xBB\xBF" + serializeConfig(c), back);  // и с BOM
    CHECK(back.serverUrl == c.serverUrl && back.workstation == c.workstation);
    CHECK(back.popupSeconds == 7);
    CHECK(back.serverListen == L"0.0.0.0:8737" && back.serverAutostart);
    CHECK(!Config().serverAutostart);  // без установщика трей сервер сам не поднимает

    Config d;
    parseConfig("# комментарий\nShowPopup=0\nPopupSeconds=-5\nUnknown=1\n  ServerURL = http://x:1  \n", d);
    CHECK(d.popupSeconds == 10 && d.serverUrl == L"http://x:1");  // старые ключи (ShowPopup) пропускаются
    CHECK(effectiveWorkstation(d) == computerName());
    CHECK(urlEncode(L"КАССА 1") == L"%D0%9A%D0%90%D0%A1%D0%A1%D0%90%201");
}

static void testPrompt() {
    const char* open = R"({"seq":41,"device_id":"acquiring-01","workstation_id":"KASSA-01","method":"device.prompt",)"
                       R"("payload":{"prompt_id":"acquiring-01-x-1","operation":"Pay","title":"ОПЛАТА","text":"Приложите карту",)"
                       R"("amount":111,"actions":[{"id":"card","title":"Приложить карту","kind":"primary"},{"id":"cancel","title":"Отмена"}],)"
                       R"("cards":[{"id":"mir","title":"МИР"},{"id":"visa","title":"VISA"}],"default_card":"mir",)"
                       R"("deadline":"2030-01-01T00:00:00+05:00"}})";
    PromptEvent p;
    CHECK(parsePromptEntry(open, p));
    CHECK(!p.done && p.journalSeq == 41 && p.deviceId == "acquiring-01" && p.promptId == "acquiring-01-x-1");
    CHECK(p.title == L"ОПЛАТА" && p.amount == 111 && p.actions.size() == 2 && p.actions[0].kind == "primary");
    CHECK(p.cards.size() == 2 && p.cards[1].id == "visa" && p.defaultCard == "mir");
    CHECK(secondsUntil(p.deadline) > 1000000);
    CHECK(secondsUntil("2000-01-01T00:00:00Z") < 0);

    const char* done = R"({"seq":42,"device_id":"acquiring-01","method":"device.prompt_done",)"
                       R"("payload":{"prompt_id":"acquiring-01-x-1","operation":"Pay","result":{"action":"card","card":"visa","text":"ОДОБРЕНО"}}})";
    PromptEvent d;
    CHECK(parsePromptEntry(done, d) && d.done && d.action == "card" && d.resultText == L"ОДОБРЕНО");
    PrintedDoc doc;
    CHECK(!parseJournalEntry(open, doc));  // запись экрана — не документ
    CHECK(kindTitle("slip") == L"Слип терминала");
}
// Новая версия (issue #8, #20): ответ сервера, «свой» сервер, откуда можно скачать и как сохраняется установщик.
static void testUpdates() {
    UpdateInfo u;
    const std::string gh = "https://github.com/iMironRU/emulkon/releases/download/v0.9.1/EmulkonSetup_0.9.1.exe";
    CHECK(parseUpdateState(R"({"current":"0.9.0","mode":"notify","available":true,"page":"https://github.com/iMironRU/emulkon/releases/tag/v0.9.1",)"
                           R"("latest":{"version":"0.9.1","published":"2026-11-01","sha256":"ABCDEF","size":3,"url":")" + gh + R"("}})",
                           u));
    CHECK(u.available && u.version == L"0.9.1" && u.current == L"0.9.0" && u.sha256 == L"abcdef" && u.published == L"2026-11-01");
    CHECK(u.url == widen(gh) && u.size == 3);
    CHECK(parseUpdateState(R"({"current":"0.9.0","available":false})", u) && !u.available);
    CHECK(parseUpdateState(R"({"available":true})", u) && !u.available);  // без версии — нечего предлагать
    CHECK(!parseUpdateState("не json", u));

    CHECK(isLocalServer(L"http://127.0.0.1:8737") && isLocalServer(L"http://LOCALHOST:8737/") && isLocalServer(L"http://[::1]:8737"));
    CHECK(!isLocalServer(L"http://kassa-srv:8737") && !isLocalServer(L"http://127.0.0.10:8737") && !isLocalServer(L"http://localhost.evil:8737"));

    // Выпуск GitHub — от любого сервера; свой адрес — только от сервера этого компьютера.
    CHECK(downloadAllowed(widen(gh), L"0.9.1", false));
    CHECK(!downloadAllowed(L"https://mirror.local/EmulkonSetup_0.9.1.exe", L"0.9.1", false));
    CHECK(downloadAllowed(L"https://mirror.local/EmulkonSetup_0.9.1.exe", L"0.9.1", true));
    CHECK(downloadAllowed(L"http://127.0.0.1:8799/downloads/EmulkonSetup_0.9.1.exe", L"0.9.1", true));
    CHECK(!downloadAllowed(L"https://github.com/iMironRU/emulkon/releases/download/v0.9.1/EmulkonSetup_0.9.2.exe", L"0.9.1", false));
    CHECK(!downloadAllowed(L"https://github.com/evil/emulkon/releases/download/v0.9.1/EmulkonSetup_0.9.1.exe", L"0.9.1", false));
    CHECK(!downloadAllowed(L"ftp://x/EmulkonSetup_0.9.1.exe", L"0.9.1", true));
    CHECK(!downloadAllowed(L"file:///C:/EmulkonSetup_0.9.1.exe", L"0.9.1", true));
    CHECK(!downloadAllowed(L"https://x/a/../EmulkonSetup_0.9.1.exe", L"0.9.1", true));
    CHECK(!downloadAllowed(L"https://x/EmulkonSetup_0.9.1\\..\\x.exe", L"0.9.1\\..\\x", true));
    CHECK(!downloadAllowed(L"https://x/EmulkonSetup_.exe", L"", true));

    CHECK(sha256Hex("abc") == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");  // SHA-256("abc")

    // Сохранение: только верный файл, с меткой «из Интернета».
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"emulkon-tray-updates";
    CreateDirectoryW(dir.c_str(), nullptr);
    UpdateInfo r;
    r.version = L"0.9.1";
    r.url = widen(gh);
    r.sha256 = sha256Hex("abc");
    r.size = 3;
    std::wstring path, err;
    CHECK(saveInstaller("abc", r, dir, path, err) && path == dir + L"\\EmulkonSetup_0.9.1.exe");
    std::string zone;
    if (FILE* z = _wfopen((path + L":Zone.Identifier").c_str(), L"rb")) {
        char buf[512];
        zone.assign(buf, std::fread(buf, 1, sizeof buf, z));
        std::fclose(z);
    }
    CHECK(zone.find("ZoneId=3") != std::string::npos && zone.find(gh) != std::string::npos);
    DeleteFileW(path.c_str());
    CHECK(!saveInstaller("abd", r, dir, path, err) && err.find(L"SHA-256") != std::wstring::npos);
    CHECK(GetFileAttributesW((dir + L"\\EmulkonSetup_0.9.1.exe").c_str()) == INVALID_FILE_ATTRIBUTES);
    r.size = 4;
    CHECK(!saveInstaller("abc", r, dir, path, err) && err.find(L"размер") == 0);
    r.size = 3;
    r.version = L"0.9.1\\..\\..\\x";
    CHECK(!saveInstaller("abc", r, dir, path, err));
    RemoveDirectoryW(dir.c_str());
    CHECK(!userDownloadsDir().empty());
}

int main() {
    testUpdates();
    testSse();
    testPrompt();
    testDoc();
    testOutput();
    testConfig();
    std::printf("tray unit: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
