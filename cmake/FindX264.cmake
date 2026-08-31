find_path(X264_INCLUDE_DIR NAMES x264.h)
find_library(X264_LIBRARY NAMES x264 libx264)

if(X264_LIBRARY)
  get_filename_component(_x264_library_directory "${X264_LIBRARY}" DIRECTORY)
  get_filename_component(_x264_prefix "${_x264_library_directory}" DIRECTORY)
  file(GLOB X264_RUNTIME_LIBRARY LIST_DIRECTORIES FALSE "${_x264_prefix}/bin/*x264*.dll")
  list(LENGTH X264_RUNTIME_LIBRARY _x264_runtime_count)
  if(_x264_runtime_count GREATER 1)
    list(GET X264_RUNTIME_LIBRARY 0 X264_RUNTIME_LIBRARY)
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(X264 REQUIRED_VARS X264_LIBRARY X264_INCLUDE_DIR)

if(X264_FOUND AND NOT TARGET X264::X264)
  if(X264_RUNTIME_LIBRARY)
    add_library(X264::X264 SHARED IMPORTED)
    set_target_properties(X264::X264 PROPERTIES
      IMPORTED_IMPLIB "${X264_LIBRARY}"
      IMPORTED_LOCATION "${X264_RUNTIME_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${X264_INCLUDE_DIR}"
    )
  else()
    add_library(X264::X264 UNKNOWN IMPORTED)
    set_target_properties(X264::X264 PROPERTIES
      IMPORTED_LOCATION "${X264_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${X264_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(X264_INCLUDE_DIR X264_LIBRARY X264_RUNTIME_LIBRARY)
