// Настройки трея: %APPDATA%\Emulkon\tray.ini (UTF-8, ключ=значение).
#pragma once

#include <string>

namespace tray {

struct Config {
    std::wstring serverUrl = L"http://127.0.0.1:8737";
    std::wstring controlToken;            // EMULKON_CONTROL_TOKEN сервера
    std::wstring workstation;             // пусто — этот компьютер; * — все рабочие места
    // Показывать и печатать ли документ — настройка устройства на сервере (карточка устройства,
    // ARCHITECTURE §4.3); трей выполняет решение из записи журнала. Здесь — только как.
    int popupSeconds = 10;
    std::wstring printOutputDir;          // для PDF/XPS-принтеров: сохранять файлы без диалога
    std::wstring serverExe;               // локальный сервер для «Запустить сервер»
    std::wstring serverData;              // каталог данных локального сервера
    std::wstring serverListen;            // адрес прослушивания локального сервера; пусто — из serverUrl
    bool serverAutostart = false;         // поднимать локальный сервер, если он не отвечает (ставит установщик)
    std::wstring updateNotified;          // версия, о выходе которой трей уже сообщил (issue #8)
};

std::string serializeConfig(const Config& c);
void parseConfig(const std::string& text, Config& c);

std::wstring defaultConfigPath();
bool loadConfig(const std::wstring& path, Config& c);
bool saveConfig(const std::wstring& path, const Config& c);

// effectiveWorkstation — фильтр рабочего места: из настроек или имя компьютера
// (так же, как по умолчанию называет рабочее место компонента-драйвер).
std::wstring effectiveWorkstation(const Config& c);
std::wstring computerName();

}  // namespace tray
