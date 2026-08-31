add_executable(ipmx_receiver
  src/main.cpp
  src/d3d11_renderer.cpp
  src/mf_h264_decoder.cpp
)
set_target_properties(ipmx_receiver PROPERTIES OUTPUT_NAME "ipmx-receiver")
target_include_directories(ipmx_receiver PRIVATE include)
target_link_libraries(ipmx_receiver PRIVATE
  ipmx::core
  ipmx::project_options
  ipmx::project_warnings
  d3d11
  dxgi
  windowsapp
  ole32
  mf
  mfplat
  mfuuid
  wmcodecdspuuid
)

install(TARGETS ipmx_receiver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
