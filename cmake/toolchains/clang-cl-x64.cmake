# Тулчейн для CI (GitHub Actions, windows-2022): clang-cl и lld-link из LLVM раннера, библиотеки MSVC и Windows SDK
# из его Visual Studio. Каталоги задаёт workflow: VCToolsInstallDir, WindowsSdkDir, WindowsSDKVersion (без
# INCLUDE/LIB от vcvars — одна среда на x64 и x86). Локально — портативный тулчейн C:\.dev\.tools (README).
# Архитектура: x64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(DEFINED ENV{LLVM_DIR_BIN})
  set(_LLVM "$ENV{LLVM_DIR_BIN}")
else()
  set(_LLVM "C:/Program Files/LLVM/bin")
endif()
file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" _VC)
file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" _SDK)
string(REGEX REPLACE "[/\\\\]+$" "" _SDKVER "$ENV{WindowsSDKVersion}")  # «10.0.26100.0\» → «10.0.26100.0»
if(NOT _VC OR NOT _SDK OR NOT _SDKVER)
  message(FATAL_ERROR "Нет VCToolsInstallDir, WindowsSdkDir или WindowsSDKVersion — их задаёт workflow CI")
endif()

set(CMAKE_C_COMPILER   "${_LLVM}/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${_LLVM}/clang-cl.exe")
set(CMAKE_LINKER       "${_LLVM}/lld-link.exe")
set(CMAKE_AR           "${_LLVM}/llvm-lib.exe")
set(CMAKE_RC_COMPILER  "${_LLVM}/llvm-rc.exe")
set(CMAKE_MT           "${_LLVM}/llvm-mt.exe")
set(CMAKE_C_COMPILER_TARGET   x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(_FLAGS "/vctoolsdir \"${_VC}\" /winsdkdir \"${_SDK}\" /winsdkversion ${_SDKVER}")
set(CMAKE_C_FLAGS_INIT   "${_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_FLAGS}")
foreach(_k EXE SHARED MODULE)
  set(CMAKE_${_k}_LINKER_FLAGS_INIT "/vctoolsdir:\"${_VC}\" /winsdkdir:\"${_SDK}\" /winsdkversion:${_SDKVER} /machine:x64")
endforeach()
# Статическая CRT: драйвер не должен зависеть от VC++ Redistributable на кассе.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
