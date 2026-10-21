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

# Выключено по умолчанию: обычная сборка не должна требовать сеть для FetchContent.
option(CANSIM_BUILD_TESTS "Build unit tests (downloads GoogleTest)" OFF)

if(CANSIM_BUILD_TESTS)
  enable_testing()

  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(cansim_tests
    tests/test_frame.cpp
    tests/test_controller.cpp
    tests/test_bus.cpp
  )
  target_link_libraries(cansim_tests PRIVATE cansim GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(cansim_tests)
endif()
