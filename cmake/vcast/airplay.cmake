# AirPlay screen-mirroring receiver: UxPlay's protocol library (GPL-3.0) with
# libplist (LGPL-2.1), llhttp (MIT), playfair (GPL-3.0) and orlp/ed25519 (zlib),
# all built from third_party/ as one static library, `vcast-airplay`, with the
# small C API in src/airplay/vcast-airplay.h. Crypto is mbedTLS (from
# cmake/vcast/mbedtls.cmake, which must be included first): no OpenSSL, no system
# libplist, no Bonjour/Avahi (UxPlay's built-in mDNS responder on Windows and
# Linux, the system dns_sd in libSystem on macOS).
include_guard(GLOBAL)

if(NOT TARGET mbedcrypto)
  message(FATAL_ERROR "include(cmake/vcast/mbedtls.cmake) before cmake/vcast/airplay.cmake")
endif()

set(_vcast_ap_root "${CMAKE_CURRENT_LIST_DIR}/../..")
cmake_path(NORMAL_PATH _vcast_ap_root)
set(_vcast_ap_tp "${_vcast_ap_root}/third_party")
set(_vcast_ap_ux "${_vcast_ap_tp}/uxplay/lib")
set(_vcast_ap_plist "${_vcast_ap_tp}/libplist")
set(_vcast_ap_ed "${_vcast_ap_tp}/ed25519")

# Third-party code: no warnings (and never warnings-as-errors), PIC, nothing host specific.
function(_vcast_airplay_third_party target)
  set_target_properties(
    ${target}
    PROPERTIES
      FOLDER "third_party"
      POSITION_INDEPENDENT_CODE ON
      COMPILE_WARNING_AS_ERROR OFF
      C_VISIBILITY_PRESET hidden
  )
  # Drop the plugin's warning level instead of overriding it (MSVC would warn D9025).
  get_target_property(_opts ${target} COMPILE_OPTIONS)
  if(_opts)
    list(REMOVE_ITEM _opts /W3 /W4 /WX -Werror)
    set_target_properties(${target} PROPERTIES COMPILE_OPTIONS "${_opts}")
  endif()
  target_compile_options(${target} PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>)
endfunction()

# --- libplist 2.7.0 (binary + XML plists) -------------------------------------
add_library(
  vcast_airplay_plist
  OBJECT
  ${_vcast_ap_plist}/libcnary/node.c
  ${_vcast_ap_plist}/libcnary/node_list.c
  ${_vcast_ap_plist}/src/base64.c
  ${_vcast_ap_plist}/src/bplist.c
  ${_vcast_ap_plist}/src/bytearray.c
  ${_vcast_ap_plist}/src/hashtable.c
  ${_vcast_ap_plist}/src/jplist.c
  ${_vcast_ap_plist}/src/jsmn.c
  ${_vcast_ap_plist}/src/oplist.c
  ${_vcast_ap_plist}/src/out-default.c
  ${_vcast_ap_plist}/src/out-limd.c
  ${_vcast_ap_plist}/src/out-plutil.c
  ${_vcast_ap_plist}/src/plist.c
  ${_vcast_ap_plist}/src/ptrarray.c
  ${_vcast_ap_plist}/src/time64.c
  ${_vcast_ap_plist}/src/xplist.c
)
_vcast_airplay_third_party(vcast_airplay_plist)
target_include_directories(
  vcast_airplay_plist
  PRIVATE ${_vcast_ap_plist}/src ${_vcast_ap_plist}/libcnary/include
  PUBLIC ${_vcast_ap_plist}/include
)
# LIBPLIST_STATIC: no dllimport/dllexport in plist/plist.h (consumers need it too).
target_compile_definitions(vcast_airplay_plist PUBLIC LIBPLIST_STATIC PRIVATE PACKAGE_VERSION="2.7.0")
if(NOT WIN32)
  target_compile_definitions(vcast_airplay_plist PRIVATE HAVE_STRNDUP HAVE_GMTIME_R HAVE_LOCALTIME_R HAVE_MEMMEM)
endif()

# --- orlp/ed25519 (under a private prefix, see third_party/ed25519/vcast_ed25519.h) ---
set(
  VCAST_ED25519_RENAMES
  ED25519_NO_SEED=1
  ed25519_create_keypair=vcast_orlp_ed25519_create_keypair
  ed25519_sign=vcast_orlp_ed25519_sign
  ed25519_verify=vcast_orlp_ed25519_verify
  ed25519_add_scalar=vcast_orlp_ed25519_add_scalar
  ed25519_key_exchange=vcast_orlp_ed25519_key_exchange
  sha512_init=vcast_orlp_sha512_init
  sha512_update=vcast_orlp_sha512_update
  sha512_final=vcast_orlp_sha512_final
  sha512=vcast_orlp_sha512
)
add_library(
  vcast_airplay_ed25519
  OBJECT
  ${_vcast_ap_ed}/src/fe.c
  ${_vcast_ap_ed}/src/ge.c
  ${_vcast_ap_ed}/src/keypair.c
  ${_vcast_ap_ed}/src/sc.c
  ${_vcast_ap_ed}/src/sha512.c
  ${_vcast_ap_ed}/src/sign.c
  ${_vcast_ap_ed}/src/verify.c
)
_vcast_airplay_third_party(vcast_airplay_ed25519)
target_compile_definitions(vcast_airplay_ed25519 PRIVATE ${VCAST_ED25519_RENAMES})

# --- UxPlay lib/ (+ llhttp, playfair, mDNS) -----------------------------------
set(
  _vcast_ap_ux_sources
  ${_vcast_ap_ux}/airplay_video.c
  ${_vcast_ap_ux}/byteutils.c
  ${_vcast_ap_ux}/compat.c
  ${_vcast_ap_ux}/crypto.c
  ${_vcast_ap_ux}/dnssd.c
  ${_vcast_ap_ux}/fairplay_playfair.c
  ${_vcast_ap_ux}/http_request.c
  ${_vcast_ap_ux}/http_response.c
  ${_vcast_ap_ux}/httpd.c
  ${_vcast_ap_ux}/logger.c
  ${_vcast_ap_ux}/mirror_buffer.c
  ${_vcast_ap_ux}/netutils.c
  ${_vcast_ap_ux}/pairing.c
  ${_vcast_ap_ux}/raop.c
  ${_vcast_ap_ux}/raop_buffer.c
  ${_vcast_ap_ux}/raop_ntp.c
  ${_vcast_ap_ux}/raop_rtp.c
  ${_vcast_ap_ux}/raop_rtp_mirror.c
  ${_vcast_ap_ux}/srp.c
  ${_vcast_ap_ux}/utils.c
  ${_vcast_ap_ux}/llhttp/api.c
  ${_vcast_ap_ux}/llhttp/http.c
  ${_vcast_ap_ux}/llhttp/llhttp.c
  ${_vcast_ap_ux}/playfair/hand_garble.c
  ${_vcast_ap_ux}/playfair/modified_md5.c
  ${_vcast_ap_ux}/playfair/omg_hax.c
  ${_vcast_ap_ux}/playfair/playfair.c
  ${_vcast_ap_ux}/playfair/sap_hash.c
)
if(APPLE)
  # mDNSResponder owns UDP 5353 on macOS: register through it (dns_sd.h, libSystem).
  list(APPEND _vcast_ap_ux_sources ${_vcast_ap_ux}/dns_sd/dns_sd.c)
else()
  # Self-contained responder: no Bonjour (Windows) or Avahi (Linux) needed.
  list(APPEND _vcast_ap_ux_sources ${_vcast_ap_ux}/mdnsd/mdnsd.c ${_vcast_ap_ux}/mdnsd/dnssd_mdnsd.c)
endif()

add_library(vcast_airplay_uxplay OBJECT ${_vcast_ap_ux_sources})
_vcast_airplay_third_party(vcast_airplay_uxplay)
target_include_directories(
  vcast_airplay_uxplay
  PRIVATE ${_vcast_ap_ux} ${_vcast_ap_ux}/llhttp ${_vcast_ap_ux}/playfair ${_vcast_ap_ed}
)
target_link_libraries(vcast_airplay_uxplay PRIVATE vcast_airplay_plist mbedcrypto)
# PLIST_210/PLIST_230: the vendored libplist is 2.7.0. LLHTTP_EXPORT: static, no dllexport.
target_compile_definitions(vcast_airplay_uxplay PRIVATE PLIST_210 PLIST_230 LLHTTP_EXPORT=)
if(WIN32)
  target_compile_definitions(
    vcast_airplay_uxplay
    PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
            _WINSOCK_DEPRECATED_NO_WARNINGS
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(vcast_airplay_uxplay PRIVATE _GNU_SOURCE)
elseif(CMAKE_SYSTEM_NAME MATCHES "BSD")
  target_compile_definitions(vcast_airplay_uxplay PRIVATE SYS_ENDIAN_H)
endif()

# --- the library ---------------------------------------------------------------
add_library(
  vcast-airplay
  STATIC
  ${_vcast_ap_root}/src/airplay/vcast-airplay.c
  ${_vcast_ap_root}/src/airplay/vcast-airplay.h
  $<TARGET_OBJECTS:vcast_airplay_uxplay>
  $<TARGET_OBJECTS:vcast_airplay_plist>
  $<TARGET_OBJECTS:vcast_airplay_ed25519>
)
set_target_properties(vcast-airplay PROPERTIES FOLDER "plugins/vcast" POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  vcast-airplay
  PUBLIC ${_vcast_ap_root}/src/airplay
  PRIVATE ${_vcast_ap_ux} ${_vcast_ap_plist}/include
)
target_compile_definitions(vcast-airplay PRIVATE LIBPLIST_STATIC)
target_link_libraries(vcast-airplay PUBLIC mbedcrypto)
if(WIN32)
  target_compile_definitions(vcast-airplay PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(vcast-airplay PUBLIC ws2_32 iphlpapi)
elseif(NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(vcast-airplay PUBLIC Threads::Threads m)
endif()

if(VCAST_BUILD_TESTS)
  add_subdirectory("${_vcast_ap_root}/tests/airplay" "${CMAKE_BINARY_DIR}/tests/airplay")
endif()
