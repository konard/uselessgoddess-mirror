# Unit tests: plain C, no Vulkan required.
add_executable(unit_config unit_config.c ${CMAKE_SOURCE_DIR}/src/config.c ${CMAKE_SOURCE_DIR}/src/hash.c)
target_link_libraries(unit_config PRIVATE mirror_warnings)
add_test(NAME unit_config COMMAND unit_config)

add_executable(unit_umap unit_umap.c ${CMAKE_SOURCE_DIR}/src/umap.c)
target_link_libraries(unit_umap PRIVATE mirror_warnings)
add_test(NAME unit_umap COMMAND unit_umap)

# Integration tests: run the real layer through the Vulkan loader on any
# available device (lavapipe in CI).
mirror_compile_shader(shaders/fullscreen.vert fullscreen_vert_spv)
mirror_compile_shader(shaders/red.frag red_frag_spv)
mirror_compile_shader(shaders/blue.frag blue_frag_spv)

add_executable(integration_render
    integration_render.c
    vkutil.c
    ${CMAKE_SOURCE_DIR}/src/hash.c
    ${MIRROR_GENERATED_DIR}/fullscreen_vert_spv.h
    ${MIRROR_GENERATED_DIR}/red_frag_spv.h
    ${MIRROR_GENERATED_DIR}/blue_frag_spv.h)
target_include_directories(integration_render PRIVATE ${MIRROR_GENERATED_DIR})
target_link_libraries(integration_render PRIVATE Vulkan::Vulkan mirror_warnings)
add_dependencies(integration_render mirror_layer)

foreach(scenario enumerate nolayer off depth highlight validation)
    add_test(NAME render_${scenario} COMMAND integration_render ${scenario})
    set_tests_properties(render_${scenario} PROPERTIES
        ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer.d"
        SKIP_RETURN_CODE 77
        TIMEOUT 120)
endforeach()
