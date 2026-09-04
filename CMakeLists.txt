cmake_minimum_required(VERSION 3.20)
project(crivate VERSION 1.0 LANGUAGES CXX)

# Generate VS solution: gen.bat
# Build: cmake --build build --config Release

if(NOT WIN32)
    message(FATAL_ERROR "crivate is a Windows application.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "crivate requires a 64-bit (x64) build. Configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(crivate
    src/main.cpp
    src/folder.cpp
    src/crypto.cpp
    src/cri.cpp
    src/image_load.cpp
    src/viewer.cpp
)

target_compile_definitions(crivate PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(crivate PRIVATE
    d3d11
    dxgi
    d3dcompiler
    windowscodecs
    bcrypt
    user32
    gdi32
    ole32
    uuid
)

set_target_properties(crivate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(crivate PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET crivate PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
