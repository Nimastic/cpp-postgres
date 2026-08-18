cmake_minimum_required(VERSION 3.20)
project(cpp_postgres LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Core Postgres engine library
add_library(pg_core
    src/pager.cpp
    src/page.cpp
    src/heap.cpp
    src/tx.cpp
    src/mvcc.cpp
    src/vacuum.cpp
    src/btree.cpp
    src/buffer_pool.cpp
    src/wal.cpp
    src/toast.cpp
    src/engine.cpp
)

# Interactive SQL REPL CLI Application
add_executable(pg_cli src/main.cpp)
target_link_libraries(pg_cli PRIVATE pg_core)

# Item 1 test executable
add_executable(test_pager tests/test_pager.cpp)
target_link_libraries(test_pager PRIVATE pg_core)

# Item 2 test executable
add_executable(test_page tests/test_page.cpp)
target_link_libraries(test_page PRIVATE pg_core)

# Item 3 test executable
add_executable(test_heap tests/test_heap.cpp)
target_link_libraries(test_heap PRIVATE pg_core)

# Item 4 test executable
add_executable(test_mvcc tests/test_mvcc.cpp)
target_link_libraries(test_mvcc PRIVATE pg_core)

# Item 5 test executable
add_executable(test_vacuum tests/test_vacuum.cpp)
target_link_libraries(test_vacuum PRIVATE pg_core)

# Item 6 test executable
add_executable(test_index tests/test_index.cpp)
target_link_libraries(test_index PRIVATE pg_core)

# Item 7 test executable
add_executable(test_hot tests/test_hot.cpp)
target_link_libraries(test_hot PRIVATE pg_core)

# Item 8 test executable
add_executable(test_buffer_pool tests/test_buffer_pool.cpp)
target_link_libraries(test_buffer_pool PRIVATE pg_core)

# Item 9 test executable
add_executable(test_wal tests/test_wal.cpp)
target_link_libraries(test_wal PRIVATE pg_core)

# Item 10 test executable
add_executable(test_toast tests/test_toast.cpp)
target_link_libraries(test_toast PRIVATE pg_core)

# Item 11 test executable (End-to-End REPL Capstone)
add_executable(test_repl tests/test_repl.cpp)
target_link_libraries(test_repl PRIVATE pg_core)







