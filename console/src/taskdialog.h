// Окно новой версии с кликабельной ссылкой «Что нового» (#25): TaskDialog из Common Controls 6.
#pragma once

#include <windows.h>

#include <string>

namespace console {

// newVersionTaskDialog — head: «Вышла версия X от даты»; installed: «Установлена Y.»; page — страница выпуска
// (ссылка «Что нового», пусто — без ссылки); about — что будет дальше. answer — IDYES («Скачать и установить») или
// IDCANCEL. Оснастка живёт в mmc.exe без своего манифеста для загрузчика: манифест Common Controls 6 (ресурс 3
// модуля, где этот код) активируется только на время окна. false — TaskDialog недоступен (окно не показано).
bool newVersionTaskDialog(HWND parent, const std::wstring& head, const std::wstring& installed, const std::wstring& page,
                          const std::wstring& about, int& answer);

}  // namespace console
