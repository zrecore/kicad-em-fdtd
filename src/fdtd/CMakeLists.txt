add_library(fdtd_solver STATIC grid.cpp solver.cpp dft.cpp sparam.cpp ntff.cpp)
target_include_directories(fdtd_solver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(fdtd_solver PUBLIC fdtd_core PRIVATE fdtd_options)
