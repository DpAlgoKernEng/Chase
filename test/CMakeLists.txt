# 测试配置
enable_testing()

# 测试源文件 - Phase 1 核心模块测试
set(TEST_SOURCES_PHASE1
    integration/c_core/test_eventloop.c
    integration/c_core/test_timer.c
    integration/c_core/test_http_parser.c
    integration/c_core/test_router.c
    integration/c_core/test_connection.c
    integration/c_core/test_error.c
    integration/c_core/test_boundary.c
    integration/c_core/test_fileserve.c
    integration/c_core/test_connection_pool.c
    integration/c_core/test_response.c
    integration/c_core/test_handler.c
    integration/c_core/test_server.c
)

# 测试源文件 - Phase 2 进程管理专项测试
set(TEST_SOURCES_PHASE2
    integration/c_core/test_process_mgmt.c
    integration/c_core/test_worker_crash.c
    integration/c_core/test_signal_handling.c
)

# 测试源文件 - Phase 3 Keep-Alive + Range 测试
set(TEST_SOURCES_PHASE3
    integration/c_core/test_keepalive.c
    integration/c_core/test_chunked.c
    integration/c_core/test_range.c
)

# 测试源文件 - Phase 4 SSL + VHost + Config 测试
set(TEST_SOURCES_PHASE4
    integration/c_core/test_vhost.c
    integration/c_core/test_config.c
    integration/c_core/test_ssl_wrap.c
)

# 测试源文件 - Phase 1 + Phase 2 联合测试
set(TEST_SOURCES_INTEGRATION
    integration/c_core/test_phase1_phase2_integration.c
)

# 合并所有测试
set(TEST_SOURCES
    ${TEST_SOURCES_PHASE1}
    ${TEST_SOURCES_PHASE2}
    ${TEST_SOURCES_PHASE3}
    ${TEST_SOURCES_PHASE4}
    ${TEST_SOURCES_INTEGRATION}
)

# 创建测试可执行文件
foreach(TEST_SRC IN LISTS TEST_SOURCES)
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} chase_core pthread)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()