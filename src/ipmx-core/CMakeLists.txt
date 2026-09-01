add_library(ipmx_core STATIC
  src/annexb.cpp
  src/bgra_to_nv12.cpp
  src/h264.cpp
  src/pcap.cpp
  src/rtp.cpp
  src/rtcp.cpp
  src/sdp.cpp
  src/traffic_shaper.cpp
  src/udp_multicast.cpp
)
add_library(ipmx::core ALIAS ipmx_core)

target_include_directories(ipmx_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(ipmx_core
  PUBLIC ipmx::project_options
  PRIVATE ipmx::project_warnings ws2_32 iphlpapi
)

install(TARGETS ipmx_core EXPORT IPMXWindowsTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
