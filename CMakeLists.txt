cmake_minimum_required(VERSION 3.10)
project(RetroCapture VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Opções de build
option(BUILD_TESTS "Build tests" OFF)
option(BUILD_DOCS "Build documentation" OFF)

# Diretórios
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Incluir diretórios
include_directories(${CMAKE_SOURCE_DIR}/src)

# Encontrar dependências
find_package(PkgConfig REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

# GLFW
find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
    pkg_check_modules(GLFW REQUIRED glfw3)
    set(GLFW_LIBRARIES ${GLFW_LIBRARIES})
    set(GLFW_INCLUDE_DIRS ${GLFW_INCLUDE_DIRS})
endif()

# V4L2 (via libv4l2)
pkg_check_modules(V4L2 REQUIRED libv4l2)

# PNG (via libpng)
pkg_check_modules(PNG REQUIRED libpng)

# Threading
find_package(Threads REQUIRED)

# Fontes
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.h"
)

# Executável principal
add_executable(retrocapture
    ${SOURCES}
)

# Link libraries
if(glfw3_FOUND)
    target_link_libraries(retrocapture
        OpenGL::GL
        glfw
        ${V4L2_LIBRARIES}
        ${PNG_LIBRARIES}
        Threads::Threads
    )
else()
    target_link_libraries(retrocapture
        OpenGL::GL
        ${GLFW_LIBRARIES}
        ${V4L2_LIBRARIES}
        ${PNG_LIBRARIES}
        Threads::Threads
    )
endif()

# Include directories
if(NOT glfw3_FOUND)
    target_include_directories(retrocapture PRIVATE
        ${GLFW_INCLUDE_DIRS}
    )
endif()
target_include_directories(retrocapture PRIVATE
    ${V4L2_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
)

# Compiler flags
target_compile_options(retrocapture PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Instalação
install(TARGETS retrocapture DESTINATION bin)

