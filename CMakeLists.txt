cmake_minimum_required(VERSION 3.16)
project(linuxcan VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CANSIM_SOURCES
  src/cansim/frame.cpp
  src/cansim/controller.cpp
  src/cansim/bus.cpp
  src/cansim/wire_protocol.cpp
  src/cansim/bus_server.cpp
  src/cansim/bus_client.cpp
)

add_library(cansim STATIC ${CANSIM_SOURCES})
target_include_directories(cansim PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(cansim PUBLIC pthread)

add_executable(linuxcan
  src/main.cpp
  src/ecu.cpp
  src/ecu_config.cpp
  src/can_bus.cpp
  src/encoding.cpp
  src/hooks.cpp
  src/profiles.cpp
  src/http_server.cpp
)

target_include_directories(linuxcan PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_definitions(linuxcan PRIVATE
  LINUXCAN_VERSION="${PROJECT_VERSION}"
  LINUXCAN_WEB_DIR="${CMAKE_SOURCE_DIR}/web"
)

target_link_libraries(linuxcan PRIVATE cansim pthread)

add_executable(linuxcan-bussim
  src/cansim/main_bussim.cpp
)
target_link_libraries(linuxcan-bussim PRIVATE cansim pthread)

install(TARGETS linuxcan linuxcan-bussim RUNTIME DESTINATION bin)
