cmake_minimum_required(VERSION 3.16)
project(vent_axia_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/vent_axia)

# Only the ESPHome-free core is compiled here. If a file below ever stops
# building on the host, it has grown a dependency on esphome/ headers and
# belongs on the other side of that line -- see README, "Portable core".
set(CORE_SOURCES)
foreach(unit protocol parser diagnostics sequence)
  if(EXISTS ${COMPONENT_DIR}/${unit}.cpp)
    list(APPEND CORE_SOURCES ${COMPONENT_DIR}/${unit}.cpp)
  endif()
endforeach()

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

add_executable(run_tests ${TEST_SOURCES} ${CORE_SOURCES} main.cpp)
target_include_directories(run_tests PRIVATE ${COMPONENT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()
add_test(NAME unit COMMAND run_tests)
