cmake_minimum_required(VERSION 3.16)
project(slop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -- FreeType (prebuilt) --
add_library(freetype STATIC IMPORTED GLOBAL)
set_target_properties(freetype PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/third_party/freetype/lib/x64/freetype.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/third_party/freetype/include"
)

add_library(lz4 STATIC
    third_party/lz4/lz4.c
    third_party/lz4/lz4hc.c
)

target_include_directories(lz4 PUBLIC third_party/lz4)

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_freetype.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
)

target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)

target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE)
target_link_libraries(imgui PUBLIC freetype)

add_executable(slop WIN32
    src/example_app.cpp
    src/main.cpp
    src/host.cpp
    src/theme.cpp
    src/gotofile.cpp
    src/gototext.cpp
    src/imgui_util.cpp
    src/allocators.cpp
    src/utf.cpp
    src/usn_journal.cpp
    src/os.cpp
    src/os_window.cpp
    src/app_util.cpp
    visualizers/slop.natvis
)

target_include_directories(slop PRIVATE
    third_party/xxhash
)

target_link_libraries(slop PRIVATE
    imgui
    lz4
)

option(ENABLE_CONSOLE "Allocate a console window on startup" OFF)
if(ENABLE_CONSOLE)
    target_compile_definitions(slop PRIVATE ENABLE_CONSOLE)
endif()
