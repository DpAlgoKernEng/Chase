# 示例配置

# 最小服务器示例（单线程）
add_executable(minimal_server minimal_server.c)
target_link_libraries(minimal_server chase_core)

# 多线程服务器示例
add_executable(threaded_server threaded_server.c)
target_link_libraries(threaded_server chase_core pthread)