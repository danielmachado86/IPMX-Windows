add_executable(ipmx_load src/main.cpp)
set_target_properties(ipmx_load PROPERTIES OUTPUT_NAME "ipmx-load")
target_link_libraries(ipmx_load PRIVATE
  ipmx::project_options
  ipmx::project_warnings
  d3d11
  dxgi
)

install(TARGETS ipmx_load RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
