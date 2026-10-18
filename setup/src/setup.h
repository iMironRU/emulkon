// EmulkonSetup — установщик Эмулкона для Windows (docs/local-install.md).
//
// Ставит на машину, с правами администратора: программу в Program Files (bin, 1C\driver, 1C\epf,
// setup), сервер — службой Windows «Emulkon» под LocalService с данными в ProgramData, трей —
// каждому пользователю. Регистрирует программу в «Установленных приложениях», удаляет её и
// проверяет, что всё может работать: до установки (ОС, каталог, порт, 1С) и после (служба,
// сервер, страница загрузок, драйвер на самом деле подключается). Прежнюю установку в профиле
// пользователя (до 0.8.0) переносит: данные — в ProgramData, сама она удаляется.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "doc.h"  // tray::widen, tray::narrow — UTF-8 ↔ UTF-16

#ifndef EMULKON_VERSION
#define EMULKON_VERSION "0.0.0-dev"
#endif

namespace setup {

using tray::narrow;
using tray::widen;

// Ключи реестра. Установка на машину — в HKLM; прежняя установка в профиле — те же пути в HKCU.
const wchar_t* const kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Emulkon";
const wchar_t* const kAppKey = L"Software\\Emulkon";
const wchar_t* const kServerKey = L"Software\\Emulkon\\Server";  // настройки службы (docs/local-install.md §2.1)
const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* const kRunValue = L"EmulkonTray";  // так же пишет трей («Запускать при входе в Windows»)
const wchar_t* const kServiceName = L"Emulkon";
const wchar_t* const kServerUrl = L"http://127.0.0.1:8737";
const int kPort = 8737;

std::wstring version();  // EMULKON_VERSION

// --- отчёт ---

enum class Status { Ok, Info, Warn, Fail };

struct Item {
    Status status = Status::Info;
    std::wstring title;   // что проверяли или делали
    std::wstring detail;  // итог и что делать
};

// Sink получает строки отчёта по мере работы (из рабочего потока).
using Sink = std::function<void(const Item&)>;

struct Tally {
    int ok = 0, info = 0, warn = 0, fail = 0;
    void add(Status s);
};

const wchar_t* statusText(Status s);  // «готово», «внимание», «ошибка», «сведения»

// --- параметры ---

struct Options {
    std::wstring dir;       // каталог установки; пусто — по умолчанию или установленный
    bool autostart = true;  // трей при входе в Windows
    bool network = false;   // сервер принимает подключения с других компьютеров
    bool run = true;        // запустить трей (он поднимет сервер) после установки
    bool purge = false;     // удаление: и данные пользователя
};

struct Installed {
    bool present = false;
    std::wstring dir;
    std::wstring version;
};

// --- paths.cpp ---

std::wstring envVar(const wchar_t* name);
std::wstring defaultInstallDir();  // %ProgramFiles%\Emulkon
std::wstring machineDataDir();     // %ProgramData%\Emulkon: data, logs, backup
std::wstring userDataDir();        // %LOCALAPPDATA%\Emulkon: кэш драйвера, лог трея (у прежней установки — и данные сервера)
std::wstring userConfigDir();      // %APPDATA%\Emulkon: tray.ini
std::wstring startMenuDir();       // общее меню «Пуск»: …\ProgramData\…\Programs\Эмулкон
std::wstring userStartMenuDir();   // меню «Пуск» пользователя — там ярлыки прежней установки
// Раскладка каталога установки (docs/local-install.md §2).
std::wstring binDir(const std::wstring& dir);     // bin: emulkon.exe, EmulkonTray.exe, EmulkonSetup.exe
std::wstring driverDir(const std::wstring& dir);  // 1C\driver: пакет драйвера и DLL для самообновления
std::wstring epfDir(const std::wstring& dir);     // 1C\epf: обработки для 1С
std::wstring setupDir(const std::wstring& dir);   // setup: установщик для соседних компьютеров
std::wstring consoleDir(const std::wstring& dir); // console: оснастка MMC и готовая консоль
std::wstring snapinDll(const std::wstring& dir);  // console\EmulkonSnapIn.dll
std::wstring consoleFile(const std::wstring& dir); // console\Emulkon.msc
std::wstring serverExe(const std::wstring& dir);  // bin\emulkon.exe
std::wstring trayExe(const std::wstring& dir);    // bin\EmulkonTray.exe
std::wstring selfPath();
std::wstring parentDir(const std::wstring& path);
std::wstring fileName(const std::wstring& path);
bool samePath(const std::wstring& a, const std::wstring& b);
bool isInside(const std::wstring& path, const std::wstring& dir);  // path внутри dir (без учёта регистра)
bool fileExists(const std::wstring& path);
bool dirExists(const std::wstring& path);
std::wstring driverPackageName();               // EmulkonDriver_<версия>.zip
std::wstring driverDllName(bool x64);           // EmulkonDriver_<версия>_win64.dll / _win32.dll
std::wstring setupDownloadName();               // EmulkonSetup_<версия>.exe
bool isLocalUrl(const std::wstring& url);       // http://127.0.0.1:…, http://localhost:…
bool isPlatformVersion(const std::wstring& s);  // 8.3.27.1606 — каталог версии 1С

// --- service.cpp: служба, журнал событий, брандмауэр, права ---

enum class ServiceState { Missing, Stopped, Starting, Running, Stopping, Unknown };
ServiceState serviceState();
std::wstring serviceStateText(ServiceState s);
// installService создаёт или перенастраивает службу: emulkon.exe -service, автозапуск, LocalService,
// перезапуск при сбое, пользователи могут запускать и останавливать (docs/local-install.md §1.9).
bool installService(const std::wstring& exe, std::wstring& error);
bool startService(std::wstring& error);         // ждёт «работает» до 30 с
bool stopService(std::wstring& error);          // ждёт «остановлена» до 30 с; нет службы — успех
bool deleteService(std::wstring& error);
bool registerEventSource(std::wstring& error);  // журнал «Приложение», источник Emulkon
void unregisterEventSource();
bool setFirewallRule(bool on, const std::wstring& exe, std::wstring& error);  // входящие TCP 8737 для emulkon.exe
// secureMachineData — права на %ProgramData%\Emulkon: служба (LocalService) и администраторы пишут,
// пользователи читают; secureServerKey — то же для HKLM\Software\Emulkon\Server (пользователи не читают).
bool secureMachineData(const std::wstring& dir, std::wstring& error);
bool secureServerKey(std::wstring& error);
// Элемент Панели управления «Эмулкон» (docs/local-install.md §4): командный объект в реестре, без DLL;
// открывает command (сейчас — веб-интерфейс, позже — оснастку MMC).
bool registerControlPanel(const std::wstring& icon, const std::wstring& command, std::wstring& error);
void unregisterControlPanel();

// --- console.cpp: оснастка MMC (docs/local-install.md §4–§5) ---

// registerSnapIn вызывает DllRegisterServer оснастки: COM-класс и запись в HKLM\Software\Microsoft\MMC.
bool registerSnapIn(const std::wstring& dll, std::wstring& error);
void unregisterSnapIn();
// createConsoleFile пишет Emulkon.msc: корень — узел «Эмулкон», режим пользователя с одним окном.
bool createConsoleFile(const std::wstring& msc, std::wstring& error);

// --- install.cpp ---

Installed findInstalled();        // установка на машину (HKLM)
Installed findLegacyInstalled();  // прежняя установка в профиле пользователя (HKCU, до 0.8.0)
// currentOptions — параметры установленного Эмулкона: каталог, автозапуск трея, сервер для касс в сети.
Options currentOptions();
bool install(const Options& o, const Sink& sink);  // установка, обновление; «Восстановить» — она же поверх
// modify — мастер «Изменить» (docs/local-install.md §3): автозапуск трея, сервер для касс в сети и правило
// брандмауэра; файлы не трогает, службу перезапускает, если сменился адрес.
bool modify(const Options& o, const Sink& sink);
bool uninstall(const Options& o, const Sink& sink);
// stopInstance закрывает трей, серверы и окна установщика, запущенные из dir, —
// перед обновлением и удалением.
void stopInstance(const std::wstring& dir, const Sink& sink);
// scheduleSelfDelete — удалить свой exe и каталог dir (если он опустел) при перезагрузке Windows:
// запущенный exe Windows удалить не даёт; штатный способ — MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT).
void scheduleSelfDelete(const std::wstring& dir);

// --- checks.cpp ---

// preChecks — можно ли ставить: ОС, каталог, место, порт, 1С.
Tally preChecks(const Options& o, const Sink& sink);
// workChecks — работает ли установленное: файлы, трей, сервер, загрузки, драйвер.
Tally workChecks(const std::wstring& dir, const Sink& sink);

// --- probe.cpp ---

// probeDriver загружает DLL драйвера как платформа 1С и выполняет «Тест устройства»
// (EquipmentTest) на сервере serverUrl. Это сквозная проверка: DLL, WinHTTP, Device API.
Item probeDriver(const std::wstring& dll, const std::wstring& serverUrl);

}  // namespace setup
