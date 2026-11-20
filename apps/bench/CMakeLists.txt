add_executable(bench bench.cpp)
target_link_libraries(bench PRIVATE fdtd_vk vkcore fdtd_options fdtd_shaders)
