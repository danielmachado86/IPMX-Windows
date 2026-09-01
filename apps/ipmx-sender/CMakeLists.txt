find_package(X264 REQUIRED)

add_library(ipmx_sender_codec STATIC
  src/x264_encoder.cpp
  src/frame_transmitter.cpp
)
add_library(ipmx::sender_codec ALIAS ipmx_sender_codec)
target_include_directories(ipmx_sender_codec PUBLIC include)
target_link_libraries(ipmx_sender_codec PUBLIC ipmx::core X264::X264
  PRIVATE ipmx::project_options ipmx::project_warnings avrt winmm)

add_executable(ipmx_sender
  src/main.cpp
  src/test_pattern.cpp
  src/wgc_capture.cpp
)
set_target_properties(ipmx_sender PROPERTIES OUTPUT_NAME "ipmx-sender")
target_include_directories(ipmx_sender PRIVATE include)
target_link_libraries(ipmx_sender PRIVATE
  ipmx::sender_codec
  ipmx::project_options
  ipmx::project_warnings
  d3d11
  dxgi
  windowsapp
  ole32
)

if(X264_RUNTIME_LIBRARY)
  add_custom_command(TARGET ipmx_sender POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:X264::X264>"
      "$<TARGET_FILE_DIR:ipmx_sender>"
    COMMENT "Copying the x264 runtime beside ipmx-sender"
  )
endif()

install(TARGETS ipmx_sender RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
