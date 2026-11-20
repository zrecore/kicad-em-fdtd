add_library(sim STATIC simulation.cpp)
target_include_directories(sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sim PUBLIC board fdtd_solver PRIVATE fdtd_options)
