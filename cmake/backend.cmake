find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
set(TTP_MKV_BACKEND_CACHE "${CMAKE_CURRENT_BINARY_DIR}/backend" CACHE PATH "Private FFmpeg source and static libraries")
set(TTP_MKV_FFMPEG_SOURCE "" CACHE PATH "Optional complete FFmpeg 9.0.2 source; copied before patching")
set(TTP_MKV_BASH "" CACHE FILEPATH "Optional Git for Windows bash.exe")
set(_backend_args)
if(NOT TTP_MKV_FFMPEG_SOURCE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/configure")
  set(TTP_MKV_FFMPEG_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
endif()
if(TTP_MKV_FFMPEG_SOURCE)
  list(APPEND _backend_args --source "${TTP_MKV_FFMPEG_SOURCE}")
endif()
if(TTP_MKV_BASH)
  list(APPEND _backend_args --bash "${TTP_MKV_BASH}")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/make-4.4.1.tar.gz")
  list(APPEND _backend_args --make-archive "${CMAKE_CURRENT_SOURCE_DIR}/third_party/make-4.4.1.tar.gz")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/build_matroska.py"
  --build "${TTP_MKV_BACKEND_CACHE}" --vs "${CMAKE_GENERATOR_INSTANCE}"
  --ltl "${ttplayer_vc_ltl_SOURCE_DIR}" ${_backend_args}
  COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/backend-path.txt" "${TTP_MKV_BACKEND_CACHE}")
add_library(mkv_backend INTERFACE)
target_include_directories(mkv_backend INTERFACE "${TTP_MKV_BACKEND_CACHE}/ffmpeg-9.0.2"
  "${TTP_MKV_BACKEND_CACHE}/static")
target_link_libraries(mkv_backend INTERFACE
  "${TTP_MKV_BACKEND_CACHE}/static/libavformat/avformat.lib"
  "${TTP_MKV_BACKEND_CACHE}/static/libavcodec/avcodec.lib"
  "${TTP_MKV_BACKEND_CACHE}/static/libswresample/swresample.lib"
  "${TTP_MKV_BACKEND_CACHE}/static/libavutil/avutil.lib" bcrypt)
