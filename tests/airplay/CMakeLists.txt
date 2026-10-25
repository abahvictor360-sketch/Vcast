# Tests for the AirPlay receiver library (built with -DVCAST_BUILD_TESTS=ON).
#   vcast-airplay-crypto-test   mbedTLS crypto port vs. test vectors and, if OpenSSL is
#                               found, the original OpenSSL implementation
#   vcast-airplay-smoke         runs a receiver and prints callbacks (used by airplay-e2e.py)

set(_vcast_ap_tp "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")

add_executable(vcast-airplay-crypto-test crypto-test.c)
target_include_directories(vcast-airplay-crypto-test PRIVATE ${_vcast_ap_tp}/uxplay/lib)
target_link_libraries(vcast-airplay-crypto-test PRIVATE vcast-airplay mbedcrypto)

find_package(OpenSSL QUIET COMPONENTS Crypto)
if(OpenSSL_FOUND)
  # The original UxPlay OpenSSL code, renamed, as a reference (test only: the shipped
  # library never links OpenSSL).
  add_library(vcast_airplay_openssl_reference OBJECT reference/crypto_openssl.c reference/srp_openssl.c)
  target_include_directories(
    vcast_airplay_openssl_reference
    BEFORE
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/reference ${_vcast_ap_tp}/uxplay/lib
  )
  if(MSVC)
    target_compile_options(
      vcast_airplay_openssl_reference
      PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/reference/ref_rename.h /W0
    )
  else()
    target_compile_options(
      vcast_airplay_openssl_reference
      PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/reference/ref_rename.h -w
    )
  endif()
  set_target_properties(vcast_airplay_openssl_reference PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  target_link_libraries(vcast_airplay_openssl_reference PRIVATE OpenSSL::Crypto)
  target_sources(vcast-airplay-crypto-test PRIVATE $<TARGET_OBJECTS:vcast_airplay_openssl_reference>)
  target_compile_definitions(vcast-airplay-crypto-test PRIVATE VCAST_HAVE_OPENSSL_REFERENCE)
  target_link_libraries(vcast-airplay-crypto-test PRIVATE OpenSSL::Crypto)
  message(STATUS "Vcast: AirPlay crypto test compares against OpenSSL ${OPENSSL_VERSION}")
endif()

add_executable(vcast-airplay-smoke airplay-smoke.c)
target_include_directories(vcast-airplay-smoke PRIVATE ${_vcast_ap_tp}/uxplay/lib)
target_link_libraries(vcast-airplay-smoke PRIVATE vcast-airplay)

enable_testing()
add_test(NAME vcast-airplay-crypto COMMAND vcast-airplay-crypto-test)
