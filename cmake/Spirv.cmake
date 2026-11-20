# add_spirv_shaders(<target> <shader.comp>...): compile GLSL to SPIR-V into ${CMAKE_BINARY_DIR}/shaders.
function(add_spirv_shaders target)
  set(outs)
  foreach(src IN LISTS ARGN)
    get_filename_component(name ${src} NAME)
    set(out ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
    add_custom_command(
      OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
      COMMAND Vulkan::glslc --target-env=vulkan1.2 -O -I ${CMAKE_SOURCE_DIR}/src/shared ${CMAKE_SOURCE_DIR}/${src} -o ${out}
      DEPENDS ${CMAKE_SOURCE_DIR}/${src}
      COMMENT "glslc ${src}")
    list(APPEND outs ${out})
  endforeach()
  add_custom_target(${target} DEPENDS ${outs})
endfunction()
