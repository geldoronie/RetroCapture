cmake_minimum_required(VERSION 3.10)
project(RetroCapture VERSION 0.3.0 LANGUAGES CXX)

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

# Detectar plataforma
if(WIN32)
    set(PLATFORM_WINDOWS ON)
    message(STATUS "Plataforma: Windows")
elseif(UNIX AND NOT APPLE)
    set(PLATFORM_LINUX ON)
    message(STATUS "Plataforma: Linux")
elseif(APPLE)
    set(PLATFORM_MACOS ON)
    message(STATUS "Plataforma: macOS")
else()
    message(FATAL_ERROR "Plataforma não suportada")
endif()

# Encontrar dependências
# OpenGL
find_package(OpenGL REQUIRED)

# GLFW
find_package(glfw3 QUIET)

if(NOT glfw3_FOUND)
    if(PLATFORM_LINUX)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW REQUIRED glfw3)
        set(GLFW_LIBRARIES ${GLFW_LIBRARIES})
        set(GLFW_INCLUDE_DIRS ${GLFW_INCLUDE_DIRS})
    else()
        message(FATAL_ERROR "GLFW não encontrado. Instale glfw3.")
    endif()
endif()

# PNG (via libpng)
if(PLATFORM_LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PNG REQUIRED libpng)
else()
    find_package(PNG REQUIRED)
endif()

# FFmpeg for streaming
if(PLATFORM_LINUX)
    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(AVFORMAT REQUIRED libavformat)
    pkg_check_modules(AVUTIL REQUIRED libavutil)
    pkg_check_modules(SWSCALE REQUIRED libswscale)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
else()
    # Windows: usar find_package ou vcpkg
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        pkg_check_modules(AVCODEC REQUIRED libavcodec)
        pkg_check_modules(AVFORMAT REQUIRED libavformat)
        pkg_check_modules(AVUTIL REQUIRED libavutil)
        pkg_check_modules(SWSCALE REQUIRED libswscale)
        pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    else()
        # Tentar encontrar via find_package (vcpkg)
        find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
        set(AVCODEC_LIBRARIES avcodec)
        set(AVFORMAT_LIBRARIES avformat)
        set(AVUTIL_LIBRARIES avutil)
        set(SWSCALE_LIBRARIES swscale)
        set(SWRESAMPLE_LIBRARIES swresample)
        set(AVCODEC_INCLUDE_DIRS ${FFmpeg_INCLUDE_DIRS})
        set(AVFORMAT_INCLUDE_DIRS ${FFmpeg_INCLUDE_DIRS})
        set(AVUTIL_INCLUDE_DIRS ${FFmpeg_INCLUDE_DIRS})
        set(SWSCALE_INCLUDE_DIRS ${FFmpeg_INCLUDE_DIRS})
        set(SWRESAMPLE_INCLUDE_DIRS ${FFmpeg_INCLUDE_DIRS})
    endif()
endif()

# HTTP server implementado internamente (HTTPServer.cpp)

# V4L2 (Linux only)
if(PLATFORM_LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(V4L2 REQUIRED libv4l2)
endif()

# PulseAudio (Linux only)
if(PLATFORM_LINUX)
    pkg_check_modules(PULSE REQUIRED libpulse)
endif()

# OpenSSL for HTTPS support - sempre habilitado se disponível
# Tentar encontrar OpenSSL primeiro
find_package(OpenSSL QUIET)

if(OpenSSL_FOUND)
    set(ENABLE_HTTPS ON)
    set(HTTPS_ENABLED TRUE)
    message(STATUS "OpenSSL found via find_package")
    message(STATUS "  OpenSSL version: ${OPENSSL_VERSION}")
    message(STATUS "  HTTPS support: ENABLED")
else()
    # Tentar via pkg-config como fallback
    pkg_check_modules(OPENSSL QUIET openssl)

    if(OPENSSL_FOUND)
        set(ENABLE_HTTPS ON)
        set(HTTPS_ENABLED TRUE)
        message(STATUS "OpenSSL found via pkg-config")
        message(STATUS "  OpenSSL version: ${OPENSSL_VERSION}")
        message(STATUS "  HTTPS support: ENABLED")
    else()
        set(ENABLE_HTTPS OFF)
        set(HTTPS_ENABLED FALSE)
        message(WARNING "OpenSSL not found, HTTPS support will be DISABLED")
        message(WARNING "Install OpenSSL development packages to enable HTTPS:")
        message(WARNING "  Arch/Manjaro: sudo pacman -S openssl")
        message(WARNING "  Ubuntu/Debian: sudo apt-get install libssl-dev")
        message(WARNING "  Fedora: sudo dnf install openssl-devel")
    endif()
endif()

# Resumo final
if(HTTPS_ENABLED)
    message(STATUS "=== HTTPS Support: ENABLED ===")
else()
    message(STATUS "=== HTTPS Support: DISABLED ===")
endif()

# ImGui (via FetchContent)
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# nlohmann/json (header-only JSON library)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# Criar target para ImGui
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui PUBLIC
    glfw
    OpenGL::GL
)

if(PLATFORM_LINUX)
    target_link_libraries(imgui PUBLIC X11)
endif()

# Fontes
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.h"
)

# Excluir implementações de outras plataformas
if(PLATFORM_LINUX)
    # No Linux, excluir implementações Windows (quando existirem)
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureMF\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioCaptureWASAPI\\.(cpp|h)$")
elseif(PLATFORM_WINDOWS)
    # No Windows, excluir implementações Linux
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureV4L2\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioCapturePulse\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*V4L2.*\\.(cpp|h)$")
endif()

# Executável principal
add_executable(retrocapture
    ${SOURCES}
)

# Definir macros de plataforma
if(PLATFORM_LINUX)
    target_compile_definitions(retrocapture PRIVATE PLATFORM_LINUX)
elseif(PLATFORM_WINDOWS)
    target_compile_definitions(retrocapture PRIVATE PLATFORM_WINDOWS)
elseif(PLATFORM_MACOS)
    target_compile_definitions(retrocapture PRIVATE PLATFORM_MACOS)
endif()

# Threading
find_package(Threads REQUIRED)

# Link libraries
set(LINK_LIBS
    OpenGL::GL
    Threads::Threads
    imgui
)

if(glfw3_FOUND)
    list(APPEND LINK_LIBS glfw)
else()
    list(APPEND LINK_LIBS ${GLFW_LIBRARIES})
endif()

list(APPEND LINK_LIBS
    ${PNG_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
)

# Bibliotecas específicas de plataforma
if(PLATFORM_LINUX)
    list(APPEND LINK_LIBS ${V4L2_LIBRARIES} ${PULSE_LIBRARIES})
elseif(PLATFORM_WINDOWS)
    # Windows Media Foundation libraries
    list(APPEND LINK_LIBS
        mf
        mfplat
        mfuuid
        strmiids
        ole32
        oleaut32
    )
    # Windows Audio Session API (WASAPI) libraries
    list(APPEND LINK_LIBS
        avrt
        propsys
    )
endif()

target_link_libraries(retrocapture PRIVATE ${LINK_LIBS})

# Include directories
if(NOT glfw3_FOUND)
    target_include_directories(retrocapture PRIVATE
        ${GLFW_INCLUDE_DIRS}
    )
endif()

set(INCLUDE_DIRS
    ${PNG_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
)

# Include directories específicos de plataforma
if(PLATFORM_LINUX)
    list(APPEND INCLUDE_DIRS ${V4L2_INCLUDE_DIRS} ${PULSE_INCLUDE_DIRS})
endif()

target_include_directories(retrocapture PRIVATE ${INCLUDE_DIRS})

# Adicionar include do nlohmann/json (header-only, não precisa linkar)
# O nlohmann/json é header-only, então precisamos apenas adicionar o diretório de include
# O FetchContent disponibiliza json_SOURCE_DIR
if(DEFINED json_SOURCE_DIR)
    target_include_directories(retrocapture PRIVATE ${json_SOURCE_DIR}/include)
endif()

# Configuração de build types
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG")

# Compiler flags base (comuns a todos os builds)
target_compile_options(retrocapture PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Flags específicas por build type
target_compile_options(retrocapture PRIVATE
    $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
    $<$<CONFIG:Release>:-O3 -DNDEBUG -march=native>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
    $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
)

# Linker flags específicas por build type
target_link_options(retrocapture PRIVATE
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Release>:>
    $<$<CONFIG:RelWithDebInfo>:-g>
    $<$<CONFIG:MinSizeRel>:>
)

# Copiar pasta de shaders para o diretório de build
# Isso garante que o binário encontre os shaders corretamente ao executar
add_custom_command(TARGET retrocapture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/shaders"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders"
    COMMENT "Copying shaders directory to build output"
)

# Copiar pasta web para o diretório de build
# Isso garante que o servidor HTTP encontre os arquivos web corretamente
add_custom_command(TARGET retrocapture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/src/web"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/web"
    COMMENT "Copying web directory to build output"
)

# Copiar pasta ssl para o diretório de build
# Isso garante que o servidor HTTPS encontre os certificados SSL corretamente
add_custom_command(TARGET retrocapture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ssl"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/ssl"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ssl"
    COMMENT "Copying ssl directory to build output"
)

# Copiar pasta assets para o diretório de build
# Isso garante que a aplicação encontre as imagens (logo, etc.) corretamente
add_custom_command(TARGET retrocapture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets"
    COMMENT "Copying assets directory to build output"
)

# Copiar certificados placeholder da raiz para ssl/ se existirem
# Isso permite ter certificados de exemplo ou placeholder na raiz do projeto
# que serão copiados para a pasta ssl/ no diretório de build
if(EXISTS "${CMAKE_SOURCE_DIR}/server.crt")
    add_custom_command(TARGET retrocapture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/server.crt"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ssl/server.crt"
        COMMENT "Copying server.crt placeholder from root to build output"
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/server.key")
    add_custom_command(TARGET retrocapture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/server.key"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ssl/server.key"
        COMMENT "Copying server.key placeholder from root to build output"
    )
endif()

# Link OpenSSL if HTTPS is enabled
if(HTTPS_ENABLED)
    if(OpenSSL_FOUND)
        target_link_libraries(retrocapture PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(retrocapture PRIVATE ENABLE_HTTPS)
    elseif(OPENSSL_FOUND)
        target_link_libraries(retrocapture PRIVATE ${OPENSSL_LIBRARIES})
        target_include_directories(retrocapture PRIVATE ${OPENSSL_INCLUDE_DIRS})
        target_compile_definitions(retrocapture PRIVATE ENABLE_HTTPS)
    endif()
endif()

# Instalação
install(TARGETS retrocapture DESTINATION bin)
