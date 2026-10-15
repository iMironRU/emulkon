// Данные оснастки «Эмулкон» (docs/local-install.md §4.1): сводка корня и раздел «Служба».
// Логика без COM и окон — её проверяют модульные тесты; система (SCM, реестр, HTTP) — system.cpp, admin.cpp.
#pragma once

#include <string>
#include <vector>

#ifndef EMULKON_VERSION
#define EMULKON_VERSION "0.0.0-dev"
#endif

namespace console {

// Настройки службы из реестра (docs/local-install.md §2.1): политика перекрывает HKLM\Software\Emulkon\Server.
// Пусто — значения нет или ключ не читается (обычному пользователю ключ Server закрыт).
struct ServerSettings {
    std::wstring addr;          // Addr
    std::wstring network;       // Network: L"1" — сервер для касс в сети
    bool controlToken = false;  // задан ControlTokenHash
    bool deviceToken = false;   // задан DeviceTokenHash
    bool policy = false;        // есть ключ политики: его значения консоль не меняет
    bool readable = false;      // ключ Server прочитан (нужны права администратора)
};

// listenAddr — адрес, который слушает служба: Addr, иначе 0.0.0.0:8737 при Network=1, иначе 127.0.0.1:8737.
std::wstring listenAddr(const ServerSettings& s);
// localUrl — адрес для запросов с этого компьютера: 0.0.0.0, [::] и пустой хост → 127.0.0.1.
std::wstring localUrl(const std::wstring& listen);
// isLocalOnly — служба слушает только этот компьютер (127.0.0.1, localhost, [::1]).
bool isLocalOnly(const std::wstring& listen);

enum class Service { Missing, Stopped, Starting, Running, Stopping, Unknown };
std::wstring serviceText(Service s);

// ServiceInfo — служба Emulkon в SCM. Конфигурацию видно не всем: configKnown.
struct ServiceInfo {
    Service state = Service::Unknown;
    bool configKnown = false;
    std::wstring startType;  // «автоматически», «вручную», «отключена»
    std::wstring account;    // NT AUTHORITY\LocalService
    std::wstring command;    // "…\emulkon.exe" -service
};

// Row — строка списка: «Параметр» — «Значение».
struct Row {
    std::wstring name;
    std::wstring value;
};

// Ответы сервера: тело и HTTP-статус (0 — нет связи, error — почему).
struct Reply {
    int status = 0;
    std::string body;
    std::string error;
};

struct Inputs {
    Service service = Service::Unknown;
    ServerSettings settings;
    bool consoleToken = false;  // в консоли введён токен управления
    Reply info;          // GET /control/v1/info
    Reply devices;       // GET /control/v1/devices
    Reply workstations;  // GET /control/v1/workstations
};

// summaryRows собирает строки сводки. Сбой запроса — строка с причиной, а не пустое место.
std::vector<Row> summaryRows(const Inputs& in);
// serviceRows — раздел «Служба»: состояние, запуск, учётная запись, адрес, токены, где данные и журнал.
std::vector<Row> serviceRows(const ServiceInfo& svc, const ServerSettings& s, bool consoleToken, const std::wstring& dataDir);

// --- system.cpp: реестр, служба, HTTP ---

ServerSettings readServerSettings();
Service queryService();
ServiceInfo queryServiceInfo();
Inputs collect();  // всё сразу: запросы к серверу — с коротким таймаутом и токеном консоли
// serverBase — адрес сервера для запросов: служба этого компьютера; переменная EMULKON_CONSOLE_URL —
// другой сервер (живой тест console/tests/live.ps1 поднимает отдельный сервер с временными данными).
std::wstring serverBase(const ServerSettings& s);

// Токен управления, введённый или выданный в консоли, — только в памяти, до закрытия консоли.
void setConsoleToken(const std::string& token);
std::string consoleToken();

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

// --- admin.cpp: действия администратора ---

// controlService останавливает (stop) и/или запускает (start) службу и ждёт итога до 30 с.
bool controlService(bool stop, bool start, std::wstring& error);
// newToken — случайный токен (32 байта, base64url) с префиксом «emk_».
std::string newToken();
std::string sha256Hex(const std::string& s);
std::string base64url(const unsigned char* data, size_t n);
// writeTokenHash пишет SHA-256 токена в HKLM\Software\Emulkon\Server (value — ControlTokenHash или
// DeviceTokenHash); пустой hash — снять токен. Нужны права администратора.
bool writeTokenHash(const wchar_t* value, const std::string& hash, std::wstring& error);
// installDir — каталог установки: на уровень выше console\ с этой DLL.
std::wstring installDir();
std::wstring machineDataDir();  // %ProgramData%\Emulkon
// runModify — мастер «Изменить» без окна: EmulkonSetup.exe /modify /network|/nonetwork. Он же меняет
// правило брандмауэра и перезапускает службу (docs/local-install.md §3).
bool runModify(bool network, std::wstring& error);

}  // namespace console
