cmake_minimum_required(VERSION 3.20)
project(cpp_postgres LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Core Postgres engine library
add_library(pg_core
    src/pager.cpp
)

# Item 1 test executable
add_executable(test_pager tests/test_pager.cpp)
target_link_libraries(test_pager PRIVATE pg_core)
