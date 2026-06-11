cmake_minimum_required(VERSION 3.25)

project(mirror
    VERSION 0.1.0
    DESCRIPTION "Vulkan layer that renders depth instead of materials, with configurable highlights"
    HOMEPAGE_URL "https://github.com/uselessgoddess/mirror"
    LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MIRROR_BUILD_TESTS "Build mirror tests" ON)
option(MIRROR_BUILD_EXAMPLES "Build mirror examples" ON)
option(MIRROR_WERROR "Treat compiler warnings as errors" OFF)
option(MIRROR_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(Vulkan REQUIRED COMPONENTS glslangValidator)

add_library(mirror_warnings INTERFACE)
target_compile_options(mirror_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wvla
    $<$<BOOL:${MIRROR_WERROR}>:-Werror>)

if(MIRROR_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Compiles a GLSL shader into a C header containing a uint32_t SPIR-V array.
set(MIRROR_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${MIRROR_GENERATED_DIR})
function(mirror_compile_shader source variable_name)
    add_custom_command(
        OUTPUT ${MIRROR_GENERATED_DIR}/${variable_name}.h
        COMMAND Vulkan::glslangValidator -V --vn ${variable_name}
                -o ${MIRROR_GENERATED_DIR}/${variable_name}.h
                ${CMAKE_CURRENT_SOURCE_DIR}/${source}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
        COMMENT "Compiling shader ${source}"
        VERBATIM)
endfunction()

# ------------------------------------------------------------------ layer --

mirror_compile_shader(shaders/mirror.frag mirror_frag_spv)

add_library(mirror_layer MODULE
    src/layer.c
    src/config.c
    src/hash.c
    src/umap.c
    ${MIRROR_GENERATED_DIR}/mirror_frag_spv.h)
set_target_properties(mirror_layer PROPERTIES
    OUTPUT_NAME VkLayer_MIRROR_mirror
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/layer.d
    C_VISIBILITY_PRESET hidden)
target_include_directories(mirror_layer PRIVATE ${MIRROR_GENERATED_DIR})
target_link_libraries(mirror_layer PRIVATE Vulkan::Headers mirror_warnings)

# Build-tree manifest, usable directly via VK_ADD_LAYER_PATH.
set(MIRROR_LAYER_LIBRARY_PATH "./libVkLayer_MIRROR_mirror.so")
configure_file(manifest/VkLayer_MIRROR_mirror.json.in
    ${CMAKE_BINARY_DIR}/layer.d/VkLayer_MIRROR_mirror.json @ONLY)

# Installed manifest: the library is resolved through the dynamic linker.
set(MIRROR_LAYER_LIBRARY_PATH "libVkLayer_MIRROR_mirror.so")
configure_file(manifest/VkLayer_MIRROR_mirror.json.in
    ${CMAKE_BINARY_DIR}/install/VkLayer_MIRROR_mirror.json @ONLY)

include(GNUInstallDirs)
install(TARGETS mirror_layer LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_BINARY_DIR}/install/VkLayer_MIRROR_mirror.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/explicit_layer.d)

# ------------------------------------------------------------------- tests --

if(MIRROR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MIRROR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
