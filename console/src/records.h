// Сценарии и журнал в оснастке (docs/local-install.md §4.1, этап 4): разбор ответов Control API
// (docs/protocol.md §2.4, §2.5), таблицы, выгрузка и загрузка. Логика без COM — её проверяют тесты.
#pragma once

#include <string>
#include <vector>

#include "inventory.h"

namespace console {

// --- сценарии (ARCHITECTURE §7) ---

struct Scenario {
    std::string id;
    std::wstring name;
    bool enabled = false;
    int limit = 0, fired = 0, seen = 0;
    std::wstring condition;  // «класс kkt, метод PrintCheck, на 3-м вызове»
    std::wstring action;     // «ошибка -3: Нет бумаги»
    std::string raw;         // правило как прислал сервер (JSON-объект)
};

bool parseScenarios(const std::string& body, std::vector<Scenario>& out, std::wstring& err);
Table scenariosTable(const std::vector<Scenario>& list);
// ruleForPut — правило для PUT/POST: enabled заменён, счётчики fired/seen убраны (их ведёт сервер).
std::string ruleForPut(const std::string& raw, bool enabled);
// scenariosFile — выгрузка: {"rules":[…]} с отступами, без счётчиков; её же принимает importScenarios.
std::string scenariosFile(const std::vector<Scenario>& list);

struct ImportResult {
    int created = 0, replaced = 0;
    std::vector<std::wstring> errors;  // «правило x: причина»
};
// importScenarios — загрузить правила из файла ({"rules":[…]} или […]): с известным id — заменить (PUT),
// с новым или без id — добавить (POST). current — правила сервера сейчас.
ImportResult importScenarios(const std::string& text, const std::vector<Scenario>& current);

Reply apiCreateScenario(const std::string& rule);
Reply apiPutScenario(const std::string& id, const std::string& rule);
Reply apiDeleteScenario(const std::string& id);

// --- журнал (docs/protocol.md §2.5) ---

struct JournalEntry {
    long long seq = 0;
    std::wstring time;       // местное, с секундами
    std::wstring direction;  // «команда», «ответ», «событие», «служебное»
    std::wstring workstation, device, method, latency, rules;
    std::wstring payload;    // начало данных одной строкой
    std::string raw;         // запись целиком (JSON)
};

const int kJournalLimit = 500;

// journalPath — запрос последних limit записей; text — отбор (поиск по записи), пусто — все.
std::string journalPath(const std::wstring& text, int limit = kJournalLimit, bool jsonl = false);
bool parseJournal(const std::string& body, std::vector<JournalEntry>& out, bool& hasMore, std::wstring& err);
Table journalTable(const std::vector<JournalEntry>& list);  // новые сверху

// exportJournal — все записи по отбору в файл JSON Lines потоком (журнал бывает большим). lines — сколько
// записей записано.
bool exportJournal(const std::wstring& path, const std::wstring& text, long long& lines, std::wstring& err);

// Отбор журнала в этой консоли (общий для окон): только в памяти.
void setJournalFilter(const std::wstring& text);
std::wstring journalFilter();

// prettyJson — JSON с отступами для окон свойств (строки — как есть, без \u-экранирования кириллицы).
std::wstring prettyJson(const std::string& raw);

}  // namespace console
