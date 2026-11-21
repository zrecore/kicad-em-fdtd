add_executable(emfdtd main.cpp ui.cpp)
    target_link_libraries(emfdtd PRIVATE fdtd_vk viz_vk vkcore board sim viz viz_ui viz_window appcore imgui_core
                              fdtd_options fdtd_shaders fdtd_version)
