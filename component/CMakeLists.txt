# Компонента-драйвер Эмулкон (Native API). Сборка — component/build.ps1 (Windows, clang-cl).
cmake_minimum_required(VERSION 3.20)
# Версия продукта — одна на сервер, драйвер, трей и установщик: файл VERSION в корне.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" EMULKON_PRODUCT_VERSION LIMIT_COUNT 1)
project(EmulkonDriver VERSION ${EMULKON_PRODUCT_VERSION} LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")

# SDK Native API 1С (заголовки ComponentBase.h и др.) в репозиторий не входит: каталог include — из -DONEC_SDK,
# переменной окружения ONEC_SDK (CI берёт его из секрета) или локального каталога разработчика.
if(DEFINED ENV{ONEC_SDK})
  set(_ONEC_SDK_DEFAULT "$ENV{ONEC_SDK}")
else()
  set(_ONEC_SDK_DEFAULT "C:/.dev/1C-AddIn-Native/_extract/vncomps/VNCOMPS/include")
endif()
set(ONEC_SDK "${_ONEC_SDK_DEFAULT}" CACHE PATH "Каталог include SDK Native API 1С")
if(NOT EXISTS "${ONEC_SDK}/ComponentBase.h")
  message(FATAL_ERROR "Не найден SDK Native API 1С (ComponentBase.h): укажите -DONEC_SDK=<каталог include>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(EMULKON_BITS 64)
else()
  set(EMULKON_BITS 32)
endif()

if(WIN32)
  set(EMULKON_DLL_NAME "EmulkonDriver_win${EMULKON_BITS}")
else()
  set(EMULKON_DLL_NAME "libEmulkonDriver_lin${EMULKON_BITS}")
endif()

# --- ядро: протокол, без зависимостей от 1С ---
add_library(emulkon_core STATIC
  src/json.cpp
  src/text.cpp
  src/methods.cpp
  src/device_client.cpp
  src/events.cpp
  src/update.cpp)
if(WIN32)
  target_sources(emulkon_core PRIVATE src/http_winhttp.cpp src/update_win.cpp)
  target_link_libraries(emulkon_core PUBLIC winhttp)
else()
  message(FATAL_ERROR "Linux: HTTP-транспорт компоненты ещё не реализован (TODO: libcurl, ARCHITECTURE §4.1)")
endif()
target_include_directories(emulkon_core PUBLIC src "${ONEC_SDK}")
target_compile_definitions(emulkon_core PUBLIC EMULKON_VERSION="${PROJECT_VERSION}" EMULKON_DLL_FILE="${EMULKON_DLL_NAME}.dll")
if(WIN32)
  # _WINDOWS обязателен: по нему SDK включает windows.h и ADDIN_API = __stdcall (важно для x86).
  target_compile_definitions(emulkon_core PUBLIC _WINDOWS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  target_compile_options(emulkon_core PUBLIC /utf-8 /EHsc /W3)
endif()

# --- сама компонента ---
add_library(EmulkonDriver SHARED src/driver.cpp src/exports.cpp)
target_link_libraries(EmulkonDriver PRIVATE emulkon_core)
set_target_properties(EmulkonDriver PROPERTIES OUTPUT_NAME "${EMULKON_DLL_NAME}" PREFIX "")
if(WIN32)
  configure_file(src/emulkon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/emulkon.rc" @ONLY)
  target_sources(EmulkonDriver PRIVATE src/emulkon.def "${CMAKE_CURRENT_BINARY_DIR}/emulkon.rc")
else()
  target_link_options(EmulkonDriver PRIVATE "-Wl,--no-undefined" "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/version.script")
endif()

# --- тесты ---
enable_testing()
add_executable(unit_tests tests/unit.cpp)
target_link_libraries(unit_tests PRIVATE emulkon_core)
add_test(NAME unit COMMAND unit_tests)

if(WIN32)
  # Стенд «как платформа 1С»: грузит DLL и вызывает методы через vtable.
  add_executable(harness tests/harness.cpp)
  target_include_directories(harness PRIVATE "${ONEC_SDK}")
  target_link_libraries(harness PRIVATE winhttp)
  target_compile_definitions(harness PRIVATE _WINDOWS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(MSVC)
    target_compile_options(harness PRIVATE /utf-8 /EHsc)
  endif()
  add_test(NAME harness_offline COMMAND harness "$<TARGET_FILE:EmulkonDriver>")
  add_test(NAME harness_update_redirect COMMAND harness --redirect "$<TARGET_FILE:EmulkonDriver>" "${CMAKE_CURRENT_BINARY_DIR}/update-cache-on" 1)
  add_test(NAME harness_update_disabled COMMAND harness --redirect "$<TARGET_FILE:EmulkonDriver>" "${CMAKE_CURRENT_BINARY_DIR}/update-cache-off" 0)
  set_tests_properties(harness_update_disabled PROPERTIES ENVIRONMENT "EMULKON_DRIVER_NO_UPDATE=1")
  if(DEFINED ENV{EMULKON_TEST_SERVER})
    add_test(NAME harness_server COMMAND harness "$<TARGET_FILE:EmulkonDriver>" "$ENV{EMULKON_TEST_SERVER}")
  endif()
endif()
