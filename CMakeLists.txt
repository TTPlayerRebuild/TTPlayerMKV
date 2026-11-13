cmake_minimum_required(VERSION 3.24)
project(TtpMkv LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ttp_mkv requires MSVC and a Win32/x86 generator")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/backend.cmake)
find_program(TTP_MKV_POWERSHELL NAMES powershell pwsh REQUIRED)
set(TTP_MKV_BUILD_VERSION "" CACHE STRING "Date version yyyy.MM.dd[pN]; empty uses Beijing date")
set(_version_args)
if(TTP_MKV_BUILD_VERSION)
  list(APPEND _version_args -Version "${TTP_MKV_BUILD_VERSION}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_VERSION")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_VERSION" _bundle_version LIMIT_COUNT 1)
  list(APPEND _version_args -Version "${_bundle_version}")
endif()
set(_resource "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_custom_target(ttp_mkv_version
  COMMAND "${TTP_MKV_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_version.ps1" -Output "${_resource}" ${_version_args}
  BYPRODUCTS "${_resource}" VERBATIM)
add_library(ttp_mkv SHARED src/reader.cpp src/ttp_mkv.def "${_resource}")
add_dependencies(ttp_mkv ttp_mkv_version)
target_include_directories(ttp_mkv PRIVATE include)
target_compile_definitions(ttp_mkv PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttp_mkv PRIVATE /W4 /permissive- /EHsc /GR- /O1 /Os /Gy /Gw /GF
  /utf-8 /fp:precise /arch:SSE2)
target_link_libraries(ttp_mkv PRIVATE mkv_backend ole32 uuid)
target_link_options(ttp_mkv PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE
  /ENTRY:DllMainCRTStartupForYY_Thunks
  /alternatename:_YY_ThunksOriginalDllMainCRTStartup@12=__DllMainCRTStartup@12)
set_target_properties(ttp_mkv PROPERTIES PREFIX "" OUTPUT_NAME "ttp_mkv" INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
add_custom_command(TARGET ttp_mkv POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
    "$<TARGET_FILE:ttp_mkv>" --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
    --report "$<TARGET_FILE_DIR:ttp_mkv>/legacy-imports.json" VERBATIM)
install(TARGETS ttp_mkv RUNTIME DESTINATION AddIn)
