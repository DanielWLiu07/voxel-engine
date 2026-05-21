cmake_minimum_required(VERSION 3.20)
project(voxel_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- GLFW ---------------------------------------------------------------------
set(GLFW_BUILD_DOCS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL       OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4
  GIT_SHALLOW    TRUE
)

# --- GLM (header-only) --------------------------------------------------------
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
  GIT_SHALLOW    TRUE
)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.5
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw glm)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_MakeAvailable(imgui)
endif()

# Wrap ImGui sources into a static lib. Includes the GLFW + OpenGL3 backends.
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

# --- GLAD (vendored generated loader for GL 4.1 core, glad2 layout) ----------
add_library(glad STATIC third_party/glad/src/gl.c)
target_include_directories(glad PUBLIC third_party/glad/include)

# --- stb (header-only, vendored) ----------------------------------------------
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE third_party/stb)

# --- FastNoiseLite (header-only, vendored) -----------------------------------
add_library(fastnoise INTERFACE)
target_include_directories(fastnoise INTERFACE third_party/FastNoiseLite)

# --- App ----------------------------------------------------------------------
add_executable(voxel_engine
  src/main.cpp
  src/core/input.cpp
  src/core/thread_pool.cpp
  src/game/player.cpp
  src/gfx/camera.cpp
  src/gfx/frustum.cpp
  src/gfx/mesh.cpp
  src/gfx/shader.cpp
  src/gfx/shadow_map.cpp
  src/gfx/texture.cpp
  src/ui/debug_hud.cpp
  src/world/chunk_mesh.cpp
  src/world/terrain_gen.cpp
  src/world/world.cpp
)

target_include_directories(voxel_engine PRIVATE src)

target_link_libraries(voxel_engine PRIVATE
  glfw
  glm::glm
  glad
  stb
  fastnoise
  imgui
)

if(APPLE)
  find_library(COCOA_LIB Cocoa REQUIRED)
  find_library(IOKIT_LIB IOKit REQUIRED)
  find_library(COREVIDEO_LIB CoreVideo REQUIRED)
  find_library(OPENGL_LIB OpenGL REQUIRED)
  target_link_libraries(voxel_engine PRIVATE
    ${COCOA_LIB} ${IOKIT_LIB} ${COREVIDEO_LIB} ${OPENGL_LIB}
  )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(voxel_engine PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
  )
endif()
