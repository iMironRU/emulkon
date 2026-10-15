// Окна оснастки: свойства службы, токен для консоли, новый токен (docs/local-install.md §4.1, этап 2).
// Шаблоны строятся в памяти: без .rc и его кодировок.
#pragma once

#include <windows.h>
#include <prsht.h>

#include <string>

namespace console {

extern HINSTANCE g_instance;  // DLL оснастки — её задаёт DllMain

// createServicePage — страница «Общие» свойств службы. notify — дескриптор MMC для MMCPropertyChangeNotify:
// страница освобождает его сама. Страница работает в своём потоке: интерфейсы MMC из неё не вызываются.
HPROPSHEETPAGE createServicePage(LONG_PTR notify);

// askToken — ввести токен управления для этой консоли. false — отмена.
bool askToken(HWND parent, std::string& token);

// issueToken / removeToken — выдать новый или снять токен управления (control) или устройств: вопрос,
// хеш в реестр, перезапуск службы, показ токена один раз. true — что-то изменилось.
bool issueToken(HWND parent, bool control);
bool removeToken(HWND parent, bool control);

// showToken — новый токен один раз: поле для выделения и «Копировать».
void showToken(HWND parent, const std::wstring& head, const std::string& token, const std::wstring& note);
// askText — строка от пользователя (password — скрытый ввод). false — отмена.
bool askText(HWND parent, const std::wstring& title, const std::wstring& prompt, std::wstring& value, bool password = false);

// --- рабочие места и устройства (этап 3) ---

struct Workstation;
struct Device;

// issueWorkstationToken / revokeWorkstationToken — токен рабочего места через Control API (сервер хранит хеш,
// токен показывается один раз). true — изменилось.
bool issueWorkstationToken(HWND parent, const std::string& id, bool isNew = false);
bool revokeWorkstationToken(HWND parent, const std::string& id);
// newWorkstation — завести рабочее место заранее: код и сразу токен для драйвера кассы.
bool newWorkstation(HWND parent);
bool resetDevice(HWND parent, const Device& d);
bool deleteDevice(HWND parent, const Device& d);

// Страницы свойств: копия объекта на момент открытия; подпись и заметка — PATCH при «Применить».
HPROPSHEETPAGE createWorkstationPage(LONG_PTR notify, const Workstation& w);
HPROPSHEETPAGE createDevicePage(LONG_PTR notify, const Device& d, const std::wstring& classTitle);
HPROPSHEETPAGE createDeviceStatePage(const Device& d);

// --- сценарии и журнал (этап 4) ---

struct Scenario;

bool toggleScenario(HWND parent, const Scenario& s);  // включить или выключить
bool deleteScenario(HWND parent, const Scenario& s);
bool exportScenariosDialog(HWND parent);  // все правила в файл JSON
bool importScenariosDialog(HWND parent);  // правила из файла: известные заменить, новые добавить
bool exportJournalDialog(HWND parent);    // журнал по отбору в файл JSON Lines
bool askJournalFilter(HWND parent);       // отбор журнала по тексту
// createTextPage — страница свойств «только чтение»: заголовок и многострочный текст.
HPROPSHEETPAGE createTextPage(const wchar_t* title, const std::wstring& head, const std::wstring& text);

// --- обновления и настройки (этап 5) ---

// checkUpdatesDialog — latest.json последнего выпуска; новее — «что нового», скачать с проверкой SHA-256 и
// запустить мастер обновления.
bool checkUpdatesDialog(HWND parent);
bool exportSettingsDialog(HWND parent);  // значения HKLM\Software\Emulkon\Server в файл JSON
bool importSettingsDialog(HWND parent);  // из файла: записать в реестр и перезапустить службу

enum class ServiceAction { Start, Stop, Restart };
// serviceAction — запустить, остановить или перезапустить службу (ждёт до 30 с); ошибка — в окне.
bool serviceAction(HWND parent, ServiceAction action);

}  // namespace console
