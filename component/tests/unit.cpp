// Модульные тесты ядра компоненты: JSON, текст, параметры подключения, таблица методов.
#include <cstdio>
#include <set>
#include <string>

#include "device_client.h"
#include "events.h"
#include "json.h"
#include "methods.h"
#include "text.h"
#include "update.h"

#ifdef _WIN32
#include <windows.h>
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
    CHECK(std::string(driverArch()) == (sizeof(void*) == 8 ? "x86_64" : "i386"));
}

#ifdef _WIN32
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

static void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(path);
            else DeleteFileW(path.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static void testUpdateCache() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = std::wstring(tmp) + L"emulkon-unit-" + std::to_wstring(GetCurrentProcessId());
    removeTree(root);
    SetEnvironmentVariableW(L"EMULKON_DRIVER_CACHE", root.c_str());
    CHECK(updateCacheRoot() == root);
    CHECK(cachedVersions().empty() && rememberedServer().empty());

    rememberServer("http://kassa-srv:8737");
    CHECK(rememberedServer() == "http://kassa-srv:8737");

    // Старая версия в кэше — после установки новой её DLL удаляется.
    std::wstring oldDir = root + L"\\0.0.1";
    CreateDirectoryW(oldDir.c_str(), nullptr);
    HANDLE f = CreateFileW(cachedDllPath("0.0.1").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(f != INVALID_HANDLE_VALUE);
    CloseHandle(f);
    CHECK(cachedVersions().size() == 1);

    FakeHttp http;
    std::string message;
    CHECK(checkForUpdate(http, "http://srv/", 1000, "WS", message) == UpdateCheck::UpToDate);
    CHECK(message.find("последняя версия") != std::string::npos && message.find("не опубликованы") != std::string::npos);

    http.dll = std::string("MZ") + std::string(300, '\x90');
    const std::string next = "99.0.0";
    http.available = true;
    http.latestJson = "{\"version\":\"" + next + "\",\"url\":\"/downloads/EmulkonDriver_" + next +
                      "_win64.dll\",\"size\":" + std::to_string(http.dll.size()) + ",\"sha256\":\"" + sha256Hex(http.dll) + "\"}";
    CHECK(checkForUpdate(http, "http://srv/", 1000, "WS", message) == UpdateCheck::Downloaded);
    CHECK(message.find("Загружена версия драйвера 99.0.0") != std::string::npos);
    CHECK(http.gets.size() == 3 && http.gets[2] == "http://srv/downloads/EmulkonDriver_99.0.0_win64.dll");
    auto versions = cachedVersions();
    CHECK(versions.size() == 1 && versions[0] == next);  // 0.0.1 вычищена
    CHECK(GetFileAttributesW(oldDir.c_str()) == INVALID_FILE_ATTRIBUTES);

    CHECK(checkForUpdate(http, "http://srv", 1000, "WS", message) == UpdateCheck::Pending);
    CHECK(http.gets.size() == 4);  // второй раз файл не качаем

    // Подмена файла по дороге: контрольная сумма не сходится — в кэш ничего не попадает.
    http.latestJson = "{\"version\":\"99.0.1\",\"url\":\"/downloads/EmulkonDriver_99.0.1_win64.dll\",\"sha256\":\"" +
                      sha256Hex("другое") + "\"}";
    CHECK(checkForUpdate(http, "http://srv", 1000, "WS", message) == UpdateCheck::Failed);
    CHECK(message.find("контрольная сумма") != std::string::npos);
    CHECK(cachedVersions().size() == 1);

    removeTree(root);
    SetEnvironmentVariableW(L"EMULKON_DRIVER_CACHE", nullptr);
}
#endif
int main() {
    testJson();
    testText();
    testInfobase();
    testMethods();
    testRequestId();
    testSse();
    testUpdateLogic();
#ifdef _WIN32
    testUpdateCache();
#endif
    std::printf("unit: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
