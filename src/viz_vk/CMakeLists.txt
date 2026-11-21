# GPU volume renderer. Links vkcore only, so the CPU tests can use the factory without a device.
add_library(viz_vk STATIC gpu_volume.cpp gpu_slice.cpp)
target_include_directories(viz_vk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(viz_vk PUBLIC viz vkcore PRIVATE fdtd_options fdtd_shaders)
