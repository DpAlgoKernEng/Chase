# 示例配置

# 最小服务器示例（单线程）
add_executable(minimal_server minimal_server.c)
target_link_libraries(minimal_server chase_core)

# 生产级服务器示例（SO_REUSEPORT + Master/Worker）
add_executable(production_server production_server.c)
target_link_libraries(production_server chase_core)