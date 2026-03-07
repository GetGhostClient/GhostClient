cmake_minimum_required(VERSION 3.20)
project(ghostclient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "GhostClient must be built as x64. Roblox is a 64-bit process and pointer chain reads require 8-byte pointers. Re-run CMake with -A x64.")
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/deps/imgui)

add_library(imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC d3d11 d3dcompiler dxgi)

add_executable(ghostclient WIN32
    src/main.cpp
    src/gui.cpp
    src/process.cpp
    src/fflags.cpp
    src/parser.cpp
)
target_include_directories(ghostclient PRIVATE src)
target_link_libraries(ghostclient PRIVATE imgui_lib dwmapi winhttp)

if(MSVC)
    set_target_properties(imgui_lib ghostclient PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    set_target_properties(ghostclient PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS"
    )
    target_compile_options(ghostclient PRIVATE /W3 /MP)
endif()
