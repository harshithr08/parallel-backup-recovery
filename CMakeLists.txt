cmake_minimum_required(VERSION 3.20)
project(ipbrs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenMP REQUIRED COMPONENTS CXX)

add_library(ipbrs_core STATIC
    src/util.cpp
    src/sha256.cpp
    src/merkle.cpp
    src/fileio.cpp
    src/scheduler.cpp
    src/workload.cpp
    src/catalog.cpp
    src/engine.cpp
    src/dataset.cpp)
target_include_directories(ipbrs_core PUBLIC src)
target_link_libraries(ipbrs_core PUBLIC OpenMP::OpenMP_CXX)
if(MSVC)
    target_compile_options(ipbrs_core PUBLIC /W4 /utf-8 /permissive-)
    target_compile_definitions(ipbrs_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(ipbrs src/main.cpp)
target_link_libraries(ipbrs PRIVATE ipbrs_core)

enable_testing()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_main.cpp)
    add_executable(ipbrs_tests tests/test_main.cpp)
    target_link_libraries(ipbrs_tests PRIVATE ipbrs_core)
    add_test(NAME ipbrs_tests COMMAND ipbrs_tests)
endif()
