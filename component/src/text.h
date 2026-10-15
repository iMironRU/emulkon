// Работа с текстом: UTF-8 <-> UTF-16 (WCHAR_T платформы — char16_t), регистронезависимое
// сравнение имён методов (1С не различает регистр, в том числе кириллицы), XML-помощники.
#pragma once

#include <map>
#include <string>

namespace emu {

std::u16string utf8to16(const std::string& s);
std::string utf16to8(const std::u16string& s);
std::string utf16to8(const char16_t* s);

// Сравнение без учёта регистра для латиницы и кириллицы (включая Ё).
bool equalsIgnoreCase(const std::u16string& a, const char16_t* b);

// Экранирование значения XML-атрибута.
std::string xmlAttr(const std::string& s);

// Разбор ConnectionParameters: <Parameters><Parameter Name="…" Value="…"/>…</Parameters>.
// Возвращает имя → значение (сущности XML раскрыты). Неизвестная разметка пропускается.
std::map<std::string, std::string> parseConnectionParameters(const std::string& xml);

// База 1С из командной строки процесса (UTF-8) — запасной способ, если параметр «База 1С» пуст
// (ARCHITECTURE §4.3): /F"путь" → путь, /S"сервер\база" → сервер/база, /IBName"имя" → имя,
// /IBConnectionString"File=…;" или "Srvr=…;Ref=…;". Остальные ключи (в том числе /P с паролем)
// не читаются. Пусто — базы в командной строке нет.
std::string infobaseFromCommandLine(const std::string& commandLine);

}  // namespace emu
