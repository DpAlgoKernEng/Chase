# 示例配置

# 最小服务器示例（单线程）
add_executable(minimal_server minimal_server.c)
target_link_libraries(minimal_server chase_core)

# 生产级服务器示例（SO_REUSEPORT + Master/Worker）
add_executable(production_server production_server.c)
target_link_libraries(production_server chase_core)

# libevent 对照服务器（用于性能对比测试）
option(ENABLE_LIBEVENT_EXAMPLE "Build libevent comparison server" ON)
if(ENABLE_LIBEVENT_EXAMPLE)
    find_package(Libevent CONFIG QUIET)
    if(Libevent_FOUND)
        add_executable(libevent_server libevent_server.c)
        target_link_libraries(libevent_server Libevent::core Libevent::extra)
        message(STATUS "libevent example enabled - libevent found")
    else()
        message(STATUS "libevent example disabled - libevent not found")
    endif()
endif()