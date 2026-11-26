cmake_minimum_required(VERSION 3.24)
# The version (semantic versioning: MAJOR.MINOR.PATCH[-prerelease]) lives only in the VERSION file;
# scripts/version.sh bumps it and keeps plugin/pyproject.toml in step.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION FDTD_VERSION LIMIT_COUNT 1)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" FDTD_VERSION_CORE "${FDTD_VERSION}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
project(kicad_em_fdtd VERSION ${FDTD_VERSION_CORE} LANGUAGES CXX)
if(APPLE)
  enable_language(OBJCXX) # system accessibility queries and the screen-reader adapter
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(FDTD_SANITIZE "ASan + UBSan" OFF)
option(FDTD_VALIDATION "Run GPU tests with the Vulkan validation layer" ON)
option(FDTD_WARNINGS_AS_ERRORS "-Werror" OFF)

# One place for compile flags: every target links fdtd_options.
add_library(fdtd_options INTERFACE)
if(MSVC)
  # cl takes /W3 (-Wall is extremely noisy on it) and needs these for portable source: UTF-8 sources,
  # strict conformance, M_PI from <cmath>, no min/max macros from <windows.h>.
  target_compile_options(fdtd_options INTERFACE /W3 /utf-8 /permissive-)
  target_compile_definitions(fdtd_options INTERFACE _USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
  if(FDTD_WARNINGS_AS_ERRORS)
    target_compile_options(fdtd_options INTERFACE /WX)
  endif()
  if(FDTD_SANITIZE)
    target_compile_options(fdtd_options INTERFACE /fsanitize=address)
  endif()
else()
  target_compile_options(fdtd_options INTERFACE -Wall -Wextra -Wshadow
    # Vulkan create-info structs are initialised as {sType}; the other fields are intentionally zero.
    -Wno-missing-field-initializers)
  if(FDTD_WARNINGS_AS_ERRORS)
    target_compile_options(fdtd_options INTERFACE -Werror)
  endif()
  if(FDTD_SANITIZE)
    target_compile_options(fdtd_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(fdtd_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

find_package(Vulkan REQUIRED COMPONENTS glslc)
include(cmake/Spirv.cmake)
add_spirv_shaders(shaders shaders/hello.comp shaders/copy.comp shaders/fdtd_step.comp shaders/phasor_accum.comp shaders/slice_render.comp shaders/volume_field.comp shaders/volume_render.comp shaders/line2d_rb.comp)

# Anything that loads a SPIR-V file links this to learn where they are.
add_library(fdtd_shaders INTERFACE)
add_dependencies(fdtd_shaders shaders)
target_compile_definitions(fdtd_shaders INTERFACE FDTD_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")
if(FDTD_VALIDATION)
  target_compile_definitions(fdtd_shaders INTERFACE FDTD_VALIDATION=1)
endif()

# Header-only, Vulkan-free core pieces (units, later the CPU solver) live under src/core.
add_library(fdtd_core INTERFACE)
target_include_directories(fdtd_core INTERFACE ${CMAKE_SOURCE_DIR}/src)

include(FetchContent)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.12.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
# Screen-reader support: AccessKit's C bindings (MIT or Apache-2.0), used as the release's prebuilt static
# libraries (no Rust toolchain needed). One API for VoiceOver, Windows UI Automation and AT-SPI (Orca); only
# the macOS adapter is wired up so far (src/viz/screen_reader.cpp).
if(APPLE)
  option(FDTD_SCREEN_READER "Expose the window to screen readers (AccessKit)" ON)
else()
  option(FDTD_SCREEN_READER "Expose the window to screen readers (AccessKit)" OFF)
endif()
if(FDTD_SCREEN_READER)
  FetchContent_Declare(accesskit
    URL https://github.com/AccessKit/accesskit-c/releases/download/0.23.2/accesskit-c-0.23.2.zip
    URL_HASH SHA256=c7fa84f8f34fca909f662375737a54f98b17cf9eae51fffd94ab3d9f662e3681
    SOURCE_SUBDIR prebuilt-only) # the release's own CMakeLists builds from source with cargo; skip it
  FetchContent_MakeAvailable(accesskit)
  find_package(accesskit REQUIRED CONFIG PATHS ${accesskit_SOURCE_DIR} NO_DEFAULT_PATH)
endif()

add_library(imgui_core STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_core PUBLIC glfw)

# Dear ImGui's Vulkan backend (MIT, same source tree): the window draws the UI on the GPU with it.
add_library(imgui_vulkan STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui_vulkan PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_vulkan PUBLIC Vulkan::Vulkan)

configure_file(src/version.hpp.in ${CMAKE_BINARY_DIR}/generated/version.hpp @ONLY)
add_library(fdtd_version INTERFACE)
target_include_directories(fdtd_version INTERFACE ${CMAKE_BINARY_DIR}/generated)

add_subdirectory(src/fdtd)
add_subdirectory(src/board)
add_subdirectory(src/sim)
add_subdirectory(src/viz)
add_subdirectory(src/app)
add_subdirectory(src/vkcore)
add_subdirectory(src/fdtd_vk)
add_subdirectory(src/viz_vk)
add_subdirectory(apps/bench)
add_subdirectory(apps/emfdtd)
enable_testing()
add_subdirectory(tests)

include(cmake/Package.cmake)
