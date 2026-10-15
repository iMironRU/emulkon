// Документ, «напечатанный» виртуальным устройством: запись журнала device.print
// (docs/protocol.md §2.1, models.PrintedDocument на сервере).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tray {

struct PrintedDoc {
    int64_t journalSeq = 0;   // seq записи журнала — курсор потока
    std::string deviceId;
    std::string workstation;
    std::string kind;          // check, correction, copy, open_shift, close_shift, x_report, …
    int number = 0;            // номер ФД, 0 — нефискальный
    std::string time;          // RFC 3339
    bool electronic = false;   // только в электронном виде
    bool printError = false;   // сбой печати бумажной формы
    std::vector<std::string> lines;  // UTF-8
    std::vector<std::string> qrRows; // QR-код под текстом (чек ФНС): строки «1010…», 1 — тёмный модуль
    // Что сделать с документом, решает сервер по настройке вывода устройства — она в карточке
    // устройства веб-интерфейса, а не в трее (docs/protocol.md §2.3, ARCHITECTURE §4.3).
    bool reprint = false;      // device.reprint — «напечатать ещё раз» из карточки
    bool showOnScreen = true;  // всплывающее окно; сервер без output — показываем, как раньше
    bool print = false;        // на принтер Windows
    std::wstring printer;      // пусто — принтер по умолчанию
};

// parseJournalEntry разбирает запись журнала (JSON) с method=device.print или device.reprint.
bool parseJournalEntry(const std::string& json, PrintedDoc& out);

// PromptOption — кнопка экрана устройства или карта в списке.
struct PromptOption {
    std::string id;
    std::wstring title;
    std::string kind;  // primary | danger
};

// PromptEvent — экран устройства ждёт человека (device.prompt) или дождался
// (device.prompt_done): эквайринговый терминал «приложите карту» (docs/models/acquiring.md),
// весы «положите товар» с полем веса (docs/models/scales.md).
struct PromptEvent {
    int64_t journalSeq = 0;
    bool done = false;
    std::string promptId;
    std::string deviceId;
    std::string workstation;
    std::string operation;
    std::wstring title, text;   // «ОПЛАТА», «Приложите, вставьте или проведите карту»
    double amount = 0, amountCash = 0;
    std::vector<PromptOption> actions, cards;
    std::string defaultCard;
    std::string deadline;       // RFC 3339
    bool hasInput = false;      // на экране поле ввода («Вес, кг»)
    std::wstring inputLabel, inputUnit;
    double inputValue = 0, inputMin = 0, inputMax = 0;
    std::string action;         // done: чем закончился (card, decline, weigh, …, timeout)
    std::wstring resultText;    // done: «ОДОБРЕНО», «ОТКАЗ: …», «ВЕС 0.532 КГ»
    bool hasValue = false;      // done: значение поля в ответе
    double value = 0;
};

// parsePromptEntry разбирает запись журнала device.prompt или device.prompt_done.
bool parsePromptEntry(const std::string& json, PromptEvent& out);

// KeyboardEvent — запись device.keyboard: сканер или считыватель в режиме клавиатуры, текст
// вводится в активное окно (docs/models/input.md).
struct KeyboardEvent {
    int64_t journalSeq = 0;
    std::string deviceId;
    std::wstring text;
    std::string suffix;  // enter, tab, none
    int delayMs = 0;
};

// parseKeyboardEntry разбирает запись журнала с method=device.keyboard.
bool parseKeyboardEntry(const std::string& json, KeyboardEvent& out);

// secondsUntil — сколько секунд осталось до момента RFC 3339 (отрицательное — прошло).
int secondsUntil(const std::string& rfc3339);

// kindTitle — название вида документа для заголовка окна.
std::wstring kindTitle(const std::string& kind);

// shortTime — «08.10 14:00:38» из RFC 3339 (местное время).
std::wstring shortTime(const std::string& rfc3339);

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

}  // namespace tray
