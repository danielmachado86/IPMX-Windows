add_library(ipmx_receiver_codec STATIC
  src/mf_h264_decoder.cpp
)
add_library(ipmx::receiver_codec ALIAS ipmx_receiver_codec)
target_include_directories(ipmx_receiver_codec PUBLIC include)
target_link_libraries(ipmx_receiver_codec PUBLIC ipmx::core
  PRIVATE ipmx::project_options ipmx::project_warnings ole32 mf mfplat mfuuid wmcodecdspuuid)

add_executable(ipmx_receiver
  src/main.cpp
  src/d3d11_renderer.cpp
)
set_target_properties(ipmx_receiver PROPERTIES OUTPUT_NAME "ipmx-receiver")
target_include_directories(ipmx_receiver PRIVATE include)
target_link_libraries(ipmx_receiver PRIVATE
  ipmx::receiver_codec
  ipmx::project_options
  ipmx::project_warnings
  d3d11
  dxgi
  windowsapp
  ole32
)

install(TARGETS ipmx_receiver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
