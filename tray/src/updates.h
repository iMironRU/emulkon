// Новая версия Эмулкона для трея (docs/local-install.md §2.3, issue #8): сервер проверяет выпуски раз в сутки и
// только сообщает — трей по «Обновить до X…» сам качает установщик в «Загрузки» пользователя с меткой «из
// Интернета», сверяет размер и SHA-256 и запускает его (UAC). Не служба и не ProgramData: exe, тихо скачанный в
// общий каталог и запущенный оттуда, антивирус считает подозрительным (#20).
#pragma once

#include <string>

namespace tray {

// UpdateInfo — ответ GET /api/v1/updates (server/internal/updates).
struct UpdateInfo {
    bool available = false;
    std::wstring current;
    std::wstring version;  // новая версия
    std::wstring published;
    std::wstring page;     // страница выпуска
    std::wstring url;      // установщик выпуска
    std::wstring sha256;
    long long size = 0;
};

bool parseUpdateState(const std::string& json, UpdateInfo& out);

// isLocalServer — сервер на этом компьютере (127.0.0.1, localhost, [::1]).
bool isLocalServer(const std::wstring& url);

// downloadAllowed — откуда трею можно скачать установщик: EmulkonSetup_<версия>.exe из выпуска на GitHub
// (https://github.com/iMironRU/emulkon/releases/download/v<версия>/). Свой адрес (UpdateURL службы — стенды,
// зеркало) — только от сервера этого компьютера: его настройки меняет администратор. Иначе — страница выпуска.
bool downloadAllowed(const std::wstring& url, const std::wstring& version, bool localServer);

// saveInstaller — тело ответа в dir\EmulkonSetup_<версия>.exe, только если сошлись размер (если известен) и
// SHA-256; с меткой «из Интернета» (Zone.Identifier, ZoneId=3), как у файла из браузера. Путь — в path.
bool saveInstaller(const std::string& body, const UpdateInfo& u, const std::wstring& dir, std::wstring& path,
                   std::wstring& err);

// userDownloadsDir — папка «Загрузки» текущего пользователя.
std::wstring userDownloadsDir();

// sha256Hex — SHA-256 шестнадцатеричной строкой в нижнем регистре; пусто — не посчитать.
std::wstring sha256Hex(const std::string& data);

}  // namespace tray
