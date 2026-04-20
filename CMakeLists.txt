cmake_minimum_required(VERSION 3.19)

project(Chase
    VERSION 0.1.0
    DESCRIPTION "High-performance HTTP/1.1 server library"
    LANGUAGES C
)

# C11 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 选项配置
option(ENABLE_TESTS "Enable unit tests" ON)
option(ENABLE_COVERAGE "Enable code coverage" OFF)
option(ENABLE_EXAMPLES "Build example programs" ON)
option(ENABLE_VALGRIND "Enable Valgrind memory check" ON)

# 查找依赖
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# 编译选项
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(ENABLE_COVERAGE)
        # macOS 使用 LLVM coverage (推荐)
        if(APPLE)
            add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
            add_link_options(-fprofile-instr-generate -fcoverage-mapping)
        else()
            # Linux 使用 gcov + lcov
            add_compile_options(-fprofile-arcs -ftest-coverage)
            add_link_options(-fprofile-arcs -ftest-coverage)
        endif()
    endif()
endif()

# 定义库源文件
set(CHASE_CORE_HEADERS
    include/eventloop.h
    include/timer.h
    include/connection.h
    include/http_parser.h
    include/router.h
    include/error.h
)

set(CHASE_CORE_SOURCES
    src/eventloop.c
    src/timer.c
    src/connection.c
    src/http_parser.c
    src/router.c
    src/error.c
)

# 创建核心库
add_library(chase_core STATIC ${CHASE_CORE_SOURCES} ${CHASE_CORE_HEADERS})
target_include_directories(chase_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(chase_core
    PUBLIC OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB
)

# 安装配置
include(GNUInstallDirs)
install(TARGETS chase_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${CHASE_CORE_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 测试配置
if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# 示例配置
if(ENABLE_EXAMPLES)
    add_subdirectory(examples)
endif()

# 版本信息
message(STATUS "Chase HTTP Server Library v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Tests: ${ENABLE_TESTS}")
message(STATUS "Coverage: ${ENABLE_COVERAGE}")
message(STATUS "Examples: ${ENABLE_EXAMPLES}")
message(STATUS "Valgrind: ${ENABLE_VALGRIND}")

# Valgrind 内存检测目标
if(ENABLE_VALGRIND AND ENABLE_TESTS)
    find_program(VALGRIND_EXECUTABLE valgrind)
    if(VALGRIND_EXECUTABLE)
        add_custom_target(valgrind
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_valgrind.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running Valgrind memory check on all tests"
            VERBATIM
        )
        message(STATUS "Valgrind found: ${VALGRIND_EXECUTABLE}")
    else()
        message(STATUS "Valgrind not found - valgrind target disabled")
    endif()
endif()