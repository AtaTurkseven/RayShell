cmake_minimum_required(VERSION 3.16)
project(RayShell C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} main.c)

# Use the raylib source tree already present in UIDeneme
set(RAYLIB_SRC "${CMAKE_SOURCE_DIR}/../UIDeneme/raylib-src")

if(EXISTS "${RAYLIB_SRC}/CMakeLists.txt")
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${RAYLIB_SRC}" raylib-build)
else()
    # Fallback: look for a locally installed package
    if(EXISTS "${CMAKE_SOURCE_DIR}/../UIDeneme/raylib-install")
        list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../UIDeneme/raylib-install")
    endif()
    find_package(raylib CONFIG REQUIRED)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE raylib)

# On Windows, prefer the default Windows subsystem so the console
# doesn't flash; raylib opens its own window.
if(WIN32)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()
