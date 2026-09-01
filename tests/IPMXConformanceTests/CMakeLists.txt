find_package(X264 REQUIRED)

add_executable(ipmx_conformance_tests
  conformance_tests.cpp
)
target_include_directories(ipmx_conformance_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/apps/ipmx-sender/include
  ${CMAKE_SOURCE_DIR}/apps/ipmx-receiver/include
)
target_compile_definitions(ipmx_conformance_tests PRIVATE
  IPMX_CONFORMANCE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
target_link_libraries(ipmx_conformance_tests PRIVATE
  ipmx::core
  ipmx::project_options
  ipmx::project_warnings
  ipmx::sender_codec
  ipmx::receiver_codec
)

add_test(NAME IPMXConformanceTests COMMAND ipmx_conformance_tests)
