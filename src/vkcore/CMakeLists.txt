add_library(vkcore buffer.cpp compute.cpp context.cpp swapchain.cpp)
target_include_directories(vkcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vkcore PUBLIC Vulkan::Vulkan PRIVATE fdtd_options)
