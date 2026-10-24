# mbedTLS (Apache-2.0): TLS for the browser cast page and crypto for AirPlay.
# Built statically from a pinned release so every platform uses the same version.
include_guard(GLOBAL)

include(FetchContent)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(INSTALL_MBEDTLS_HEADERS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  vcast_mbedtls
  GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
  GIT_TAG 068ff080b369adfac81509f9b57b2afabaf82dc5 # mbedtls-3.6.7
  GIT_SHALLOW FALSE
  EXCLUDE_FROM_ALL
)

# Keep the plugin's warnings-as-errors policy away from third-party code.
set(_vcast_saved_warn_as_error ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
set(_vcast_saved_pic ${CMAKE_POSITION_INDEPENDENT_CODE})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
FetchContent_MakeAvailable(vcast_mbedtls)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${_vcast_saved_warn_as_error})
set(CMAKE_POSITION_INDEPENDENT_CODE ${_vcast_saved_pic})

foreach(_t mbedtls mbedx509 mbedcrypto)
  if(TARGET ${_t})
    set_target_properties(${_t} PROPERTIES FOLDER "third_party" COMPILE_WARNING_AS_ERROR OFF)
  endif()
endforeach()
