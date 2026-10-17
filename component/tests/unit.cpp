// Модульные тесты ядра компоненты: JSON, текст, параметры подключения, таблица методов.
#include <clocale>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include "device_client.h"
#include "events.h"
#include "json.h"
#include "methods.h"
#include "platform.h"
#include "text.h"
#include "update.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#endif

using namespace emu;

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_passed;                                                   \
        } else {                                                          \
            ++g_failed;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

static void testJson() {
    Json j;
    std::string err;
    CHECK(parseJson(R"({"a":[1,2.5,-3e2,true,false,null,"x\"y\\zЖ😀"],"b":{"c":9007199254740993}})", j, err));
    CHECK(err.empty());
    const Json* a = j.get("a");
    CHECK(a && a->isArray() && a->items.size() == 7);
    CHECK(a->items[0].isInteger() && a->items[0].asInt() == 1);
    CHECK(!a->items[1].isInteger() && a->items[1].asDouble() == 2.5);
    CHECK(a->items[3].isBool() && a->items[3].b);
    CHECK(a->items[5].isNull());
    CHECK(a->items[6].text == "x\"y\\z\xD0\x96\xF0\x9F\x98\x80");  // Ж и эмодзи в UTF-8
    // Большие целые — без потери точности (хранятся текстом).
    CHECK(j.get("b")->get("c")->text == "9007199254740993");

    // dump → parse даёт то же значение.
    Json back;
    CHECK(parseJson(j.dump(), back, err) && back == j);

    CHECK(!parseJson("{\"a\":1,}", back, err));
    CHECK(!parseJson("[1,2", back, err));
    CHECK(!parseJson("{} x", back, err));

    CHECK(Json::number(int64_t(4008)).dump() == "4008");
    CHECK(Json::number(0.1).dump() == "0.1");
    CHECK(Json::number(1500.5).dump() == "1500.5");
    CHECK(Json::string("a\nb").dump() == "\"a\\nb\"");
    CHECK(Json::numberRaw("1.0") == Json::numberRaw("1"));  // сравнение чисел — по значению
    CHECK(Json::string("1") != Json::numberRaw("1"));
}

// Числа JSON не зависят от локали процесса: клиент 1С под Linux включает локаль системы (ru_RU — десятичная
// запятая), и вес 0,532 приходил в 1С нулём. Нет такой локали в системе — проверка без неё.
static void testJsonLocale() {
    const char* names[] = {"ru_RU.UTF-8", "ru_RU.utf8", "de_DE.UTF-8", "Russian_Russia.1251", "ru-RU"};
    const char* set = nullptr;
    for (const char* n : names)
        if ((set = std::setlocale(LC_NUMERIC, n)) != nullptr) break;
    if (set) {
        char probe[16];
        std::snprintf(probe, sizeof probe, "%.1f", 0.5);
        std::printf("локаль чисел %s: printf пишет «%s»\n", set, probe);
    }
    CHECK(Json::number(0.532).text == "0.532");
    CHECK(Json::number(-12.25).text == "-12.25");
    Json w;
    std::string err;
    CHECK(parseJson("{\"weight\":0.777,\"big\":1.5e3}", w, err));
    CHECK(w.get("weight") && w.get("weight")->asDouble() == 0.777);
    CHECK(w.get("big") && w.get("big")->asDouble() == 1500 && w.get("big")->asInt() == 1500);
    CHECK(Json::number(0.1 + 0.2).dump() == "0.30000000000000004");
    std::setlocale(LC_NUMERIC, "C");
}

// Ошибка 900 говорит, где искали сервер и что сделать (issue #14); начало — как прежде: по нему БПО узнаёт «нет связи».
static void testNoConnectionHint() {
    for (const char* local : {"http://127.0.0.1:8737", "http://localhost:8737/", "http://[::1]:8737", "HTTP://LocalHost:9000"}) {
        const std::string m = noConnectionMessage(local, "соединение отклонено");
        CHECK(m.rfind(std::string("Нет связи с сервером эмуляции ") + local + ": соединение отклонено.", 0) == 0);
        CHECK(m.find("не запущен или не установлен") != std::string::npos && m.find("imiron.ru/emulkon") != std::string::npos);
        CHECK(m.find("ghcr.io/imironru/emulkon") != std::string::npos);
    }
    const std::string remote = noConnectionMessage("http://kassa-srv.local:8737", "таймаут");
    CHECK(remote.find("на kassa-srv.local запущен") != std::string::npos && remote.find("открыт для касс в сети") != std::string::npos);
    CHECK(remote.find("не установлен") == std::string::npos);
    CHECK(noConnectionMessage("http://10.0.0.5:8737", "").rfind("Нет связи с сервером эмуляции http://10.0.0.5:8737. ", 0) == 0);
}

static void testInfobase() {
    // Как запускают 1С: ярлык, стартер, run.ps1. Пароль (/P) не должен попасть в результат.
    CHECK(infobaseFromCommandLine(R"x("C:\Program Files\1cv8\bin\1cv8c.exe" ENTERPRISE /F"C:\.temp\Base1C\EmulkonTest" /N"Админ" /P"секрет")x") ==
          "C:\\.temp\\Base1C\\EmulkonTest");
    CHECK(infobaseFromCommandLine(R"x(1cv8.exe ENTERPRISE /F "D:\Базы\Розница" /P secret)x") == "D:\\Базы\\Розница");
    CHECK(infobaseFromCommandLine(R"x(1cv8c.exe /S"srv1c\retail_test" /Nkassir)x") == "srv1c/retail_test");
    CHECK(infobaseFromCommandLine(R"x(1cv8c.exe /SAOnRestart /IBName"Розница (тест)")x") == "Розница (тест)");
    CHECK(infobaseFromCommandLine(R"x(1cv8c.exe /IBConnectionString"Srvr=""srv:1541"";Ref=""ut"";" /Pпароль)x") == "srv:1541/ut");
    CHECK(infobaseFromCommandLine(R"x(1cv8c.exe /IBConnectionString"File=""C:\b"";")x") == "C:\\b");
    CHECK(infobaseFromCommandLine(R"x(1cv8c.exe ENTERPRISE /Pпароль /Nuser)x").empty());
    CHECK(infobaseFromCommandLine("").empty());
}

static void testText() {
    std::string ru = "Кассовая смена не открыта ё Ё 😀";
    CHECK(utf16to8(utf8to16(ru)) == ru);
    CHECK(utf8to16("Ж") == std::u16string(u"Ж"));
    CHECK(equalsIgnoreCase(u"получитьревизиюинтерфейса", u"ПолучитьРевизиюИнтерфейса"));
    CHECK(equalsIgnoreCase(u"ЁЛКА", u"ёлка"));
    CHECK(equalsIgnoreCase(u"processcheck", u"ProcessCheck"));
    CHECK(!equalsIgnoreCase(u"ProcessCheck", u"ProcessChecks"));
    CHECK(!equalsIgnoreCase(u"ProcessChecks", u"ProcessCheck"));

    CHECK(xmlAttr("a&b<\"c\">") == "a&amp;b&lt;&quot;c&quot;&gt;");

    auto p = parseConnectionParameters(
        "<?xml version=\"1.0\"?><Parameters>\n"
        "<Parameter Name=\"ServerURL\" Value=\"http://emulkon:8737/\"/>\n"
        "<Parameter Value='s&amp;cret&#33;' Name='DeviceToken'/>\n"
        "<Parameter Name=\"WorkstationID\" Value=\"КАССА-01\"></Parameter>\n"
        "<Parameter Name=\"Empty\"/>\n"
        "</Parameters>");
    CHECK(p.size() == 4);
    CHECK(p["ServerURL"] == "http://emulkon:8737/");
    CHECK(p["DeviceToken"] == "s&cret!");
    CHECK(p["WorkstationID"] == "КАССА-01");
    CHECK(p.count("Empty") && p["Empty"].empty());
    CHECK(parseConnectionParameters("").empty());
    CHECK(parseConnectionParameters("<Parameters></Parameters>").empty());
}

static void testMethods() {
    std::set<std::u16string> names;
    for (int i = 0; i < methodCount(); ++i) {
        const MethodDef& m = methods()[i];
        CHECK(names.insert(utf8to16(utf16to8(m.en))).second);
        CHECK(names.insert(m.ru).second);
        CHECK(m.params >= 0 && m.params <= 8);
        // Методы раздела 3.5 — с DeviceID в позиции 0, значит минимум 1 параметр.
        if (m.handling == Handling::Forward) CHECK(m.params >= 1);
    }
    CHECK(methodCount() == 99);
    // Эквайринг: «C» в ОплатаCВыдачейНаличных — латинская, как вызывает БПО; с кириллической не найдётся.
    CHECK(findMethod(u"ОплатаCВыдачейНаличных") == findMethod(u"PayWithCashWithdrawal"));
    CHECK(findMethod(u"ОплатаСВыдачейНаличных") == -1);
    CHECK(methods()[findMethod(u"Оплатить")].params == 4 && methods()[findMethod(u"Оплатить")].waitsForPerson);
    CHECK(!methods()[findMethod(u"ОтменитьПлатеж")].waitsForPerson);
    CHECK(methods()[findMethod(u"ПолучитьПараметрыКарты")].params == 8);
    CHECK(findMethod(u"TerminalParamters") == findMethod(u"ПараметрыТерминала"));
    CHECK(findMethod(u"СФОРМИРОВАТЬЧЕК") == findMethod(u"ProcessCheck"));
    CHECK(findMethod(u"ProcessCheck") >= 0);
    CHECK(methods()[findMethod(u"ProcessCheck")].params == 4);
    CHECK(methods()[findMethod(u"ПолучитьРезультатыЗапросаКМ")].params == 3);
    CHECK(findMethod(u"НетТакогоМетода") == -1);
    CHECK(methods()[0].handling == Handling::Local && methods()[0].params == 0);
}

static void testSse() {
    SseParser p;
    std::vector<SseParser::Event> out;
    // куски режут строки посередине; \r\n и пинги-комментарии
    p.feed("event: ready\ndata: {}\n\n: ping\n\nid: abc.1\nevent: dev", out);
    CHECK(out.size() == 1 && out[0].event == "ready");
    out.clear();
    p.feed("ice\r\ndata: {\"name\":\"Barcode\",\r\ndata: \"data\":\"46\"}\r\n\r\n", out);
    CHECK(out.size() == 1 && out[0].id == "abc.1" && out[0].event == "device");
    CHECK(out[0].data == "{\"name\":\"Barcode\",\n\"data\":\"46\"}");
    out.clear();
    p.feed("data:x\n\n", out);  // без пробела после двоеточия, id не наследуется
    CHECK(out.size() == 1 && out[0].data == "x" && out[0].id.empty());
}

static void testRequestId() {
    std::string a = newRequestId(), b = newRequestId();
    CHECK(a.size() == 36 && a[8] == '-' && a[14] == '4');
    CHECK(a != b);
    CHECK(nowRfc3339().size() == 20);
}


static void testUpdateLogic() {
    CHECK(compareVersions("0.2.0", "0.2.0") == 0);
    CHECK(compareVersions("0.10.0", "0.9.9") == 1);
    CHECK(compareVersions("0.2.0", "0.2.1") == -1);
    CHECK(compareVersions("0.2.0-dev", "0.2.0") == 0);
    CHECK(compareVersions("dev", "0.0.1") == -1);
    CHECK(!validVersion("1.2") && !validVersion("1.2.3.4") && !validVersion("a.b.c") && validVersion("1.22.333"));

    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 байт: дополнение уходит во второй блок.
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");

    auto names = newerVersions({"0.1.0", "0.3.0", "x", "0.10.0", "0.2.0", "0.3.0-rc"}, "0.2.0");
    CHECK(names.size() == 2 && names[0] == "0.10.0" && names[1] == "0.3.0");

    Json j;
    std::string err;
    CHECK(parseJson(R"({"version":"0.3.0","url":"/downloads/EmulkonDriver_0.3.0_win64.dll","size":12,)"
                    R"("sha256":"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"})", j, err));
    UpdateOffer o = parseUpdateOffer(&j);
    CHECK(o.valid() && o.size == 12 && o.sha256 == sha256Hex("abc"));
    o.url = "/downloads/../secret";
    CHECK(!o.valid());
    CHECK(!parseUpdateOffer(nullptr).valid());
    // Архитектура — по компилятору (issue #12): сборка ARM64 не должна назваться x86_64.
#if defined(__aarch64__) || defined(_M_ARM64)
    CHECK(std::string(driverArch()) == "aarch64");
#elif defined(__x86_64__) || defined(_M_X64)
    CHECK(std::string(driverArch()) == "x86_64");
#else
    CHECK(std::string(driverArch()) == "i386");
#endif
}

// FakeHttp — сервер обновлений в памяти: ответ /api/v1/driver/update и файл DLL.
class FakeHttp : public HttpClient {
public:
    std::string latestJson;  // объект latest или пусто
    bool available = false;
    std::string dll;
    std::vector<std::string> gets;

    HttpResponse post(const std::string&, const std::string&, const Headers&, int) override { return {}; }
    HttpResponse stream(const std::string&, const Headers&, int, StreamCancel&, const std::function<bool(const std::string&)>&) override {
        return {};
    }
    HttpResponse get(const std::string& url, const Headers&, int) override {
        gets.push_back(url);
        HttpResponse r;
        r.status = 200;
        if (url.find("/api/v1/driver/update?") != std::string::npos) {
            r.body = std::string("{\"current\":\"") + EMULKON_VERSION + "\",\"update_available\":" +
                     (available ? "true" : "false") + (latestJson.empty() ? "" : ",\"latest\":" + latestJson) + "}";
        } else if (url.find("/downloads/") != std::string::npos) {
            r.body = dll;
        } else {
            r.status = 404;
        }
        return r;
    }
};

static void removeTree(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
}

// Кэш обновлений — на обеих ОС: файлы через платформенный слой (platform.h).
static void testUpdateCache() {
    const std::string root = sys::tempDir() + sys::kSep + "emulkon-unit-" + std::to_string(sys::processId());
    removeTree(root);
    sys::setEnv("EMULKON_DRIVER_CACHE", root.c_str());
    CHECK(updateCacheRoot() == root);
    CHECK(cachedVersions().empty() && rememberedServer().empty());

    rememberServer("http://kassa-srv:8737");
    CHECK(rememberedServer() == "http://kassa-srv:8737");

    // Старая версия в кэше — после установки новой её файл удаляется.
    const std::string oldDir = root + sys::kSep + "0.0.1";
    CHECK(sys::ensureDir(oldDir));
    CHECK(sys::writeFile(cachedDllPath("0.0.1"), "old"));
    CHECK(cachedVersions().size() == 1);

    FakeHttp http;
    std::string message;
    CHECK(checkForUpdate(http, "http://srv/", 1000, "WS", message) == UpdateCheck::UpToDate);
    CHECK(message.find("последняя версия") != std::string::npos && message.find("не опубликованы") != std::string::npos);
    CHECK(http.gets.size() == 1 && http.gets[0].find(std::string("&os=") + driverOS()) != std::string::npos);

#ifdef _WIN32
    http.dll = std::string("MZ") + std::string(300, '\x90');
    const std::string file = "_win64.dll";
#else
    http.dll = std::string("\x7f" "ELF") + std::string(300, '\0');
    const std::string file = "_lin64.so";
#endif
    const std::string next = "99.0.0";
    http.available = true;
    http.latestJson = "{\"version\":\"" + next + "\",\"url\":\"/downloads/EmulkonDriver_" + next + file +
                      "\",\"size\":" + std::to_string(http.dll.size()) + ",\"sha256\":\"" + sha256Hex(http.dll) + "\"}";
    CHECK(checkForUpdate(http, "http://srv/", 1000, "WS", message) == UpdateCheck::Downloaded);
    CHECK(message.find("Загружена версия драйвера 99.0.0") != std::string::npos);
    CHECK(http.gets.size() == 3 && http.gets[2] == "http://srv/downloads/EmulkonDriver_99.0.0" + file);
    auto versions = cachedVersions();
    CHECK(versions.size() == 1 && versions[0] == next);  // 0.0.1 вычищена
    CHECK(!sys::isDir(oldDir));

    CHECK(checkForUpdate(http, "http://srv", 1000, "WS", message) == UpdateCheck::Pending);
    CHECK(http.gets.size() == 4);  // второй раз файл не качаем

    // Подмена файла по дороге: контрольная сумма не сходится — в кэш ничего не попадает.
    http.latestJson = "{\"version\":\"99.0.1\",\"url\":\"/downloads/EmulkonDriver_99.0.1" + file + "\",\"sha256\":\"" +
                      sha256Hex("другое") + "\"}";
    CHECK(checkForUpdate(http, "http://srv", 1000, "WS", message) == UpdateCheck::Failed);
    CHECK(message.find("контрольная сумма") != std::string::npos);
    CHECK(cachedVersions().size() == 1);

    // Файл не той ОС (DLL на Linux, ELF на Windows) не ставится, даже с верной суммой.
#ifdef _WIN32
    const std::string alien = std::string("\x7f" "ELF") + std::string(100, '\0');
#else
    const std::string alien = std::string("MZ") + std::string(100, '\x90');
#endif
    http.dll = alien;
    http.latestJson = "{\"version\":\"99.0.2\",\"url\":\"/downloads/EmulkonDriver_99.0.2" + file + "\",\"sha256\":\"" +
                      sha256Hex(alien) + "\"}";
    CHECK(checkForUpdate(http, "http://srv", 1000, "WS", message) == UpdateCheck::Failed);
    CHECK(message.find("не библиотека драйвера") != std::string::npos);

    removeTree(root);
    sys::setEnv("EMULKON_DRIVER_CACHE", nullptr);
}
#ifndef _WIN32
// Маленький HTTP-сервер на loopback для проверки клиента на сокетах (http_posix.cpp): каждый
// запрос — один заранее заданный сырой ответ; запрос запоминается.
struct LoopbackServer {
    int listenFd = -1;
    int port = 0;
    std::vector<std::string> requests;
    std::thread thread;

    explicit LoopbackServer(std::vector<std::string> responses) {
        listenFd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listenFd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        getsockname(listenFd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        listen(listenFd, 8);
        thread = std::thread([this, responses]() {
            for (const std::string& resp : responses) {
                int c = accept(listenFd, nullptr, nullptr);
                if (c < 0) return;
                std::string req;
                char buf[4096];
                // Заголовки и тело (по Content-Length) — целиком.
                for (;;) {
                    ssize_t n = recv(c, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    req.append(buf, static_cast<size_t>(n));
                    size_t end = req.find("\r\n\r\n");
                    if (end == std::string::npos) continue;
                    size_t cl = req.find("Content-Length: ");
                    size_t need = cl == std::string::npos ? 0 : std::stoul(req.substr(cl + 16));
                    if (req.size() >= end + 4 + need) break;
                }
                requests.push_back(req);
                if (resp == "SLOW") {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                } else {
                    send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
                }
                close(c);
            }
        });
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
    ~LoopbackServer() {
        shutdown(listenFd, SHUT_RDWR);
        close(listenFd);
        if (thread.joinable()) thread.join();
    }
};

static void testHttpClient() {
    auto http = makeHttpClient("unit");
    {
        LoopbackServer srv({"HTTP/1.1 201 Created\r\nContent-Length: 11\r\nContent-Type: application/json\r\n\r\n{\"ok\":true}",
                            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nЖу\r\n9\r\nрнал!\r\n0\r\n\r\n",
                            "HTTP/1.1 404 Not Found\r\n\r\nнет такого",
                            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nda"});
        HttpResponse r = http->post(srv.url("/api/v1/sessions?x=1"), "{\"a\":1}", {{"X-Emulkon-Session", "s1"}}, 3000);
        CHECK(r.error.empty() && r.status == 201 && r.body == "{\"ok\":true}");
        r = http->get(srv.url("/chunked"), {}, 3000);
        CHECK(r.error.empty() && r.status == 200 && r.body == "Журнал!");
        r = http->remove(srv.url("/gone"), {}, 3000);
        CHECK(r.error.empty() && r.status == 404 && r.body == "нет такого");  // тело до закрытия соединения
        r = http->get(srv.url("/short"), {}, 3000);
        CHECK(!r.error.empty() && r.error.find("оборван") != std::string::npos);
        const std::string& first = srv.requests.at(0);
        CHECK(first.rfind("POST /api/v1/sessions?x=1 HTTP/1.1\r\n", 0) == 0);
        CHECK(first.find("\r\nHost: 127.0.0.1:" + std::to_string(srv.port) + "\r\n") != std::string::npos);
        CHECK(first.find("\r\nX-Emulkon-Session: s1\r\n") != std::string::npos);
        CHECK(first.find("\r\nContent-Length: 7\r\n") != std::string::npos && first.size() >= 7 &&
              first.compare(first.size() - 7, 7, "{\"a\":1}") == 0);
        CHECK(srv.requests.at(2).rfind("DELETE /gone HTTP/1.1", 0) == 0);
    }
    {
        // Поток событий: куски приходят в onData; отмена из другого потока прерывает чтение.
        LoopbackServer srv({"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\ndata: 1\n\n", "SLOW"});
        StreamCancel cancel;
        std::string got;
        HttpResponse r = http->stream(srv.url("/events"), {}, 2000, cancel, [&](const std::string& c) {
            got += c;
            return true;
        });
        CHECK(r.status == 200 && got == "data: 1\n\n" && r.error.empty());
        CHECK(srv.requests.at(0).find("\r\nAccept: text/event-stream\r\n") != std::string::npos);
        StreamCancel cancel2;
        std::thread stopper([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            cancel2.cancel();
        });
        auto t0 = std::chrono::steady_clock::now();
        r = http->stream(srv.url("/events"), {}, 5000, cancel2, [](const std::string&) { return true; });
        stopper.join();
        CHECK(r.error == "отменено");
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1400));  // не ждали таймаута
    }
    {
        LoopbackServer srv({"SLOW"});
        HttpResponse r = http->get(srv.url("/slow"), {}, 300);
        CHECK(r.timeout && r.error.find("таймаут") != std::string::npos);
    }
    HttpResponse r = http->get("https://127.0.0.1:1/x", {}, 1000);
    CHECK(r.error.find("https") != std::string::npos);
    r = http->get("http://127.0.0.1:1/x", {}, 1000);  // порт закрыт
    CHECK(!r.error.empty() && r.status == 0);
    r = http->get("http://", {}, 1000);
    CHECK(r.error.find("неверный адрес") != std::string::npos);
}
#else
static void testHttpClient() {}  // WinHTTP проверяют стенд (harness) и живые тесты
#endif

int main() {
    testJson();
    testJsonLocale();
    testNoConnectionHint();
    testText();
    testInfobase();
    testMethods();
    testRequestId();
    testSse();
    testUpdateLogic();
    testUpdateCache();
    testHttpClient();
    std::printf("unit: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
