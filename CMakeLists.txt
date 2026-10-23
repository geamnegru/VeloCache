cmake_minimum_required(VERSION 3.16)
project(VeloCache VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
endif()

set(SOURCES
    main.cpp
    storage.cpp
    client_handler.cpp
    server.cpp
    resp.cpp
    background_worker.cpp
    event_loop.cpp
)

if(APPLE OR FREEBSD OR OPENBSD OR NETBSD OR DRAGONFLYBSD)
    list(APPEND SOURCES event_loop_kqueue.cpp)
else()
    list(APPEND SOURCES event_loop_epoll.cpp)
endif()

add_executable(velocache ${SOURCES})
target_include_directories(velocache PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(velocache pthread)

add_custom_target(make_test
    COMMAND ${CMAKE_COMMAND} -E echo "Run tests with: ctest"
)

enable_testing()

# Unit tests
add_executable(storage_test
    tests/storage_test.cpp
    storage.cpp
)
target_include_directories(storage_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(storage_test pthread)
add_test(NAME storage_test COMMAND storage_test)

add_executable(resp_test
    tests/resp_test.cpp
    resp.cpp
    client_handler.cpp
    storage.cpp
)
target_include_directories(resp_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(resp_test pthread)
add_test(NAME resp_test COMMAND resp_test)

add_executable(background_worker_test
    tests/background_worker_test.cpp
    background_worker.cpp
)
target_include_directories(background_worker_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(background_worker_test pthread)
add_test(NAME background_worker_test COMMAND background_worker_test)

# Slow fsync test library is macOS-only: it relies on Darwin interposition.
if(APPLE)
    add_library(slow_fsync SHARED tests/slow_fsync.cpp)
    set_target_properties(slow_fsync PROPERTIES
        PREFIX ""
        SUFFIX ".dylib"
    )

    add_test(NAME background_io_py
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/background_io.py $<TARGET_FILE:velocache> $<TARGET_FILE:slow_fsync>
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()

# Integration tests (Python)
add_test(NAME integration_py
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:velocache>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(NAME resp_integration_py
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/resp_integration.py $<TARGET_FILE:velocache>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Install target
install(TARGETS velocache DESTINATION bin)

# Package configuration
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
