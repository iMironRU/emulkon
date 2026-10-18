// Обслуживание из оснастки (docs/local-install.md §4.1, этап 5): обновления из релизов GitHub (§2.3) и
// выгрузка и загрузка настроек службы. Разбор и проверки — без окон, их проверяют модульные тесты.
#pragma once

#include <string>
#include <vector>

namespace console {

// --- Обновления ---

// Release — описание выпуска latest.json (его публикует релиз: .github/workflows/release.yml).
struct Release {
    std::string version;    // 0.9.0
    std::string published;  // 2026-10-20
    std::string url;        // https://…/EmulkonSetup_0.9.0.exe
    std::string sha256;     // 64 шестнадцатеричных символа
    long long size = 0;     // байт; 0 — не указан
    std::string notes;      // что нового: Markdown раздела CHANGELOG
};

// parseRelease разбирает latest.json. allowHttp — адрес установщика может быть http (только когда описание
// задано переменной EMULKON_UPDATE_URL: живой тест); иначе — только https. Имя файла — EmulkonSetup_<version>.exe.
bool parseRelease(const std::string& json, bool allowHttp, Release& r, std::wstring& err);
// compareVersions — SemVer без сборки: <0, 0, >0. Предварительная (0.9.0-rc1) младше выпуска 0.9.0.
int compareVersions(const std::string& a, const std::string& b);
// releasePage — страница выпуска (что нового) по адресу установщика …/releases/download/vX/EmulkonSetup_X.exe;
// пусто — адрес не из выпуска GitHub.
std::wstring releasePage(const Release& r);

// updateFeedUrl — адрес latest.json последнего выпуска; EMULKON_UPDATE_URL — другой (живой тест).
std::wstring updateFeedUrl(bool* overridden = nullptr);
// fetchRelease — скачать и разобрать latest.json.
bool fetchRelease(Release& r, std::wstring& err);
// downloadRelease — установщик в каталог dir (оснастка — «Загрузки» пользователя) с проверкой размера и
// SHA-256: файл пишется, только если всё сошлось, с меткой «из Интернета», как из браузера. Путь — в path.
bool downloadRelease(const Release& r, const std::wstring& dir, std::wstring& path, std::wstring& err);
// userDownloadsDir — папка «Загрузки» текущего пользователя.
std::wstring userDownloadsDir();
// runInstaller — запустить скачанный установщик: мастер обновления (консоль уже с правами администратора).
bool runInstaller(const std::wstring& path, std::wstring& err);

// --- Настройки службы ---

// Setting — значение HKLM\Software\Emulkon\Server. Network — DWORD, остальные — строки.
struct Setting {
    std::wstring name;
    std::wstring text;
    unsigned long number = 0;
    bool dword = false;
};

// settingNames — значения, которые читает служба (server/cmd/emulkon/settings.go), в порядке файла.
const std::vector<std::wstring>& settingNames();
// settingsFile — JSON выгрузки: {"format":"emulkon-settings","version":1,"product_version","computer","exported","server":{…}}.
std::string settingsFile(const std::vector<Setting>& values, const std::string& computer, const std::string& exported);
// parseSettingsFile — проверить файл выгрузки: формат, имена и значения. Незнакомые имена — в warnings.
bool parseSettingsFile(const std::string& text, std::vector<Setting>& out, std::vector<std::wstring>& warnings,
                       std::wstring& err);
// settingsSummary — что будет записано: строки «Имя = значение» (хеши токенов — «задан»).
std::wstring settingsSummary(const std::vector<Setting>& values);

// readSettings — значения ключа Server как есть (без политики). Нужны права администратора.
bool readSettings(std::vector<Setting>& out, std::wstring& err);
// applySettings — сделать ключ Server как в файле: заданные записать, остальные из settingNames удалить; затем
// перезапустить службу, если она работала. Network меняет мастер «Изменить» (брандмауэр) — runModify.
bool applySettings(const std::vector<Setting>& values, std::wstring& err);

}  // namespace console
