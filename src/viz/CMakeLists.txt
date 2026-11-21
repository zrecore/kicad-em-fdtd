add_library(viz STATIC png.cpp slice.cpp volume.cpp theme.cpp a11y.cpp access.cpp vtk_export.cpp isosurface.cpp volume_renderer.cpp)
target_include_directories(viz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(viz PUBLIC fdtd_solver PRIVATE fdtd_options)
if(APPLE)
  target_sources(viz PRIVATE a11y_mac.mm)
  target_link_libraries(viz PRIVATE "-framework CoreFoundation" "-framework AppKit")
endif()

# UI: Dear ImGui drawn by our CPU rasteriser (no Vulkan needed, so it is testable on any machine).
add_library(viz_ui STATIC imgui_soft.cpp)
target_include_directories(viz_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(viz_ui PUBLIC imgui_core PRIVATE fdtd_options)

# The window needs GLFW and Vulkan; the CPU-side pieces above do not (tests link only `viz` and `viz_ui`).
add_library(viz_window STATIC window.cpp screen_reader.cpp gpu_ui.cpp)
target_include_directories(viz_window PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(viz_window PUBLIC vkcore glfw PRIVATE imgui_vulkan fdtd_options)
if(FDTD_SCREEN_READER)
  target_compile_definitions(viz_window PRIVATE FDTD_SCREEN_READER)
  target_link_libraries(viz_window PRIVATE accesskit-static)
  if(APPLE)
    target_link_libraries(viz_window PRIVATE "-framework AppKit" "-framework Foundation")
  endif()
endif()
