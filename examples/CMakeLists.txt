find_package(ZLIB REQUIRED)

mirror_compile_shader(shaders/sky.frag sky_frag_spv)
mirror_compile_shader(shaders/wall.frag wall_frag_spv)
mirror_compile_shader(shaders/floor.frag floor_frag_spv)
mirror_compile_shader(shaders/player.frag player_frag_spv)
mirror_compile_shader(shaders/chest.frag chest_frag_spv)

add_executable(scene
    scene.c
    png.c
    ${CMAKE_SOURCE_DIR}/tests/vkutil.c
    ${CMAKE_SOURCE_DIR}/src/hash.c
    ${MIRROR_GENERATED_DIR}/sky_frag_spv.h
    ${MIRROR_GENERATED_DIR}/wall_frag_spv.h
    ${MIRROR_GENERATED_DIR}/floor_frag_spv.h
    ${MIRROR_GENERATED_DIR}/player_frag_spv.h
    ${MIRROR_GENERATED_DIR}/chest_frag_spv.h
    ${MIRROR_GENERATED_DIR}/fullscreen_vert_spv.h)
target_include_directories(scene PRIVATE ${MIRROR_GENERATED_DIR})
target_link_libraries(scene PRIVATE Vulkan::Vulkan ZLIB::ZLIB mirror_warnings)
add_dependencies(scene mirror_layer)

# Keeps the example exercised in CI and produces screenshot artifacts.
if(MIRROR_BUILD_TESTS)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/screenshots)
    add_test(NAME example_scene COMMAND scene ${CMAKE_BINARY_DIR}/screenshots)
    set_tests_properties(example_scene PROPERTIES
        ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer.d"
        SKIP_RETURN_CODE 77
        TIMEOUT 120)
endif()
