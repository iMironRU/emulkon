# Установщик Эмулкона для Windows x64. Сборка — setup/build.ps1 (его вызывает build.ps1 в корне).
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" EMULKON_PRODUCT_VERSION LIMIT_COUNT 1)
project(EmulkonSetup VERSION ${EMULKON_PRODUCT_VERSION} LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Установщик — только Windows x64")
endif()

set(PAYLOAD_DIR "" CACHE PATH "Файлы для установки: emulkon.exe, EmulkonTray.exe, EmulkonSnapIn.dll, драйвер, emulkon.ico")
if(NOT EXISTS "${PAYLOAD_DIR}/emulkon.exe")
  message(FATAL_ERROR "Нет полезной нагрузки (PAYLOAD_DIR=${PAYLOAD_DIR}): соберите через setup/build.ps1")
endif()
# SDK Native API 1С (заголовки ComponentBase.h и др.) в репозиторий не входит: каталог include — из -DONEC_SDK,
# переменной окружения ONEC_SDK (CI берёт его из секрета) или локального каталога разработчика.
if(DEFINED ENV{ONEC_SDK})
  set(_ONEC_SDK_DEFAULT "$ENV{ONEC_SDK}")
else()
  set(_ONEC_SDK_DEFAULT "C:/.dev/1C-AddIn-Native/_extract/vncomps/VNCOMPS/include")
endif()
set(ONEC_SDK "${_ONEC_SDK_DEFAULT}" CACHE PATH "Каталог include SDK Native API 1С")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMPONENT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../component/src")
set(TRAY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../tray/src")

# Логика без окна: пути, установка, проверки. Общее с треем (настройки) и компонентой (JSON, HTTP).
add_library(setup_core STATIC
  src/paths.cpp
  src/install.cpp
  src/service.cpp
  src/console.cpp
  src/checks.cpp
  src/probe.cpp
  "${TRAY_SRC}/config.cpp"
  "${TRAY_SRC}/doc.cpp"
  "${COMPONENT_SRC}/json.cpp"
  "${COMPONENT_SRC}/text.cpp"
  "${COMPONENT_SRC}/http_winhttp.cpp")
target_include_directories(setup_core PUBLIC src "${TRAY_SRC}" "${COMPONENT_SRC}" "${ONEC_SDK}")
target_compile_definitions(setup_core PUBLIC EMULKON_VERSION="${PROJECT_VERSION}"
  _WINDOWS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(setup_core PUBLIC /utf-8 /EHsc /W3)
target_link_libraries(setup_core PUBLIC winhttp shell32 ole32 oleaut32 advapi32 rstrtmgr iphlpapi ws2_32 user32)

# Драйвер Linux — в установщике, только если собран (CI): строка ресурса подставляется в setup.rc.
set(LINUX_DRIVER_RC "")
if(EXISTS "${PAYLOAD_DIR}/EmulkonDriver_${PROJECT_VERSION}_lin64.so")
  set(LINUX_DRIVER_RC "LINUX64 RCDATA \"${PAYLOAD_DIR}/EmulkonDriver_${PROJECT_VERSION}_lin64.so\"")
endif()
configure_file(src/setup.rc.in "${CMAKE_CURRENT_BINARY_DIR}/setup.rc" @ONLY)
add_executable(EmulkonSetup WIN32 src/main.cpp "${CMAKE_CURRENT_BINARY_DIR}/setup.rc")
target_link_libraries(EmulkonSetup PRIVATE setup_core comctl32 gdi32 uuid)
# Манифест: requireAdministrator — установка на машину, служба (docs/local-install.md §1.3)
# и визуальные стили Common Controls 6.
target_link_options(EmulkonSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
  "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")

enable_testing()
add_executable(setup_unit tests/unit.cpp)
target_link_libraries(setup_unit PRIVATE setup_core)
add_test(NAME setup_unit COMMAND setup_unit)
