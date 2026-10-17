// Новая версия Эмулкона для трея (docs/local-install.md §2.3, issue #8): сервер проверяет выпуски раз в сутки,
// служба Windows скачивает установщик — трей сообщает и по «Обновить до X…» запускает его (UAC).
#pragma once

#include <string>

namespace tray {

// UpdateInfo — ответ GET /api/v1/updates (server/internal/updates).
struct UpdateInfo {
    bool available = false;
    std::wstring current;
    std::wstring version;    // новая версия
    std::wstring published;
    std::wstring page;       // страница выпуска
    std::wstring installer;  // скачанный службой установщик (на компьютере сервера)
    std::wstring sha256;
};

bool parseUpdateState(const std::string& json, UpdateInfo& out);

// isLocalServer — сервер на этом компьютере (127.0.0.1, localhost, [::1]): только тогда установщик из ответа — свой.
bool isLocalServer(const std::wstring& url);

// installerAllowed — запускать можно только установщик, скачанный службой: файл EmulkonSetup_<версия>.exe в
// каталоге <ProgramData>\Emulkon\updates, без «..» и других каталогов.
bool installerAllowed(const std::wstring& path, const std::wstring& version, const std::wstring& programData);

// fileSha256 — SHA-256 файла шестнадцатеричной строкой в нижнем регистре; пусто — не прочитать.
std::wstring fileSha256(const std::wstring& path);

}  // namespace tray
