# Драйвер для образа Docker

Каталог копируется в образ как `/downloads` (страница загрузок сервера и самообновление драйверов). Перед сборкой
образа сюда кладутся файлы из `dist/downloads` сборки Windows: `EmulkonDriver_<версия>.zip`,
`EmulkonDriver_<версия>_win64.dll`, `…_win32.dll`, `…_lin64.so` (и при желании `EmulkonSetup_<версия>.exe`).
CI и релиз делают это сами; в git — только этот файл (`.gitignore`).
