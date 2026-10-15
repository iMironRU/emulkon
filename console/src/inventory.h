// Рабочие места и устройства в оснастке (docs/local-install.md §4.1, этап 3): разбор ответов Control API
// (docs/protocol.md §2.3), таблицы для правой панели, запросы. Логика без COM — её проверяют тесты.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "summary.h"

namespace console {

struct Workstation {
    std::string id;
    std::wstring alias, note;
    std::wstring firstSeen, lastSeen;  // местное время «15.10.2026 13:20», «—» — не было
    std::vector<std::wstring> infobases, devices, printers;
    std::wstring defaultPrinter;
    int sessions = 0;
    bool token = false;
    std::wstring tokenHint, tokenIssued;  // последние символы токена, когда выдан
};

struct Device {
    std::string id;
    std::string type;  // ключ класса: kkt, scanner, …
    std::wstring name, alias, note, equipmentType, workstation, infobase, instanceName;
    std::wstring created, lastSeen, lastError;
    long long commands = 0;
    int sessions = 0, listeners = 0;
    bool online = false;
    std::vector<Row> state, settings;  // «ключ» — «значение» (JSON одной строкой)
};

// Разбор ответов GET /control/v1/workstations, /control/v1/devices; err — почему не вышло.
bool parseWorkstations(const std::string& body, std::vector<Workstation>& out, std::wstring& err);
bool parseDevices(const std::string& body, std::vector<Device>& out, std::wstring& err);
// typeTitles — ключ класса → название («Контрольно-кассовая техника») из GET /control/v1/info.
std::map<std::string, std::wstring> typeTitles(const std::string& infoBody);

// localTime — RFC 3339 → «15.10.2026 13:20» по местному времени; пусто и нулевое время — «—».
std::wstring localTime(const std::string& rfc3339);
std::wstring localTimeSeconds(const std::string& rfc3339);  // «15.10.2026 13:20:05»

// Таблица правой панели: колонки, строки, ключ строки (id объекта). problem — вместо строк, если
// данных нет (сервер не отвечает, нужен токен).
struct Column {
    std::wstring name;
    int width;
};
struct Table {
    std::vector<Column> columns;
    std::vector<std::vector<std::wstring>> cells;
    std::vector<std::string> keys;
    std::wstring problem;
};

Table rowsTable(const std::vector<Row>& rows);  // «Параметр — Значение»
Table workstationsTable(const std::vector<Workstation>& list);
Table devicesTable(const std::vector<Device>& list, const std::map<std::string, std::wstring>& titles);
std::wstring workstationTitle(const Workstation& w);  // подпись или код
std::wstring deviceTitle(const Device& d);            // подпись или имя

// replyError — ответ сервера по-человечески: нет связи, нужен токен, текст ошибки API.
std::wstring replyError(const Reply& r);

// --- запросы (system.cpp): адрес службы и токен консоли ---

Reply apiRequest(const char* method, const std::string& path, const std::string& body = {});

// Действия над рабочими местами и устройствами (Control API, docs/protocol.md §2.3). Их зовут окна
// оснастки и живой тест. Успех — статус 2xx.
Reply apiIssueWorkstationToken(const std::string& id, std::string& token);  // token — выданный, один раз
Reply apiRevokeWorkstationToken(const std::string& id);
Reply apiSetWorkstationMeta(const std::string& id, const std::string& patch);
Reply apiSetDeviceMeta(const std::string& id, const std::string& patch);
Reply apiResetDevice(const std::string& id);
Reply apiDeleteDevice(const std::string& id);
// cleanText — одна строка без управляющих символов: сервер их в подписях и заметках не принимает
// (sessions.validateText), переводы строк и табуляции становятся пробелами.
std::wstring cleanText(const std::wstring& s);
// metaPatch — тело PATCH: только изменившиеся подпись и заметка (после cleanText); пусто — ничего не менялось.
std::string metaPatch(const std::wstring& alias0, const std::wstring& alias, const std::wstring& note0, const std::wstring& note);
std::string jsonString(const std::wstring& s);  // строка JSON в кавычках
std::string urlPart(const std::string& s);      // экранирование для пути URL

}  // namespace console
