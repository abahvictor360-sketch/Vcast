# FFmpeg decoding libraries. OBS ships these with the app; on Windows and macOS
# they come from the obs-deps package the build downloads, on Linux from the
# system (matching the distribution's OBS build).
include_guard(GLOBAL)

set(_vcast_ffmpeg_components avcodec avformat avutil swscale)

foreach(_c IN LISTS _vcast_ffmpeg_components)
  find_path(VCAST_FFMPEG_${_c}_INCLUDE_DIR lib${_c}/${_c}.h PATH_SUFFIXES include)
  find_library(VCAST_FFMPEG_${_c}_LIBRARY NAMES ${_c} lib${_c})
  if(NOT VCAST_FFMPEG_${_c}_INCLUDE_DIR OR NOT VCAST_FFMPEG_${_c}_LIBRARY)
    message(
      FATAL_ERROR
      "FFmpeg ${_c} not found (install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev on Linux)"
    )
  endif()
  if(NOT TARGET VcastFFmpeg::${_c})
    add_library(VcastFFmpeg::${_c} UNKNOWN IMPORTED)
    set_target_properties(
      VcastFFmpeg::${_c}
      PROPERTIES
        IMPORTED_LOCATION "${VCAST_FFMPEG_${_c}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${VCAST_FFMPEG_${_c}_INCLUDE_DIR}"
    )
  endif()
  message(STATUS "Vcast: FFmpeg ${_c}: ${VCAST_FFMPEG_${_c}_LIBRARY}")
endforeach()
