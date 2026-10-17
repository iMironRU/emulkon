// EmulkonTray — значок сервера Эмулкон в области уведомлений Windows:
// всплывающее окно с каждым «напечатанным» чеком, печать на принтер Windows,
// состояние и перезапуск службы Emulkon (установка на машину, docs/local-install.md); без службы
// (запуск из каталога разработки) — запуск локального сервера. Чеки берёт из журнала сервера
// (device.print, device.reprint).
// Показывать ли чек и печатать ли его, решает сервер по настройке устройства — она в
// карточке устройства веб-интерфейса (ARCHITECTURE §4.3). В трее — только настройки трея и сервера.
//
//   EmulkonTray.exe                         обычный запуск (один экземпляр)
//   EmulkonTray.exe --config <ini>          другой файл настроек
//   EmulkonTray.exe --exit                  закрыть запущенный трей (ждёт, пока он закроется)
//   EmulkonTray.exe --render <out.bmp>      самопроверка: нарисовать окно с образцом чека
//   EmulkonTray.exe --render-terminal <out.bmp> самопроверка: окно терминала «приложите карту»
//   EmulkonTray.exe --render-scales <out.bmp>   самопроверка: окно весов «положите товар»
//   EmulkonTray.exe --print <принтер> <файл> самопроверка: напечатать образец (PDF/XPS — в файл)
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "json.h"
#include "doc.h"
#include "stream.h"
#include "ui.h"
#include "updates.h"

using namespace tray;

namespace {

const wchar_t* kMainClass = L"EmulkonTrayMain";
const UINT WM_APP_TRAY = WM_APP + 1;     // события значка
const UINT WM_APP_DOC = WM_APP + 2;      // lParam = PrintedDoc*, wParam = live
const UINT WM_APP_STATUS = WM_APP + 3;   // wParam = connected, lParam = std::wstring*
const UINT WM_APP_PRINTED = WM_APP + 4;  // wParam = ok, lParam = std::wstring* (ошибка)
const UINT WM_APP_PROMPT = WM_APP + 5;   // lParam = PromptEvent* — экран терминала
const UINT WM_APP_ANSWER = WM_APP + 6;   // wParam = ok, lParam = std::wstring* — ответ с экрана отправлен
const UINT WM_APP_LOG = WM_APP + 7;      // lParam = std::wstring* — строка лога из фонового потока
const UINT WM_APP_SERVICE = WM_APP + 8;  // lParam = ServiceResult* — запуск или остановка службы закончены
const UINT WM_APP_UPDATE = WM_APP + 9;   // lParam = UpdateInfo* — ответ сервера о новой версии (issue #8)
const UINT_PTR kTimerUpdates = 1;         // проверка новой версии: через 30 с после запуска, затем раз в час

enum Cmd : UINT {
    kCmdShowLast = 100,
    kCmdOpenWeb,
    kCmdDeviceOutput,
    kCmdStartServer,
    kCmdStopServer,
    kCmdSettings,
    kCmdAutostart,
    kCmdServerAutostart,
    kCmdDownloads,
    kCmdCheck,
    kCmdTerminal,
    kCmdExit,
    kCmdServiceRestart,
    kCmdServiceStart,
    kCmdServiceStop,
    kCmdMaintain,
    kCmdUpdate,
};

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    std::wstring configPath;
    Config cfg;
    JournalStream stream;
    ReceiptPopup popup;
    TerminalWindow terminal;
    NOTIFYICONDATAW nid{};
    HICON iconOn = nullptr, iconOff = nullptr;
    bool connected = false;
    std::wstring status = L"подключение…";
    PrintedDoc last;
    bool hasLast = false;
    PROCESS_INFORMATION server{};        // локальный сервер, запущенный из трея
    UINT taskbarCreated = 0;
    FILE* log = nullptr;
    bool serverAutostartTried = false;   // автозапуск сервера — один раз за запуск трея
    bool serviceBusy = false;            // идёт запуск или остановка службы
    bool everConnected = false;          // трей уже был на связи с сервером
    UpdateInfo update;                   // новая версия по ответу сервера (GET /api/v1/updates)
};

App g;

void logLine(const std::wstring& text) {
    if (!g.log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g.log, "%04u-%02u-%02u %02u:%02u:%02u %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                 narrow(text).c_str());
    std::fflush(g.log);
}

void openLog() {
    wchar_t buf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return;
    std::wstring path = std::wstring(buf) + L"\\Emulkon";
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\tray.log";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (1u << 20);
    g.log = _wfopen(path.c_str(), big ? L"w" : L"a");  // больше 1 МБ — начать заново
}

bool serverRunning() {
    if (!g.server.hProcess) return false;
    DWORD code = 0;
    return GetExitCodeProcess(g.server.hProcess, &code) && code == STILL_ACTIVE;
}

// --- служба Emulkon (установка на машину) ---
// Пользователи могут запускать и останавливать её без UAC (права на службу ставит установщик, §1.9).

enum class Svc { Missing, Stopped, Starting, Running, Stopping, Unknown };

Svc serviceState() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return Svc::Unknown;
    SC_HANDLE svc = OpenServiceW(scm, L"Emulkon", SERVICE_QUERY_STATUS);
    Svc out = Svc::Unknown;
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) out = Svc::Missing;
    } else {
        SERVICE_STATUS st{};
        if (QueryServiceStatus(svc, &st)) {
            switch (st.dwCurrentState) {
                case SERVICE_STOPPED: out = Svc::Stopped; break;
                case SERVICE_START_PENDING: out = Svc::Starting; break;
                case SERVICE_RUNNING: out = Svc::Running; break;
                case SERVICE_STOP_PENDING: out = Svc::Stopping; break;
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return out;
}

const wchar_t* serviceText(Svc s) {
    switch (s) {
        case Svc::Missing: return L"не установлена";
        case Svc::Stopped: return L"остановлена";
        case Svc::Starting: return L"запускается";
        case Svc::Running: return L"работает";
        case Svc::Stopping: return L"останавливается";
        case Svc::Unknown: break;
    }
    return L"состояние неизвестно";
}

std::wstring sysError(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s + L" (код " + std::to_wstring(code) + L")";
}

bool waitService(SC_HANDLE svc, DWORD want, DWORD ms) {
    for (DWORD waited = 0;; waited += 250) {
        SERVICE_STATUS st{};
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == want) return true;
        if (waited >= ms) return false;
        Sleep(250);
    }
}

// controlService — остановить (stop), запустить (start) или то и другое; ошибка — текст, пусто — успех.
std::wstring controlService(bool stop, bool start) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return L"диспетчер служб: " + sysError(GetLastError());
    SC_HANDLE svc = OpenServiceW(scm, L"Emulkon", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    std::wstring err;
    if (!svc) {
        err = L"служба Emulkon: " + sysError(GetLastError());
    } else {
        SERVICE_STATUS st{};
        if (stop && !ControlService(svc, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
            err = L"остановить: " + sysError(GetLastError());
        } else if (stop && !waitService(svc, SERVICE_STOPPED, 30000)) {
            err = L"служба не остановилась за 30 с";
        } else if (start && !StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            err = L"запустить: " + sysError(GetLastError());
        } else if (start && !waitService(svc, SERVICE_RUNNING, 30000)) {
            err = L"служба не запустилась за 30 с — причина в журнале событий Windows (источник Emulkon)";
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return err;
}

struct ServiceResult {
    std::wstring done;   // что сделано — для лога и уведомления
    std::wstring error;  // пусто — успех
};

// serviceAsync — запуск, остановка или перезапуск службы в фоне: ожидание до 30 с не должно вешать меню.
void serviceAsync(bool stop, bool start) {
    if (g.serviceBusy) return;
    g.serviceBusy = true;
    std::wstring done = stop && start ? L"сервер перезапущен" : start ? L"сервер запущен" : L"сервер остановлен";
    logLine(L"служба Emulkon: " + std::wstring(stop && start ? L"перезапуск…" : start ? L"запуск…" : L"остановка…"));
    HWND hwnd = g.hwnd;
    std::thread([hwnd, stop, start, done] {
        auto* r = new ServiceResult{done, controlService(stop, start)};
        PostMessageW(hwnd, WM_APP_SERVICE, 0, reinterpret_cast<LPARAM>(r));
    }).detach();
}

// machineAutostart — автозапуск трея для всех пользователей (HKLM\…\Run, ставит установщик).
bool machineAutostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    bool on = RegQueryValueExW(key, L"EmulkonTray", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return on;
}

void updateIcon() {
    g.nid.hIcon = g.connected ? g.iconOn : g.iconOff;
    std::wstring tip = L"Эмулкон: " + (g.connected ? std::wstring(L"на связи") : g.status) + L"\n" +
                       effectiveWorkstation(g.cfg) + L" · " + g.cfg.serverUrl;
    wcsncpy_s(g.nid.szTip, tip.c_str(), _TRUNCATE);
    g.nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

void addIcon() {
    g.nid = NOTIFYICONDATAW{};
    g.nid.cbSize = sizeof g.nid;
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    g.nid.hIcon = g.connected ? g.iconOn : g.iconOff;
    wcsncpy_s(g.nid.szTip, L"Эмулкон", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g.nid);
    g.nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
    updateIcon();
}

void balloon(const std::wstring& title, const std::wstring& text) {
    g.nid.uFlags = NIF_INFO;
    wcsncpy_s(g.nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(g.nid.szInfo, text.c_str(), _TRUNCATE);
    g.nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

void startStream() {
    HWND hwnd = g.hwnd;
    g.stream.start(
        g.cfg.serverUrl, g.cfg.controlToken, effectiveWorkstation(g.cfg),
        [hwnd](const PrintedDoc& doc, bool live) {
            PostMessageW(hwnd, WM_APP_DOC, live ? 1 : 0, reinterpret_cast<LPARAM>(new PrintedDoc(doc)));
        },
        [hwnd](bool connected, const std::wstring& message) {
            PostMessageW(hwnd, WM_APP_STATUS, connected ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
        },
        [hwnd](const PromptEvent& ev) {
            PostMessageW(hwnd, WM_APP_PROMPT, 0, reinterpret_cast<LPARAM>(new PromptEvent(ev)));
        },
        [hwnd](const KeyboardEvent& ev) {
            // Сканер в режиме клавиатуры: пауза (переключиться в 1С), затем ввод в активное окно.
            std::thread([ev, hwnd] {
                if (ev.delayMs > 0) Sleep(static_cast<DWORD>(ev.delayMs));
                std::wstring error;
                bool ok = typeText(ev.text, ev.suffix, error);
                auto* line = new std::wstring(widen(ev.deviceId) +
                                              (ok ? L": введено с клавиатуры, символов " + std::to_wstring(ev.text.size())
                                                  : L": ввод с клавиатуры не прошёл — " + error));
                PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(line));
            }).detach();
        });
}

// deadlineIn — момент через seconds секунд в RFC 3339 (UTC): «осталось N с» в самопроверках.
std::string deadlineIn(int seconds) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += static_cast<ULONGLONG>(seconds) * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    char when[32];
    std::snprintf(when, sizeof when, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return when;
}

// answerPrompt — кнопка на экране устройства: ответ уходит на сервер в фоне.
void answerPrompt(const PromptEvent& p, const PromptReply& reply) {
    std::wstring server = g.cfg.serverUrl;
    while (!server.empty() && server.back() == L'/') server.pop_back();
    std::wstring url = server + L"/control/v1/devices/" + urlEncode(widen(p.deviceId)) + L"/prompt";
    std::string body = "{\"prompt_id\":\"" + p.promptId + "\",\"action\":\"" + reply.action + "\",\"card\":\"" + reply.card + "\"";
    std::string value;
    if (reply.hasValue) {  // граммами без плавающей точки: не зависит от локали
        long long g3 = static_cast<long long>(reply.value * 1000 + 0.5);
        char buf[48];
        std::snprintf(buf, sizeof buf, "%lld.%03lld", g3 / 1000, g3 % 1000);
        value = buf;
        body += ",\"value\":" + value;
    }
    body += "}";
    std::wstring token = g.cfg.controlToken;
    HWND hwnd = g.hwnd;
    bool scales = p.hasInput;
    logLine(widen(p.deviceId) + L": " + widen(reply.action) + (reply.card.empty() ? L"" : L" " + widen(reply.card)) +
            (value.empty() ? L"" : L" " + widen(value)));
    std::thread([url, body, token, hwnd, scales] {
        HttpResult r = httpPost(url, token, body, 10000);
        bool ok = r.error.empty() && r.status == 200;
        std::wstring msg = ok ? (scales ? L"Отправлено" : L"Отправлено, ждём ответ банка…")
                         : r.status == 409 ? L"Операция уже завершена"
                         : r.status == 401 ? L"Сервер требует токен Control API (Настройки…)"
                         : L"Не отправлено: " + (r.error.empty() ? L"HTTP " + std::to_wstring(r.status) : widen(r.error));
        PostMessageW(hwnd, WM_APP_ANSWER, ok ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(msg)));
    }).detach();
}

void printAsync(const PrintedDoc& doc) {
    std::wstring file;
    if (!g.cfg.printOutputDir.empty()) {
        CreateDirectoryW(g.cfg.printOutputDir.c_str(), nullptr);
        std::wstring printerName = doc.printer.empty() ? defaultPrinter() : doc.printer;
        std::wstring ext = printerName.find(L"XPS") != std::wstring::npos ? L".xps" : L".pdf";
        file = g.cfg.printOutputDir + L"\\" + widen(doc.deviceId) + L"-" + std::to_wstring(doc.journalSeq) + L"-" +
               widen(doc.kind) + ext;
    }
    HWND hwnd = g.hwnd;
    std::wstring printer = doc.printer;  // принтер выбран в карточке устройства
    std::thread([doc, printer, file, hwnd] {
        std::wstring error;
        bool ok = printDoc(doc, printer, file, error);
        PostMessageW(hwnd, WM_APP_PRINTED, ok ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(ok ? file : error)));
    }).detach();
}

// onDoc — документ с ленты устройства: показать и напечатать, как решил сервер (output записи).
void onDoc(const PrintedDoc& doc, bool live) {
    if (!doc.reprint) {
        g.last = doc;
        g.hasLast = true;
    }
    if (!live) return;  // дочитка после подключения — не показываем и не печатаем
    logLine(std::wstring(doc.reprint ? L"повторная печать: " : L"документ: ") + widen(doc.deviceId) + L" " +
            widen(doc.kind) + L" ФД " + std::to_wstring(doc.number) + L" (журнал " + std::to_wstring(doc.journalSeq) + L")");
    if (doc.showOnScreen) g.popup.show(doc, g.cfg.popupSeconds);
    if (doc.print) printAsync(doc);
}

// reportPrinters — принтеры этого компьютера серверу: из них карточка устройства предлагает
// выбор принтера (docs/protocol.md §2.3).
void reportPrinters() {
    emu::Json list = emu::Json::array();
    for (const std::wstring& p : listPrinters()) list.push(emu::Json::string(narrow(p)));
    emu::Json body = emu::Json::object();
    body.set("printers", list);
    body.set("default", emu::Json::string(narrow(defaultPrinter())));
    std::wstring server = g.cfg.serverUrl;
    while (!server.empty() && server.back() == L'/') server.pop_back();
    std::wstring ws = effectiveWorkstation(g.cfg);
    if (ws == L"*") return;  // трей смотрит все рабочие места — своих принтеров не сообщаем
    std::wstring url = server + L"/control/v1/workstations/" + urlEncode(ws) + L"/printers";
    std::wstring token = g.cfg.controlToken;
    std::string json = body.dump();
    std::thread([url, token, json] { httpPut(url, token, json, 10000); }).detach();
}

std::wstring defaultServerExe() {
    if (!g.cfg.serverExe.empty()) return g.cfg.serverExe;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    for (const std::wstring& p : {dir + L"\\emulkon.exe", std::wstring(L"C:\\.dev\\1С.БТО.Эмуляторы\\server\\bin\\emulkon.exe")}) {
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

void startServer() {
    std::wstring exe = defaultServerExe();
    if (exe.empty()) {
        balloon(L"Эмулкон", L"Не найден emulkon.exe — укажите путь в настройках");
        return;
    }
    // Адрес — ServerListen или из ServerURL: http://host:port → host:port.
    std::wstring addr = g.cfg.serverListen;
    if (addr.empty()) {
        addr = g.cfg.serverUrl;
        size_t scheme = addr.find(L"://");
        if (scheme != std::wstring::npos) addr = addr.substr(scheme + 3);
        while (!addr.empty() && addr.back() == L'/') addr.pop_back();
    }
    std::wstring data = g.cfg.serverData;
    if (data.empty()) {
        wchar_t buf[MAX_PATH];
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        data = std::wstring(buf) + L"\\Emulkon\\data";
    }
    std::wstring cmd = L"\"" + exe + L"\" -addr " + addr + L" -data \"" + data + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &g.server)) {
        balloon(L"Эмулкон", L"Не удалось запустить сервер: " + exe);
        g.server = PROCESS_INFORMATION{};
        return;
    }
    logLine(L"запущен сервер: " + cmd);
}

void stopServer() {
    if (!serverRunning()) return;
    TerminateProcess(g.server.hProcess, 0);  // журнал устойчив к обрыву строки
    WaitForSingleObject(g.server.hProcess, 3000);
    CloseHandle(g.server.hProcess);
    CloseHandle(g.server.hThread);
    g.server = PROCESS_INFORMATION{};
    logLine(L"сервер остановлен");
}

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool autostartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(key, L"EmulkonTray", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return on;
}

void toggleAutostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (autostartEnabled()) {
        RegDeleteValueW(key, L"EmulkonTray");
    } else {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(self) + L"\"";
        RegSetValueExW(key, L"EmulkonTray", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}

// setupExe — установщик рядом с треем: он же «Проверка работы» (/check) и удаление.
std::wstring setupExe() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p(self);
    p = p.substr(0, p.find_last_of(L'\\')) + L"\\EmulkonSetup.exe";
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : std::wstring();
}

void save() {
    if (!saveConfig(g.configPath, g.cfg)) balloon(L"Эмулкон", L"Не удалось сохранить настройки: " + g.configPath);
}

// --- новая версия (docs/local-install.md §2.3, issue #8) ---

// checkUpdatesAsync — GET /api/v1/updates в фоне; ответ — WM_APP_UPDATE. Сервер сам проверяет выпуски раз в сутки.
void checkUpdatesAsync() {
    std::wstring url = g.cfg.serverUrl;
    while (!url.empty() && url.back() == L'/') url.pop_back();
    url += L"/api/v1/updates";
    HWND hwnd = g.hwnd;
    std::thread([url, hwnd] {
        HttpResult r = httpGet(url, L"", 10000);
        auto* info = new UpdateInfo();
        if (r.status != 200 || !parseUpdateState(r.body, *info)) *info = UpdateInfo{};
        PostMessageW(hwnd, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(info));
    }).detach();
}

// onUpdate — новая версия: пункт меню; уведомление — один раз на версию (помнит настройка UpdateNotified).
void onUpdate(const UpdateInfo& info) {
    g.update = info;
    if (!info.available || info.version == g.cfg.updateNotified) return;
    balloon(L"Эмулкон: вышла версия " + info.version,
            info.installer.empty() || !isLocalServer(g.cfg.serverUrl)
                ? L"Что нового и установщик — на странице выпуска: «Обновить до " + info.version + L"…» в меню значка."
                : L"Установщик уже скачан: «Обновить до " + info.version + L"…» в меню значка.");
    g.cfg.updateNotified = info.version;
    save();
}

// runUpdate — запустить установщик, скачанный службой этого компьютера (мастер обновления, UAC), если он
// там, где служба его кладёт, и SHA-256 совпала с описанием выпуска; иначе — страница выпуска.
void runUpdate() {
    const UpdateInfo u = g.update;
    if (!u.available) return;
    wchar_t pd[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    if (isLocalServer(g.cfg.serverUrl) && !u.installer.empty()) {
        if (installerAllowed(u.installer, u.version, pd) && !u.sha256.empty() && fileSha256(u.installer) == u.sha256) {
            logLine(L"обновление: запуск " + u.installer);
            ShellExecuteW(nullptr, L"open", u.installer.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        logLine(L"обновление: установщик не прошёл проверку — " + u.installer);
        balloon(L"Эмулкон", L"Скачанный установщик не прошёл проверку (место или SHA-256) — открываю страницу выпуска.");
    }
    if (u.page.rfind(L"https://", 0) == 0) ShellExecuteW(nullptr, L"open", u.page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showMenu() {
    HMENU menu = CreatePopupMenu();
    std::wstring head = g.connected ? L"Эмулкон — на связи" : L"Эмулкон — " + g.status;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, head.c_str());
    std::wstring ws = L"Рабочее место: " + effectiveWorkstation(g.cfg) + L" · " + g.cfg.serverUrl;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, ws.c_str());
    if (g.update.available) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_DEFAULT, kCmdUpdate, (L"Обновить до " + g.update.version + L"…").c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g.hasLast ? 0 : MF_GRAYED), kCmdShowLast, L"Показать последний чек");
    AppendMenuW(menu, MF_STRING, kCmdOpenWeb, L"Открыть веб-интерфейс");
    AppendMenuW(menu, MF_STRING, kCmdDownloads, L"Загрузки: драйвер для 1С, установщик");
    // Показ и печать чеков — настройка устройства в его карточке, а не в трее (ARCHITECTURE §4.3).
    AppendMenuW(menu, MF_STRING, kCmdDeviceOutput, L"Показ и печать чеков — в карточке устройства…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const Svc svc = serviceState();
    if (svc != Svc::Missing) {
        // Установка на машину: сервер — служба Emulkon, трей ею управляет.
        std::wstring line = L"Служба Emulkon: " + std::wstring(g.serviceBusy ? L"выполняется команда…" : serviceText(svc));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
        UINT busy = g.serviceBusy ? MF_GRAYED : 0;
        AppendMenuW(menu, MF_STRING | busy, kCmdServiceRestart, L"Перезапустить сервер");
        if (svc == Svc::Stopped) {
            AppendMenuW(menu, MF_STRING | busy, kCmdServiceStart, L"Запустить сервер");
        } else {
            AppendMenuW(menu, MF_STRING | busy, kCmdServiceStop, L"Остановить сервер");
        }
    } else {
        // Без службы (запуск из каталога разработки) — локальный сервер, как раньше.
        if (serverRunning()) {
            AppendMenuW(menu, MF_STRING, kCmdStopServer, L"Остановить локальный сервер");
        } else {
            AppendMenuW(menu, MF_STRING | (g.connected ? MF_GRAYED : 0), kCmdStartServer, L"Запустить локальный сервер");
        }
        AppendMenuW(menu, MF_STRING | (g.cfg.serverAutostart ? MF_CHECKED : 0), kCmdServerAutostart,
                    L"Запускать локальный сервер вместе с треем");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"Настройки…");
    if (machineAutostart()) {
        // Для всех пользователей — HKLM, меняется в мастере «Изменить» (нужны права администратора).
        AppendMenuW(menu, MF_STRING | MF_CHECKED | MF_GRAYED, 0, L"Запускать при входе в Windows (для всех — «Обслуживание»)");
    } else {
        AppendMenuW(menu, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), kCmdAutostart, L"Запускать при входе в Windows");
    }
    if (!setupExe().empty()) {
        AppendMenuW(menu, MF_STRING, kCmdCheck, L"Проверка работы…");
        if (svc != Svc::Missing) AppendMenuW(menu, MF_STRING, kCmdMaintain, L"Обслуживание: изменить, восстановить, удалить…");
    }
    if (g.terminal.active()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdTerminal, L"Показать экран устройства (ждёт человека)");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.hwnd);  // иначе меню не закроется по щелчку мимо
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void command(UINT id) {
    switch (id) {
        case kCmdShowLast:
            if (g.hasLast) g.popup.show(g.last, g.cfg.popupSeconds);
            return;
        case kCmdOpenWeb:
            ShellExecuteW(nullptr, L"open", g.cfg.serverUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case kCmdDeviceOutput: {
            std::wstring url = g.cfg.serverUrl;
            while (!url.empty() && url.back() == L'/') url.pop_back();
            ShellExecuteW(nullptr, L"open", (url + L"/#devices").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        case kCmdStartServer:
            startServer();
            return;
        case kCmdStopServer:
            stopServer();
            return;
        case kCmdServiceRestart:
            serviceAsync(true, true);
            return;
        case kCmdServiceStart:
            serviceAsync(false, true);
            return;
        case kCmdServiceStop:
            serviceAsync(true, false);
            return;
        case kCmdUpdate:
            runUpdate();
            return;
        case kCmdMaintain: {
            std::wstring exe = setupExe();  // мастер попросит права администратора (UAC)
            if (!exe.empty()) ShellExecuteW(nullptr, L"open", exe.c_str(), L"/maintain", nullptr, SW_SHOWNORMAL);
            return;
        }
        case kCmdSettings: {
            Config edited = g.cfg;
            if (showSettings(g.inst, g.hwnd, edited)) {
                g.cfg = edited;
                save();
                g.connected = false;
                g.status = L"подключение…";
                updateIcon();
                startStream();
            }
            return;
        }
        case kCmdAutostart:
            toggleAutostart();
            return;
        case kCmdServerAutostart:
            g.cfg.serverAutostart = !g.cfg.serverAutostart;
            save();
            if (g.cfg.serverAutostart && !g.connected && !serverRunning()) startServer();
            return;
        case kCmdDownloads: {
            std::wstring url = g.cfg.serverUrl;
            while (!url.empty() && url.back() == L'/') url.pop_back();
            url += L"/downloads";
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        case kCmdTerminal:
            g.terminal.reopen();
            return;
        case kCmdCheck: {
            std::wstring exe = setupExe();
            if (!exe.empty()) ShellExecuteW(nullptr, L"open", exe.c_str(), L"/check", nullptr, SW_SHOWNORMAL);
            return;
        }
        case kCmdExit:
            DestroyWindow(g.hwnd);
            return;
    }
}

LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreated && g.taskbarCreated) {  // Проводник перезапущен — вернуть значок
        addIcon();
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (g.hasLast) g.popup.show(g.last, g.cfg.popupSeconds);
                    return 0;
                case WM_CONTEXTMENU:
                    showMenu();
                    return 0;
            }
            return 0;
        case WM_APP_DOC: {
            std::unique_ptr<PrintedDoc> doc(reinterpret_cast<PrintedDoc*>(lp));
            onDoc(*doc, wp != 0);
            return 0;
        }
        case WM_APP_STATUS: {
            std::unique_ptr<std::wstring> message(reinterpret_cast<std::wstring*>(lp));
            bool was = g.connected;
            g.connected = wp != 0;
            g.status = g.connected ? L"на связи" : L"нет связи (" + *message + L")";
            if (was != g.connected) logLine(g.connected ? L"на связи с " + g.cfg.serverUrl : L"связь потеряна: " + *message);
            if (g.connected && !was) reportPrinters();  // сервер мог перезапуститься — сообщаем заново
            if (g.connected) g.everConnected = true;
            // Сервер не отвечает сразу после запуска трея: служба Emulkon остановлена — запустить её; без
            // службы — поднять свой локальный сервер (настройка ServerAutostart). Если связь была и пропала —
            // сервер остановили намеренно (в меню трея, «Службы» Windows), его не поднимаем.
            if (!g.connected && !g.serverAutostartTried && !g.everConnected) {
                Svc svc = serviceState();
                if (svc == Svc::Stopped) {
                    g.serverAutostartTried = true;
                    serviceAsync(false, true);
                } else if (svc == Svc::Missing && g.cfg.serverAutostart && !serverRunning()) {
                    g.serverAutostartTried = true;
                    startServer();
                }
            }
            updateIcon();
            return 0;
        }
        case WM_APP_PROMPT: {
            std::unique_ptr<PromptEvent> ev(reinterpret_cast<PromptEvent*>(lp));
            if (ev->done) {
                logLine(widen(ev->deviceId) + L": " + ev->resultText);
                g.terminal.finish(*ev);
            } else {
                logLine(widen(ev->deviceId) + (ev->hasInput ? L" ждёт вес: " : L" ждёт карту: ") + ev->title);
                g.terminal.show(*ev);
            }
            return 0;
        }
        case WM_APP_LOG: {
            std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
            logLine(*line);
            return 0;
        }
        case WM_APP_SERVICE: {
            std::unique_ptr<ServiceResult> r(reinterpret_cast<ServiceResult*>(lp));
            g.serviceBusy = false;
            if (r->error.empty()) {
                logLine(L"служба Emulkon: " + r->done);
                balloon(L"Эмулкон", L"Служба Emulkon: " + r->done);
            } else {
                logLine(L"служба Emulkon: ошибка: " + r->error);
                balloon(L"Эмулкон: служба", r->error);
            }
            return 0;
        }
        case WM_APP_UPDATE: {
            std::unique_ptr<UpdateInfo> info(reinterpret_cast<UpdateInfo*>(lp));
            onUpdate(*info);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerUpdates) {
                SetTimer(hwnd, kTimerUpdates, 60 * 60 * 1000, nullptr);  // дальше — раз в час
                checkUpdatesAsync();
                return 0;
            }
            break;
        case WM_APP_ANSWER: {
            std::unique_ptr<std::wstring> msg(reinterpret_cast<std::wstring*>(lp));
            g.terminal.answerSent(wp != 0, *msg);
            return 0;
        }
        case WM_APP_PRINTED: {
            std::unique_ptr<std::wstring> info(reinterpret_cast<std::wstring*>(lp));
            if (wp) {
                logLine(L"напечатано" + (info->empty() ? std::wstring() : L": " + *info));
            } else {
                logLine(L"ошибка печати: " + *info);
                balloon(L"Эмулкон: ошибка печати", *info);
            }
            return 0;
        }
        case WM_COMMAND:
            command(LOWORD(wp));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_ENDSESSION:  // выход из Windows — убрать значок и остановить свой сервер
            if (wp) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g.stream.stop();
            stopServer();
            Shell_NotifyIconW(NIM_DELETE, &g.nid);
            logLine(L"выход");
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

PrintedDoc sampleDoc() {
    PrintedDoc d;
    d.deviceId = "kkt-01";
    d.kind = "check";
    d.number = 3;
    d.time = "2026-10-08T09:00:38Z";
    d.journalSeq = 1;
    d.lines = {
        "    *** ЭМУЛЯТОР · НЕ ФИСКАЛЬНЫЙ ***",
        "     ООО «ТЕСТ ЭМУЛКОН» (ЭМУЛЯТОР)",
        "     г. Москва, ул. Тестовая, д. 1",
        "            ИНН 7700000000",
        "------------------------------------------",
        "               КАССОВЫЙ ЧЕК",
        "                  ПРИХОД",
        "СМЕНА 1                             ЧЕК 1",
        "КАССИР: Петрова А.А., кассир",
        "------------------------------------------",
        "Молоко 3,2% 1 л",
        "  2 x 89.90                       =179.80",
        "  НДС 10%                           16.35",
        "------------------------------------------",
        "ИТОГ                              =179.80",
        "НАЛИЧНЫМИ                         =200.00",
        "СДАЧА                              =20.20",
        "ФД                                      3",
        "ФП                             2057366264",
    };
    // QR-код чека ФНС (t=20261008T1200&s=179.80&fn=9999078900004312&i=3&fp=2057366264&n=1) — матрица сервера (internal/qr).
    d.qrRows = {
        "1111111000001101000001000010001111111",
        "1000001001101111110010111111001000001",
        "1011101011110110000001010000101011101",
        "1011101010110111011010111111001011101",
        "1011101010110001110101011010001011101",
        "1000001011000110110001001111001000001",
        "1111111010101010101010101010101111111",
        "0000000011011101011001101001000000000",
        "1011111001001011100001001101101111100",
        "0001010101100011011011101010010000010",
        "1000001001100001101100100101101001101",
        "1001110101111001001111001010001000011",
        "1010001100001111111110100101101111110",
        "0000110010100000000100011010010001000",
        "1111011111011011001010101101010001101",
        "1110110010111101100000001001011001000",
        "0110101011001111001111110101101110101",
        "0110000011010111110000011110010101110",
        "0001011001111010001010100011100010001",
        "1001100101100110100001000010101101001",
        "0110111010011010111010010101111110111",
        "1011010001110000010101111010011001010",
        "0000011010010011010011001111001101001",
        "1101000100011111011001110000000001011",
        "1101001110101111100101010101101010101",
        "1010010000001101001010111010010100110",
        "1000101110000111101101101001111010011",
        "1001110110001011001111110011101101000",
        "1011111110100101111110011101111110101",
        "0000000010110110000100111010100011010",
        "1111111001100111001010100010101011101",
        "1000001011011011000000001001100011010",
        "1011101010110001001111111101111110101",
        "1011101010000111100000010000001111001",
        "1011101010111110001011001111010000111",
        "1000001000101010101001110010101011001",
        "1111111010011010111010000101001101111",
    };
    return d;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g.inst = inst;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g.configPath = defaultConfigPath();
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--render" && i + 1 < argc) {  // самопроверка окна
            ReceiptPopup p;
            if (!p.create(inst)) return 2;
            return p.renderToBmp(sampleDoc(), argv[i + 1]) ? 0 : 1;
        }
        if (a == L"--render-scales" && i + 1 < argc) {  // самопроверка окна весов
            TerminalWindow t;
            if (!t.create(inst, nullptr)) return 2;
            PromptEvent p;
            p.promptId = "sample";
            p.deviceId = "scales-01";
            p.operation = "GetWeight";
            p.title = L"ВЗВЕШИВАНИЕ";
            p.text = L"Положите товар на весы";
            p.hasInput = true;
            p.inputLabel = L"Вес";
            p.inputUnit = L"кг";
            p.inputValue = 0.532;
            p.inputMax = 15;
            p.deadline = deadlineIn(48);
            p.actions = {{"weigh", L"Взвесить", "primary"}, {"cancel", L"Отмена", ""}};
            return t.renderToBmp(p, argv[i + 1]) ? 0 : 1;
        }
        if (a == L"--render-terminal" && i + 1 < argc) {  // самопроверка окна терминала
            TerminalWindow t;
            if (!t.create(inst, nullptr)) return 2;
            PromptEvent p;
            p.promptId = "sample";
            p.deviceId = "acquiring-01";
            p.operation = "Pay";
            p.title = L"ОПЛАТА";
            p.text = L"Приложите, вставьте или проведите карту";
            p.amount = 111;
            p.deadline = deadlineIn(48);
            p.defaultCard = "mir";
            p.cards = {{"mir", L"МИР", ""}, {"visa", L"VISA", ""}, {"mastercard", L"Mastercard", ""}};
            p.actions = {{"card", L"Приложить карту", "primary"}, {"decline", L"Отказ банка", "danger"},
                         {"insufficient", L"Недостаточно средств", "danger"}, {"wrong_pin", L"Неверный ПИН", "danger"},
                         {"cancel", L"Отмена", ""}};
            return t.renderToBmp(p, argv[i + 1]) ? 0 : 1;
        }
        if (a == L"--print" && i + 2 < argc) {  // самопроверка печати
            std::wstring error;
            bool ok = printDoc(sampleDoc(), argv[i + 1], argv[i + 2], error);
            if (!ok) MessageBoxW(nullptr, error.c_str(), L"Эмулкон: печать", MB_ICONERROR);
            return ok ? 0 : 1;
        }
        if (a == L"--exit") {  // закрыть запущенный трей штатно (значок уберётся, свой сервер остановится)
            HWND other = FindWindowW(kMainClass, nullptr);
            if (!other) return 1;
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
            PostMessageW(other, WM_COMMAND, kCmdExit, 0);
            if (proc) {
                WaitForSingleObject(proc, 10000);
                CloseHandle(proc);
            }
            return 0;
        }
        if (a == L"--config" && i + 1 < argc) g.configPath = argv[++i];
    }
    LocalFree(argv);

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\EmulkonTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Трей Эмулкон уже запущен — значок в области уведомлений.", L"Эмулкон", MB_ICONINFORMATION);
        return 0;
    }

    openLog();
    if (!loadConfig(g.configPath, g.cfg)) save();  // первый запуск — записать умолчания
    logLine(L"старт; настройки: " + g.configPath);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = mainProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    g.hwnd = CreateWindowExW(0, kMainClass, L"Эмулкон", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g.popup.create(inst);
    g.terminal.create(inst, answerPrompt);

    int iconSize = GetSystemMetrics(SM_CXSMICON);
    g.iconOn = makeIcon(true, iconSize);
    g.iconOff = makeIcon(false, iconSize);
    addIcon();
    startStream();
    SetTimer(g.hwnd, kTimerUpdates, 30 * 1000, nullptr);  // новая версия: первый раз через 30 с

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g.log) std::fclose(g.log);
    CloseHandle(single);
    return 0;
}
