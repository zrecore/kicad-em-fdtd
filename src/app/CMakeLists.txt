add_library(appcore STATIC settings.cpp paths.cpp prepare.cpp results.cpp run_archive.cpp selection_store.cpp sweep.cpp probes.cpp)
target_include_directories(appcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(appcore PUBLIC sim board PRIVATE nlohmann_json::nlohmann_json fdtd_options)
target_compile_definitions(appcore PUBLIC FDTD_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
