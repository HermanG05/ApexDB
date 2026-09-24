cmake_minimum_required(VERSION 3.16)
project(kv-store LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

set(KV_CORE_SOURCES
    src/avl.cpp
    src/hashtable.cpp
    src/heap.cpp
    src/thread_pool.cpp
    src/zset.cpp
    src/protocol.cpp
    src/kv_store.cpp
    src/sharding.cpp
    src/tcp_rpc.cpp
    src/server_loop.cpp
)

add_library(kv_core STATIC ${KV_CORE_SOURCES})
target_include_directories(kv_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(kv_core PUBLIC Threads::Threads)

add_executable(kv-server src/server.cpp)
target_link_libraries(kv-server PRIVATE kv_core)

add_executable(kv-shard src/shard_main.cpp)
target_link_libraries(kv-shard PRIVATE kv_core)

add_executable(kv-proxy src/proxy.cpp)
target_link_libraries(kv-proxy PRIVATE kv_core)

add_executable(kv-client src/client.cpp)
target_link_libraries(kv-client PRIVATE kv_core)

enable_testing()
foreach(test_name avl heap sharding store protocol)
    add_executable(test_${test_name} tests/test_${test_name}.cpp)
    target_link_libraries(test_${test_name} PRIVATE kv_core)
    target_compile_options(test_${test_name} PRIVATE -UNDEBUG)
    add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py --bin-dir $<TARGET_FILE_DIR:kv-server>)
    set_tests_properties(integration PROPERTIES TIMEOUT 60)
endif()

option(KV_BUILD_UI "Build the optional FLTK interface" ON)
if(KV_BUILD_UI)
    find_program(FLTK_CONFIG fltk-config)
    if(FLTK_CONFIG)
        execute_process(COMMAND ${FLTK_CONFIG} --cxxflags OUTPUT_VARIABLE FLTK_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${FLTK_CONFIG} --ldflags OUTPUT_VARIABLE FLTK_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
        separate_arguments(FLTK_CXXFLAGS_LIST NATIVE_COMMAND ${FLTK_CXXFLAGS})
        separate_arguments(FLTK_LDFLAGS_LIST NATIVE_COMMAND ${FLTK_LDFLAGS})
        add_executable(kv-ui src/ui.cpp)
        target_compile_options(kv-ui PRIVATE ${FLTK_CXXFLAGS_LIST})
        target_link_libraries(kv-ui PRIVATE kv_core ${FLTK_LDFLAGS_LIST})
    endif()
endif()
