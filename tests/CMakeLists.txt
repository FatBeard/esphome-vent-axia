cmake_minimum_required(VERSION 3.16)
project(vent_axia_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/vent_axia)

# Every component source except the ESPHome glue is compiled here. If a file
# stops building on the host it has grown a dependency on esphome/ headers, and
# either the dependency or the file belongs on the other side of that line --
# see README, "Portable core". The exclusion list is deliberately short; adding
# to it should feel like a decision.
file(GLOB CORE_SOURCES ${COMPONENT_DIR}/*.cpp)
list(FILTER CORE_SOURCES EXCLUDE REGEX "/vent_axia\\.cpp$")

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

add_executable(run_tests ${TEST_SOURCES} ${CORE_SOURCES} main.cpp)
target_include_directories(run_tests PRIVATE ${COMPONENT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()
add_test(NAME unit COMMAND run_tests)
