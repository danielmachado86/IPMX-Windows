cmake_minimum_required(VERSION 3.25)

project(IPMXWindows VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "IPMXWindows currently supports Windows only")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/bin")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/lib")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/lib")
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(GNUInstallDirs)
include(ProjectOptions)

option(IPMX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(IPMX_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)

ipmx_configure_project_options()

add_subdirectory(src/ipmx-core)
add_subdirectory(apps/ipmx-sender)
add_subdirectory(apps/ipmx-receiver)

if(BUILD_TESTING)
  add_subdirectory(tests/IPMXCoreTests)
endif()
