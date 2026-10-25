cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(VCAST_BUILD_TESTS "Build the headless Vcast end-to-end test program" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

include(cmake/vcast/mbedtls.cmake)
include(cmake/vcast/ffmpeg.cmake)
include(cmake/vcast/airplay.cmake)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE vcast-airplay)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE mbedtls mbedx509 mbedcrypto VcastFFmpeg::avcodec VcastFFmpeg::avformat VcastFFmpeg::avutil
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Keep our statically linked mbedTLS private so it never clashes with another copy in OBS.
  target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# The Vcast dock needs the OBS frontend API and Qt.
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
# AUTOMOC_COMPILER_PREDEFINES runs clang++ inside an Xcode script phase, where
# Xcode 26 exports several *_DEPLOYMENT_TARGET variables that make clang abort.
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON AUTOMOC_COMPILER_PREDEFINES OFF
)

# Platform independent casting core: MPEG-TS muxer, HTTP stream server,
# DLNA/UPnP discovery + control and the OBS output that ties them together.
set(
  VCAST_CORE_SOURCES
  src/cast-manager.cpp
  src/cast-manager.h
  src/dlna.cpp
  src/dlna.h
  src/http-client.cpp
  src/http-client.h
  src/http-server.cpp
  src/http-server.h
  src/net-util.cpp
  src/net-util.h
  src/receiver/app-ingest.cpp
  src/receiver/app-ingest.h
  src/receiver/container-ingest.cpp
  src/receiver/container-ingest.h
  src/receiver/receiver.cpp
  src/receiver/receiver.h
  src/receiver/receiver-source.cpp
  src/receiver/receiver-source.h
  src/receiver/stream-decoder.cpp
  src/receiver/stream-decoder.h
  src/receiver/web-server.cpp
  src/receiver/web-server.h
  src/ts-muxer.cpp
  src/ts-muxer.h
  src/vcast-output.cpp
  src/vcast-output.h
  src/vcast-settings.cpp
  src/vcast-settings.h
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    ${VCAST_CORE_SOURCES}
    src/plugin-main.cpp
    src/ui/settings-dialog.cpp
    src/ui/settings-dialog.h
    src/ui/vcast-dock.cpp
    src/ui/vcast-dock.h
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src)

if(WIN32)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 iphlpapi)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(VCAST_BUILD_TESTS)
  add_executable(vcast-e2e-test tests/e2e-headless.cpp ${VCAST_CORE_SOURCES})
  target_include_directories(vcast-e2e-test PRIVATE src)
  target_link_libraries(
    vcast-e2e-test
    PRIVATE
      OBS::libobs
      plugin-support
      mbedtls
      mbedx509
      mbedcrypto
      VcastFFmpeg::avcodec
      VcastFFmpeg::avformat
      VcastFFmpeg::avutil
  )
  add_executable(vcast-receiver-test tests/receiver-headless.cpp ${VCAST_CORE_SOURCES})
  target_include_directories(vcast-receiver-test PRIVATE src)
  target_link_libraries(
    vcast-receiver-test
    PRIVATE
      OBS::libobs
      plugin-support
      mbedtls
      mbedx509
      mbedcrypto
      VcastFFmpeg::avcodec
      VcastFFmpeg::avformat
      VcastFFmpeg::avutil
  )

  if(WIN32)
    target_compile_definitions(vcast-e2e-test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(vcast-e2e-test PRIVATE ws2_32 iphlpapi)
  endif()
endif()
