add_library(board STATIC snapshot.cpp voxelize.cpp mesh.cpp netwalk.cpp geometry_override.cpp)
target_include_directories(board PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(board PUBLIC fdtd_solver PRIVATE nlohmann_json::nlohmann_json fdtd_options)
