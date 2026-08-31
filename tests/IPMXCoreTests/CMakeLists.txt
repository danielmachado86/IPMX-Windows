add_executable(ipmx_core_tests phase0_tests.cpp)
target_link_libraries(ipmx_core_tests PRIVATE
  ipmx::core
  ipmx::project_options
  ipmx::project_warnings
)

add_test(NAME IPMXCoreTests COMMAND ipmx_core_tests)
