// Самообновление драйвера (docs/protocol.md §1.5).
//
// Сервер предлагает DLL драйвера новее (driver_update в ответе на открытие сессии или
// GET /api/v1/driver/update). Драйвер скачивает её в кэш пользователя с проверкой SHA-256:
//   %LOCALAPPDATA%\Emulkon\driver\<версия>\EmulkonDriver_win64.dll (win32 — для x86).
// При следующей загрузке компоненты экспорты переадресуются в самую новую DLL из кэша
// (exports.cpp) — переустанавливать драйвер в БПО не нужно. Работающую DLL не трогаем:
// новая версия начинает работать после перезапуска 1С.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "http.h"
#include "json.h"

#ifndef EMULKON_VERSION
#define EMULKON_VERSION "0.0.0-dev"
#endif

namespace emu {

// compareVersions — сравнение версий x.y.z как на сервере: -1, 0, 1. Суффикс после «-»
// не учитывается, неразборчивая версия считается самой старой.
int compareVersions(const std::string& a, const std::string& b);
bool validVersion(const std::string& v);

// sha256Hex — SHA-256 последовательности байтов, hex в нижнем регистре.
std::string sha256Hex(const std::string& data);

// UpdateOffer — DLL драйвера на сервере.
struct UpdateOffer {
    std::string version;
    std::string url;  // путь на сервере: /downloads/EmulkonDriver_<версия>_win64.dll
    std::string sha256;
    int64_t size = 0;
    bool valid() const;
};

// parseUpdateOffer — из объекта {version, url, sha256, size}; j == nullptr — пусто.
UpdateOffer parseUpdateOffer(const Json* j);

// driverArch — архитектура сборки в терминах MANIFEST.XML: x86_64 или i386.
const char* driverArch();

// newerVersions — имена каталогов кэша, которые являются версиями новее own, от новой к старой.
std::vector<std::string> newerVersions(const std::vector<std::string>& names, const std::string& own);

// --- кэш обновлений (update_win.cpp) ---

// updateCacheRoot — %EMULKON_DRIVER_CACHE% (тесты) или %LOCALAPPDATA%\Emulkon\driver.
std::wstring updateCacheRoot();

// cachedVersions — версии в кэше, для которых есть DLL этой архитектуры.
std::vector<std::string> cachedVersions();
std::wstring cachedDllPath(const std::string& version);

// redirectDisabled — EMULKON_DRIVER_NO_UPDATE=1: не переходить на DLL из кэша.
bool redirectDisabled();

// rememberServer, rememberedServer — адрес сервера последнего подключения. Нужен
// дополнительным действиям: БПО не передаёт им параметры подключения.
void rememberServer(const std::string& url);
std::string rememberedServer();

// updateCached — DLL этой версии уже в кэше и совпадает с предложением сервера.
bool updateCached(const UpdateOffer& offer);

// installUpdate — скачать DLL в кэш: размер и SHA-256 сверяются до того, как файл
// получит своё имя. true — DLL в кэше (скачана сейчас или раньше).
bool installUpdate(HttpClient& http, const std::string& serverUrl, const UpdateOffer& offer, int timeoutMs,
                   const std::string& workstation, std::string& message);

enum class UpdateCheck { UpToDate, Downloaded, Pending, Failed };

// checkForUpdate — спросить сервер о новой версии и сразу скачать её. message — для пользователя.
UpdateCheck checkForUpdate(HttpClient& http, const std::string& serverUrl, int timeoutMs, const std::string& workstation,
                           std::string& message);

// startBackgroundUpdate — installUpdate в фоновом потоке: операция 1С не ждёт скачивания.
// Одна попытка на версию за время жизни процесса. Поток держит ссылку на DLL, поэтому
// выгрузка компоненты во время скачивания безопасна.
void startBackgroundUpdate(const std::string& serverUrl, const UpdateOffer& offer, int timeoutMs,
                           const std::string& workstation);

}  // namespace emu
