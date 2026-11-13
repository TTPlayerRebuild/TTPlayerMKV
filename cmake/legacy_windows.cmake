set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "Universal Release build" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
include(FetchContent)
FetchContent_Declare(ttplayer_yy_thunks
  URL https://github.com/Chuyu-Team/YY-Thunks/releases/download/v1.2.2/YY-Thunks-Objs.zip
  URL_HASH SHA256=518ed7ef4825e8a41997fbccfa2c8090cf31a6038fd51520a2e49886f947f9fc
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(ttplayer_vc_ltl
  URL https://github.com/Chuyu-Team/VC-LTL5/releases/download/v5.3.1/VC-LTL-Binary.7z
  URL_HASH SHA256=7a18799ed3aa84a225610a5447a56bc534c5c98ccb8dec05caba0e3f633431ad
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(ttplayer_yy_thunks ttplayer_vc_ltl)
set(VC_LTL_Root "${ttplayer_vc_ltl_SOURCE_DIR}")
set(WindowsTargetPlatformMinVersion "5.1.2600.0")
set(SupportLTL true)
include("${VC_LTL_Root}/config/config.cmake")
if(NOT InternalLTLCRTVersion STREQUAL "5.1.2600.0")
  message(FATAL_ERROR "ttp_mkv requires the XP VC-LTL runtime")
endif()
add_link_options("${ttplayer_yy_thunks_SOURCE_DIR}/objs/x86/YY_Thunks_for_WinXP.obj"
  /OSVERSION:5.1 /SUBSYSTEM:WINDOWS,5.01)
