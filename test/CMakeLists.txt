# 测试配置
enable_testing()

# 测试源文件
set(TEST_SOURCES
    integration/c_core/test_eventloop.c
    integration/c_core/test_timer.c
    integration/c_core/test_http_parser.c
    integration/c_core/test_router.c
    integration/c_core/test_connection.c
    integration/c_core/test_error.c
)

# 创建测试可执行文件
foreach(TEST_SRC IN LISTS TEST_SOURCES)
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} chase_core)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()