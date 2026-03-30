cmake_minimum_required(VERSION 3.23)
project(Imgui)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
file(GLOB SRC *.h *.cpp backends/imgui_impl_bgfx.h backends/imgui_impl_bgfx.cpp backends/imgui_impl_sdl.h backends/imgui_impl_sdl.cpp misc/cpp/imgui_stdlib.cpp)

add_library(Imgui ${SRC})

include_directories(./)
include_directories(../bgfx/include)
include_directories(../bx/include)

target_compile_definitions(Imgui PUBLIC BX_CONFIG_DEBUG=0)
set_target_properties(Imgui PROPERTIES CXX_STANDARD 17)

if(MSVC)
include_directories(../bx/include/compat/msvc)
target_compile_options(Imgui PUBLIC "/Zc:__cplusplus")
endif()
