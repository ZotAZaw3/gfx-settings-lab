cmake_minimum_required(VERSION 3.20)
project(gfxlab LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(GFX_D3D11 "Build the Direct3D 11 backend (Windows only)" ${WIN32})

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---- GLFW ----
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
# ---- glm ----
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1 GIT_SHALLOW TRUE)
# ---- Dear ImGui (no CMake of its own) ----
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw glm imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
if(GFX_D3D11)
  target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
endif()
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

# ---- glad (generated GL 4.5 core loader, vendored) ----
add_library(glad STATIC third_party/glad/src/gl.c)
target_include_directories(glad PUBLIC third_party/glad/include)

# ---- header-only third party ----
add_library(thirdparty_headers INTERFACE)
target_include_directories(thirdparty_headers INTERFACE third_party)

# ---- app ----
file(GLOB_RECURSE GFX_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/rhi/*.cpp src/main.cpp)
file(GLOB_RECURSE GFX_MODULE_SOURCES CONFIGURE_DEPENDS src/modules/*.cpp)
file(GLOB_RECURSE GFX_D3D11_SOURCES CONFIGURE_DEPENDS src/rhi/d3d11/*.cpp)
list(REMOVE_ITEM GFX_CORE_SOURCES ${GFX_D3D11_SOURCES})

add_executable(gfxlab ${GFX_CORE_SOURCES} ${GFX_MODULE_SOURCES})
target_include_directories(gfxlab PRIVATE src)
target_compile_definitions(gfxlab PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_ENABLE_EXPERIMENTAL
                           GFX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(gfxlab PRIVATE glfw glad imgui glm::glm thirdparty_headers)
find_package(OpenGL REQUIRED)
target_link_libraries(gfxlab PRIVATE OpenGL::GL)

if(GFX_D3D11)
  target_sources(gfxlab PRIVATE ${GFX_D3D11_SOURCES})
  target_compile_definitions(gfxlab PRIVATE GFX_HAS_D3D11=1)
  # glslang + SPIRV-Cross translate the GLSL sources to HLSL at runtime (hot reload works on D3D11 too)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 15.0.0 GIT_SHALLOW TRUE)
  FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git GIT_TAG vulkan-sdk-1.3.296.0 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glslang spirv_cross)
  target_link_libraries(gfxlab PRIVATE glslang glslang-default-resource-limits SPIRV spirv-cross-hlsl d3d11 dxgi d3dcompiler)
endif()

if(MSVC)
  target_compile_options(gfxlab PRIVATE /W3 /permissive- /utf-8)
else()
  target_compile_options(gfxlab PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# shaders/ and docs/ are read from the source tree (hot reload edits the real files)
