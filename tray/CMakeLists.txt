# Трей Эмулкон (Windows): значок сервера, всплывающие чеки, печать на принтер.
# Сборка — tray/build.ps1 (clang-cl, ABI MSVC).
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" EMULKON_PRODUCT_VERSION LIMIT_COUNT 1)
project(EmulkonTray VERSION ${EMULKON_PRODUCT_VERSION} LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")

if(NOT WIN32)
  message(FATAL_ERROR "Трей — только Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMPONENT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../component/src")

# Логика без окон: JSON и текст — общие с компонентой.
add_library(tray_core STATIC
  src/doc.cpp
  src/sse.cpp
  src/config.cpp
  src/stream.cpp
  "${COMPONENT_SRC}/json.cpp"
  "${COMPONENT_SRC}/text.cpp")
target_include_directories(tray_core PUBLIC src "${COMPONENT_SRC}")
target_compile_definitions(tray_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(tray_core PUBLIC /utf-8 /EHsc /W3)
target_link_libraries(tray_core PUBLIC winhttp shell32 ole32)

add_executable(EmulkonTray WIN32
  src/main.cpp
  src/icon.cpp
  src/popup.cpp
  src/printing.cpp
  src/keyboard.cpp
  src/settings.cpp
  src/terminal.cpp)
# Значок exe (ярлыки, «Установленные приложения») и версия файла.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../res/emulkon.ico" "${CMAKE_CURRENT_BINARY_DIR}/emulkon.ico" COPYONLY)
configure_file(src/tray.rc.in "${CMAKE_CURRENT_BINARY_DIR}/tray.rc" @ONLY)
target_sources(EmulkonTray PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tray.rc")
target_link_libraries(EmulkonTray PRIVATE tray_core user32 gdi32 winspool advapi32)
# Манифест: визуальные стили Common Controls 6 и UTF-8 для кода ANSI.
target_link_options(EmulkonTray PRIVATE "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")

enable_testing()
add_executable(tray_unit tests/unit.cpp)
target_link_libraries(tray_unit PRIVATE tray_core)
add_test(NAME tray_unit COMMAND tray_unit)
add_test(NAME tray_render COMMAND EmulkonTray --render "${CMAKE_CURRENT_BINARY_DIR}/sample.bmp")
add_test(NAME tray_render_terminal COMMAND EmulkonTray --render-terminal "${CMAKE_CURRENT_BINARY_DIR}/terminal.bmp")
add_test(NAME tray_render_scales COMMAND EmulkonTray --render-scales "${CMAKE_CURRENT_BINARY_DIR}/scales.bmp")
