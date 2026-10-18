# Оснастка MMC «Эмулкон» (Windows x64): EmulkonSnapIn.dll. Сборка — console/build.ps1.
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" EMULKON_PRODUCT_VERSION LIMIT_COUNT 1)
project(EmulkonSnapIn VERSION ${EMULKON_PRODUCT_VERSION} LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Оснастка — только Windows x64 (mmc.exe 64-битный)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMPONENT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../component/src")

# Логика без COM: сводка, реестр, служба, HTTP. JSON и HTTP — общие с компонентой.
add_library(console_core STATIC
  src/summary.cpp
  src/system.cpp
  src/admin.cpp
  src/inventory.cpp
  src/records.cpp
  src/maintenance.cpp
  src/taskdialog.cpp
  "${COMPONENT_SRC}/json.cpp"
  "${COMPONENT_SRC}/text.cpp"
  "${COMPONENT_SRC}/http_winhttp.cpp")
target_include_directories(console_core PUBLIC src "${COMPONENT_SRC}")
target_compile_definitions(console_core PUBLIC EMULKON_VERSION="${PROJECT_VERSION}"
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(console_core PUBLIC /utf-8 /EHsc /W3)
target_link_libraries(console_core PUBLIC winhttp advapi32 bcrypt shell32 ole32)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../res/emulkon.ico" "${CMAKE_CURRENT_BINARY_DIR}/emulkon.ico" COPYONLY)
configure_file(src/comctl6.manifest "${CMAKE_CURRENT_BINARY_DIR}/comctl6.manifest" COPYONLY)
configure_file(src/console.rc.in "${CMAKE_CURRENT_BINARY_DIR}/console.rc" @ONLY)
add_library(EmulkonSnapIn SHARED src/snapin.cpp src/dialogs.cpp src/snapin.def "${CMAKE_CURRENT_BINARY_DIR}/console.rc")
target_link_libraries(EmulkonSnapIn PRIVATE console_core mmc ole32 shell32 user32 gdi32 comctl32 comdlg32 uuid)
# Отладка: журнал вызовов MMC в %TEMP%\emulkon-snapin.log.
option(EMULKON_SNAPIN_TRACE "Журнал вызовов оснастки" OFF)
if(EMULKON_SNAPIN_TRACE)
  target_compile_definitions(EmulkonSnapIn PRIVATE EMULKON_SNAPIN_TRACE)
endif()

enable_testing()
# Окно новой версии (TaskDialog) проверяется в модульном тесте: ему нужен тот же манифест ресурсом 3.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/unit.rc" "3 24 \"${CMAKE_CURRENT_BINARY_DIR}/comctl6.manifest\"
")
add_executable(console_unit tests/unit.cpp "${CMAKE_CURRENT_BINARY_DIR}/unit.rc")
target_link_libraries(console_unit PRIVATE console_core user32 gdi32)
add_test(NAME console_unit COMMAND console_unit)

# Живой тест против отдельного сервера: запускает tests/live.ps1 (его зовёт build.ps1), не ctest.
add_executable(console_live tests/live.cpp)
target_link_libraries(console_live PRIVATE console_core)
