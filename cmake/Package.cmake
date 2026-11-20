# Install layout and the release archive (cpack -G TGZ):
#   bin/emfdtd                   the solver and viewer
#   share/emfdtd/data, shaders   read at run time next to bin/ (src/core/resources.hpp)
#   share/emfdtd/plugin          the KiCAD IPC plugin (install it with plugin/tools/install.sh)
#   share/doc/emfdtd             CREDITS.md and licences/ (the third-party notices that must ship with a binary)
include(GNUInstallDirs)
install(TARGETS emfdtd RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/ DESTINATION share/emfdtd/data)
install(DIRECTORY ${CMAKE_BINARY_DIR}/shaders/ DESTINATION share/emfdtd/shaders FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/plugin/ DESTINATION share/emfdtd/plugin
        PATTERN ".venv" EXCLUDE PATTERN "__pycache__" EXCLUDE PATTERN ".pytest_cache" EXCLUDE
        PATTERN ".ruff_cache" EXCLUDE PATTERN "tests" EXCLUDE PATTERN "probe" EXCLUDE
        PATTERN "*.egg-info" EXCLUDE)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/CHANGELOG.md
              ${CMAKE_SOURCE_DIR}/CREDITS.md
        DESTINATION share/doc/emfdtd)
# Licence texts of everything linked into the binary (MIT and zlib require the notice to travel with it).
install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION share/doc/emfdtd/licenses RENAME imgui-LICENSE.txt)
install(FILES ${glfw_SOURCE_DIR}/LICENSE.md DESTINATION share/doc/emfdtd/licenses RENAME glfw-LICENSE.md)
install(FILES ${nlohmann_json_SOURCE_DIR}/LICENSE.MIT DESTINATION share/doc/emfdtd/licenses
        RENAME nlohmann_json-LICENSE.MIT)
if(FDTD_SCREEN_READER)
  file(GLOB _accesskit_licences ${accesskit_SOURCE_DIR}/LICENSE*)
  install(FILES ${_accesskit_licences} DESTINATION share/doc/emfdtd/licenses/accesskit)
endif()

string(TOLOWER "${CMAKE_SYSTEM_NAME}" _fdtd_os)
if(_fdtd_os STREQUAL "darwin")
  set(_fdtd_os "macos")
endif()
set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_NAME emfdtd)
set(CPACK_PACKAGE_VERSION ${FDTD_VERSION})
set(CPACK_PACKAGE_FILE_NAME "emfdtd-${FDTD_VERSION}-${_fdtd_os}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)
