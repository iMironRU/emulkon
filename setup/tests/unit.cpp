// Модульные тесты установщика: пути и разбор строк (без установки в систему).
#include <cstdio>
#include <string>

#include "setup.h"

using namespace setup;

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

static void testPaths() {
    CHECK(samePath(L"C:\\Users\\a\\AppData\\Local\\Programs\\Emulkon", L"c:/users/A/appdata/local/programs/emulkon\\"));
    CHECK(!samePath(L"", L""));
    CHECK(isInside(L"C:\\P\\Emulkon\\emulkon.exe", L"c:\\p\\emulkon"));
    CHECK(isInside(L"C:\\P\\Emulkon\\downloads\\x.dll", L"C:\\P\\Emulkon\\"));
    CHECK(!isInside(L"C:\\P\\EmulkonOld\\emulkon.exe", L"C:\\P\\Emulkon"));  // соседний каталог с тем же началом
    CHECK(!isInside(L"C:\\P\\Emulkon", L"C:\\P\\Emulkon"));
    CHECK(parentDir(L"C:\\a\\b.exe") == L"C:\\a" && fileName(L"C:\\a\\b.exe") == L"b.exe");

    CHECK(defaultInstallDir().size() > 10 && fileName(defaultInstallDir()) == L"Emulkon");
    CHECK(fileName(userDataDir()) == L"Emulkon" && fileName(userConfigDir()) == L"Emulkon");
    CHECK(fileName(startMenuDir()) == L"Эмулкон" && fileName(userStartMenuDir()) == L"Эмулкон");
    CHECK(!samePath(startMenuDir(), userStartMenuDir()));  // общее меню «Пуск», не пользователя
    CHECK(fileName(machineDataDir()) == L"Emulkon" && !samePath(machineDataDir(), userDataDir()));

    // Раскладка установки (docs/local-install.md §2).
    const std::wstring d = L"C:\\Program Files\\Emulkon";
    CHECK(binDir(d) == d + L"\\bin" && serverExe(d) == d + L"\\bin\\emulkon.exe" && trayExe(d) == d + L"\\bin\\EmulkonTray.exe");
    CHECK(driverDir(d) == d + L"\\1C\\driver" && epfDir(d) == d + L"\\1C\\epf" && setupDir(d) == d + L"\\setup");
    CHECK(isInside(serverExe(d), d) && isInside(driverDir(d), d));
    CHECK(snapinDll(d) == d + L"\\console\\EmulkonSnapIn.dll" && consoleFile(d) == d + L"\\console\\Emulkon.msc");

    const std::wstring v = version();
    CHECK(driverPackageName() == L"EmulkonDriver_" + v + L".zip");
    CHECK(driverDllName(true) == L"EmulkonDriver_" + v + L"_win64.dll");
    CHECK(driverDllName(false) == L"EmulkonDriver_" + v + L"_win32.dll");
    CHECK(setupDownloadName() == L"EmulkonSetup_" + v + L".exe");
}

static void testStrings() {
    CHECK(isLocalUrl(L"http://127.0.0.1:8737"));
    CHECK(isLocalUrl(L"HTTP://LOCALHOST:8737/"));
    CHECK(!isLocalUrl(L"http://kassa-srv:8737"));
    CHECK(!isLocalUrl(L"http://127.0.0.10:8737"));

    CHECK(isPlatformVersion(L"8.3.27.1606"));
    CHECK(!isPlatformVersion(L"8.3.27"));
    CHECK(!isPlatformVersion(L"common"));
    CHECK(!isPlatformVersion(L"8.3..1"));
    CHECK(!isPlatformVersion(L"9.0.0.1"));

    Tally t;
    t.add(Status::Ok);
    t.add(Status::Ok);
    t.add(Status::Warn);
    t.add(Status::Fail);
    CHECK(t.ok == 2 && t.warn == 1 && t.fail == 1 && t.info == 0);
    CHECK(std::wstring(statusText(Status::Fail)).find(L"ошибка") != std::wstring::npos);
}

int main() {
    testPaths();
    testStrings();
    std::printf("setup_unit: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
