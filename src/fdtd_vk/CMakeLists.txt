add_library(fdtd_vk STATIC gpu_solver.cpp gpu_phasor.cpp gpu_line2d.cpp)
target_include_directories(fdtd_vk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(fdtd_vk PUBLIC vkcore fdtd_solver sim PRIVATE fdtd_options fdtd_shaders)
