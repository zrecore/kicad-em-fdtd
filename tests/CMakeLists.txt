include(FetchContent)
FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.16.1 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

# Labels: `ctest -L cpu` runs anywhere; `ctest -L gpu` needs a GPU and runs under the GPU lock.
add_executable(tests_cpu cpu/units_test.cpp cpu/solver_test.cpp cpu/boundary_test.cpp cpu/board_test.cpp cpu/sim_test.cpp cpu/mesh_test.cpp cpu/viz_test.cpp cpu/settings_test.cpp cpu/volume_test.cpp cpu/phasor_test.cpp cpu/view_axes_test.cpp cpu/surface_current_test.cpp cpu/parts_test.cpp cpu/netwalk_test.cpp cpu/ntff_test.cpp cpu/sweep_test.cpp cpu/geometry_override_test.cpp cpu/vtk_test.cpp cpu/isosurface_test.cpp cpu/probes_test.cpp cpu/dft_known_test.cpp cpu/volume_renderer_test.cpp cpu/run_archive_test.cpp cpu/antenna_test.cpp)
# Test scratch files live in the build tree, not in the system temp folder (which is ephemeral).
target_link_libraries(tests_cpu PRIVATE fdtd_solver board sim viz viz_vk viz_ui appcore fdtd_options Catch2::Catch2WithMain)
# Long numerical scenarios are tagged [slow] and run from the optimised preset (the sanitizer build is ~50x slower).
add_test(NAME cpu COMMAND tests_cpu "~[slow]")
set_tests_properties(cpu PROPERTIES LABELS cpu)
add_test(NAME cpu_slow COMMAND tests_cpu "[slow]")
set_tests_properties(cpu_slow PROPERTIES LABELS cpu_slow)

add_executable(tests_gpu gpu/vkcore_test.cpp gpu/gpu_solver_test.cpp gpu/sweep_gpu_test.cpp gpu/volume_gpu_test.cpp gpu/slice_gpu_test.cpp)
target_link_libraries(tests_gpu PRIVATE fdtd_vk viz_vk vkcore appcore fdtd_options fdtd_shaders Catch2::Catch2WithMain)
add_test(NAME gpu COMMAND tests_gpu)
set_tests_properties(gpu PROPERTIES LABELS gpu)

target_compile_definitions(tests_cpu PRIVATE FDTD_TEST_TMP="${CMAKE_BINARY_DIR}/test-tmp")
target_compile_definitions(tests_gpu PRIVATE FDTD_TEST_TMP="${CMAKE_BINARY_DIR}/test-tmp")
