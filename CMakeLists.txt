cmake_minimum_required(VERSION 3.22)

project(minecraft_legacy
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(FetchContent)

option(MINECRAFT_LEGACY_FETCH_SDL3 "Fetch SDL3 with FetchContent" ON)
set(MINECRAFT_LEGACY_ANDROID_ASSET_DIR "" CACHE PATH "Generated Android asset output directory")

set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(ANDROID)
    find_library(Vulkan_LIBRARY vulkan REQUIRED)
else()
    find_package(Vulkan REQUIRED)
endif()

if(MINECRAFT_LEGACY_FETCH_SDL3)
    if(ANDROID)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.10
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
else()
    find_package(SDL3 CONFIG REQUIRED)
endif()

set(MINECRAFT_LEGACY_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/textures/texture_pack/classic/shaders")
if(ANDROID AND MINECRAFT_LEGACY_ANDROID_ASSET_DIR)
    set(MINECRAFT_LEGACY_COMPILED_SHADER_DIR "${MINECRAFT_LEGACY_ANDROID_ASSET_DIR}/shaders")
else()
    set(MINECRAFT_LEGACY_COMPILED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
endif()
file(MAKE_DIRECTORY "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}")

if(DEFINED ENV{VULKAN_SDK})
    set(GLSLC_EXECUTABLE "$ENV{VULKAN_SDK}/Bin/glslc.exe")
endif()

if(NOT EXISTS "${GLSLC_EXECUTABLE}")
    find_program(GLSLC_EXECUTABLE glslc REQUIRED)
endif()

set(SHADER_COMPILE_DEFINES)

set(SHADER_OUTPUTS
    "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/voxel.vert.spv"
    "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/voxel.frag.spv"
    "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/water.frag.spv"
    "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/hud.vert.spv"
    "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/hud_textured.vert.spv"
    "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/hud_textured.frag.spv"
    "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/color.frag.spv"
    "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/wireframe_debug.frag.spv"
)

add_custom_command(
    OUTPUT ${SHADER_OUTPUTS}
    COMMAND "${GLSLC_EXECUTABLE}" ${SHADER_COMPILE_DEFINES} "${MINECRAFT_LEGACY_SHADER_DIR}/voxel.vert" -o "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/voxel.vert.spv"
    COMMAND "${GLSLC_EXECUTABLE}" ${SHADER_COMPILE_DEFINES} "${MINECRAFT_LEGACY_SHADER_DIR}/voxel.frag" -o "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/voxel.frag.spv"
    COMMAND "${GLSLC_EXECUTABLE}" ${SHADER_COMPILE_DEFINES} "${MINECRAFT_LEGACY_SHADER_DIR}/water.frag" -o "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/water.frag.spv"
    COMMAND "${GLSLC_EXECUTABLE}" ${SHADER_COMPILE_DEFINES} "${MINECRAFT_LEGACY_SHADER_DIR}/hud.vert" -o "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/hud.vert.spv"
    COMMAND "${GLSLC_EXECUTABLE}" ${SHADER_COMPILE_DEFINES} "${MINECRAFT_LEGACY_SHADER_DIR}/hud_textured.vert" -o "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/hud_textured.vert.spv"
    COMMAND "${GLSLC_EXECUTABLE}" ${SHADER_COMPILE_DEFINES} "${MINECRAFT_LEGACY_SHADER_DIR}/hud_textured.frag" -o "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/hud_textured.frag.spv"
    COMMAND "${GLSLC_EXECUTABLE}" ${SHADER_COMPILE_DEFINES} "${MINECRAFT_LEGACY_SHADER_DIR}/color.frag" -o "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/color.frag.spv"
    COMMAND "${GLSLC_EXECUTABLE}" ${SHADER_COMPILE_DEFINES} "${MINECRAFT_LEGACY_SHADER_DIR}/wireframe_debug.frag" -o "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}/wireframe_debug.frag.spv"
    DEPENDS
        "${MINECRAFT_LEGACY_SHADER_DIR}/voxel.vert"
        "${MINECRAFT_LEGACY_SHADER_DIR}/voxel.frag"
        "${MINECRAFT_LEGACY_SHADER_DIR}/water.frag"
        "${MINECRAFT_LEGACY_SHADER_DIR}/hud.vert"
        "${MINECRAFT_LEGACY_SHADER_DIR}/hud_textured.vert"
        "${MINECRAFT_LEGACY_SHADER_DIR}/hud_textured.frag"
        "${MINECRAFT_LEGACY_SHADER_DIR}/color.frag"
        "${MINECRAFT_LEGACY_SHADER_DIR}/wireframe_debug.frag"
    COMMENT "Compiling Vulkan shaders"
    VERBATIM
)

add_custom_target(compile_shaders DEPENDS ${SHADER_OUTPUTS})

set(MINECRAFT_LEGACY_SOURCES
    src/main.cpp
    src/app/application.cpp
    src/common/asset_pack.cpp
    src/platform/platform_app.cpp
    src/platform/stb_vorbis_impl.cpp
    src/render/renderer.cpp
    src/game/block.cpp
    src/game/light.cpp
    src/game/world_generator.cpp
    src/game/world_save.cpp
    src/game/world_streamer.cpp
    src/game/debug_camera.cpp
    src/game/player_controller.cpp
)

if(ANDROID)
    add_library(main SHARED ${MINECRAFT_LEGACY_SOURCES})
    set(MINECRAFT_LEGACY_TARGET main)
else()
    add_executable(minecraft_legacy ${MINECRAFT_LEGACY_SOURCES})
    set(MINECRAFT_LEGACY_TARGET minecraft_legacy)
endif()

if(MSVC)
    set_source_files_properties(src/platform/stb_vorbis_impl.cpp PROPERTIES
        COMPILE_OPTIONS "/wd4244;/wd4245;/wd4456;/wd4457;/wd4701"
    )
endif()

add_dependencies(${MINECRAFT_LEGACY_TARGET} compile_shaders)

target_include_directories(${MINECRAFT_LEGACY_TARGET}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/external"
)

target_compile_definitions(${MINECRAFT_LEGACY_TARGET}
    PRIVATE
        SDL_MAIN_USE_CALLBACKS=0
        NOMINMAX
)

if(ANDROID)
    target_compile_options(${MINECRAFT_LEGACY_TARGET}
        PRIVATE
            -g0
    )
    target_link_libraries(${MINECRAFT_LEGACY_TARGET}
        PRIVATE
            SDL3::SDL3
            ${Vulkan_LIBRARY}
            android
            log
    )
else()
    target_link_libraries(${MINECRAFT_LEGACY_TARGET}
        PRIVATE
            SDL3::SDL3
            Vulkan::Vulkan
    )
endif()

if(NOT ANDROID)
    add_custom_command(TARGET minecraft_legacy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MINECRAFT_LEGACY_COMPILED_SHADER_DIR}"
            "$<TARGET_FILE_DIR:minecraft_legacy>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:minecraft_legacy>/assets"
    )
endif()
