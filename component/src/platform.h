// Платформенный слой компоненты: файлы, каталоги, окружение, фоновый поток — для кэша
// самообновления (update.cpp). Windows — platform_win.cpp, Linux — platform_posix.cpp.
// Пути — UTF-8.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace emu {
namespace sys {

extern const char kSep;  // разделитель каталогов: '\\' или '/'

std::string env(const char* name);
void setEnv(const char* name, const char* value);  // nullptr — удалить (тесты)

bool isDir(const std::string& path);
bool isFile(const std::string& path);
bool ensureDir(const std::string& path);  // со всеми родителями
bool readFile(const std::string& path, std::string& out);  // до 64 МБ
bool writeFile(const std::string& path, const std::string& data);  // с fsync; при ошибке файла нет
bool replaceFile(const std::string& from, const std::string& to);  // переименование с заменой
void removeFile(const std::string& path);
void removeDir(const std::string& path);  // только пустой
std::vector<std::string> listDirs(const std::string& path);  // имена подкаталогов
std::string tempDir();
long processId();
std::string lastError();  // текст последней ошибки ОС: «код 5», «Permission denied»

// defaultCacheRoot — кэш обновлений драйвера: %LOCALAPPDATA%\Emulkon\driver;
// на Linux — $XDG_CACHE_HOME/emulkon/driver или ~/.cache/emulkon/driver. Пусто — не определить.
std::string defaultCacheRoot();

// runDetached выполняет fn в фоновом потоке. Модуль компоненты не выгружается, пока поток работает:
// 1С может выгрузить компоненту посреди скачивания обновления.
void runDetached(std::function<void()> fn);

}  // namespace sys
}  // namespace emu
