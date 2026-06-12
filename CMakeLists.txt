cmake_minimum_required(VERSION 3.10)
project(RetroCapture VERSION 0.8.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Opções de build
option(BUILD_TESTS "Build tests" OFF)
option(BUILD_DOCS "Build documentation" OFF)
option(BUILD_WITH_SDL2 "Use SDL2 instead of GLFW (for DirectFB/framebuffer support)" OFF)
option(BUILD_COMPATIBLE_X86_64 "Build with compatible x86-64 flags (no AVX2, works on older CPUs)" OFF)
option(BUILD_COMPATIBLE_ARM64 "Build with portable ARMv8-A flags (no SVE/crypto, works on Pi 3/4/5)" OFF)
# #85 Phase 2 — DirectShow source filter DLL gate. OFF by default
# because it pulls strmbase (DirectShow BaseClasses). We carry a
# MinGW-patched snapshot under third_party/baseclasses/ so the
# Windows cross-build (MXE) can produce the DLL when this flag is
# ON; it stays OFF by default to keep configure-time cheap and
# avoid surprising native Linux builds.
option(RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER
    "Build the Windows DirectShow virtual-camera source filter DLL (#85 Phase 2)"
    OFF)

# #85 macOS path — CoreMediaIO DAL plug-in. Default ON on macOS
# because the host-side sink (VirtualCameraOutputMac) is useless
# without the plug-in that exposes the shared memory to consumers.
# OFF elsewhere so the configure step ignores src/dal_plugin/.
if(APPLE)
    set(_DAL_DEFAULT ON)
else()
    set(_DAL_DEFAULT OFF)
endif()
option(RETROCAPTURE_BUILD_VIRTCAM_DAL_PLUGIN
    "Build the macOS CoreMediaIO DAL virtual-camera plug-in (#85 macOS)"
    ${_DAL_DEFAULT})

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

# Detectar arquitetura do processador
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(ARCH_ARM ON)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(ARCH_ARM64 ON)
        message(STATUS "Arquitetura: ARM64 (aarch64)")
    else()
        set(ARCH_ARM32 ON)
        message(STATUS "Arquitetura: ARM32 (armv7/armhf)")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "amd64")
    set(ARCH_X86_64 ON)
    message(STATUS "Arquitetura: x86_64 (amd64)")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i386" OR CMAKE_SYSTEM_PROCESSOR MATCHES "i686")
    set(ARCH_X86 ON)
    message(STATUS "Arquitetura: x86 (i386/i686)")
else()
    message(STATUS "Arquitetura: ${CMAKE_SYSTEM_PROCESSOR} (genérica)")
endif()

# Encontrar dependências
# OpenGL
find_package(OpenGL REQUIRED)

# GLFW (apenas se não estiver usando SDL2)
if(NOT BUILD_WITH_SDL2)
    find_package(glfw3 QUIET)

    if(NOT glfw3_FOUND)
        if(PLATFORM_LINUX)
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(GLFW REQUIRED glfw3)
            set(GLFW_LIBRARIES ${GLFW_LIBRARIES})
            set(GLFW_INCLUDE_DIRS ${GLFW_INCLUDE_DIRS})
        elseif(PLATFORM_WINDOWS AND CMAKE_CROSSCOMPILING)
            # Cross-compiling: primeiro tentar find_package (funciona com MXE)
            # MXE fornece arquivos CMake config para glfw3
            find_package(glfw3 QUIET)

            if(NOT glfw3_FOUND)
                # Fallback: tentar encontrar GLFW manualmente no sistema MinGW-w64
                find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h
                    PATHS
                    /usr/x86_64-w64-mingw32/include
                    /usr/x86_64-w64-mingw32/include/GLFW
                    ${CMAKE_FIND_ROOT_PATH}/include
                    ${CMAKE_FIND_ROOT_PATH}/include/GLFW
                )
                find_library(GLFW_LIBRARY
                    NAMES glfw3 glfw
                    PATHS
                    /usr/x86_64-w64-mingw32/lib
                    ${CMAKE_FIND_ROOT_PATH}/lib
                )

                if(GLFW_INCLUDE_DIR AND GLFW_LIBRARY)
                    set(glfw3_FOUND TRUE)
                    set(GLFW_INCLUDE_DIRS ${GLFW_INCLUDE_DIR})
                    set(GLFW_LIBRARIES ${GLFW_LIBRARY})
                endif()
            endif()

            if(NOT glfw3_FOUND)
                message(WARNING "GLFW não encontrado para MinGW-w64.")
                message(WARNING "Opções:")
                message(WARNING "  1. Compilar GLFW manualmente para MinGW-w64")
                message(WARNING "  2. Usar MXE para compilar dependências: ./setup-mxe.sh")
                message(WARNING "  3. Usar bibliotecas pré-compiladas")
                message(FATAL_ERROR "GLFW é necessário. Veja docs/STATUS_WINDOWS_PORT.md e docs/WINDOWS_CROSS_COMPILE.md para mais informações.")
            endif()
        else()
            message(FATAL_ERROR "GLFW não encontrado. Instale glfw3.")
        endif()
    endif() # NOT BUILD_WITH_SDL2
endif() # NOT BUILD_WITH_SDL2

# SDL2 (opcional, para DirectFB/framebuffer support)
if(BUILD_WITH_SDL2)
    find_package(SDL2 QUIET)

    if(NOT SDL2_FOUND)
        if(PLATFORM_LINUX)
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(SDL2 REQUIRED sdl2)
            set(SDL2_LIBRARIES ${SDL2_LIBRARIES})
            set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
        else()
            message(FATAL_ERROR "SDL2 não encontrado. Instale libsdl2-dev.")
        endif()
    endif()

    message(STATUS "SDL2 encontrado - usando SDL2 em vez de GLFW")
    message(STATUS "  SDL2 version: ${SDL2_VERSION}")
    message(STATUS "  SDL2 libraries: ${SDL2_LIBRARIES}")
    message(STATUS "  SDL2 include dirs: ${SDL2_INCLUDE_DIRS}")

    # Definir macro para código
    add_compile_definitions(USE_SDL2)
else()
    message(STATUS "Usando GLFW (padrão). Use -DBUILD_WITH_SDL2=ON para usar SDL2.")
endif()

# PNG (via libpng)
if(PLATFORM_LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PNG REQUIRED libpng)
    # #86 system tray — StatusNotifierItem over D-Bus. Optional: if
    # libdbus-1 isn't present the tray backend compiles out and the
    # app falls back to the no-op stub (close button quits).
    pkg_check_modules(DBUS dbus-1)
    if(DBUS_FOUND)
        message(STATUS "libdbus-1 found — Linux system-tray backend ENABLED")
    else()
        message(WARNING "libdbus-1 not found — Linux system-tray disabled "
                        "(install libdbus-1-dev / dbus). Close button will quit.")
    endif()
    # #107 screen capture — PipeWire + xdg-desktop-portal (ScreenCast).
    # Needs PipeWire (the frame transport) and libdbus (the portal D-Bus
    # handshake); when either is missing the screen source falls back to
    # the test-pattern stub instead of failing to build.
    pkg_check_modules(PIPEWIRE libpipewire-0.3)
    if(PIPEWIRE_FOUND AND DBUS_FOUND)
        message(STATUS "PipeWire + libdbus found — screen capture (PipeWire/portal) ENABLED")
    else()
        message(WARNING "PipeWire or libdbus missing — screen capture uses the test-pattern stub")
    endif()
    # #107 — EGL enables the DMABUF zero-copy path (import the compositor's
    # GPU buffer straight into a GL texture, no per-frame CPU/SHM copy).
    # Optional: without it the screen backend falls back to the SHM copy.
    pkg_check_modules(EGL egl)
    if(EGL_FOUND AND PIPEWIRE_FOUND AND DBUS_FOUND)
        message(STATUS "EGL found — screen capture DMABUF zero-copy ENABLED")
    else()
        message(WARNING "EGL missing — screen capture uses the SHM copy path (slower)")
    endif()
elseif(PLATFORM_MACOS)
    # macOS: tentar pkg-config primeiro (Homebrew), depois find_package
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PNG REQUIRED libpng)
    else()
        find_package(PNG REQUIRED)
    endif()
else()
    # Windows: tentar pkg-config primeiro (MXE), depois find_package
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        pkg_check_modules(PNG REQUIRED libpng)
    else()
        find_package(PNG REQUIRED)
    endif()
endif()

# FFmpeg for streaming
if(PLATFORM_LINUX)
    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(AVFORMAT REQUIRED libavformat)
    pkg_check_modules(AVUTIL REQUIRED libavutil)
    pkg_check_modules(SWSCALE REQUIRED libswscale)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    
    # Detect FFmpeg version for compatibility
    # Extract version from pkg-config
    execute_process(
        COMMAND pkg-config --modversion libavcodec
        OUTPUT_VARIABLE AVCODEC_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND pkg-config --modversion libavformat
        OUTPUT_VARIABLE AVFORMAT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    # Parse major version (e.g., "60.3.100" -> 60)
    if(AVCODEC_VERSION)
        string(REGEX REPLACE "^([0-9]+)\\..*" "\\1" AVCODEC_VERSION_MAJOR "${AVCODEC_VERSION}")
        message(STATUS "Detected libavcodec version: ${AVCODEC_VERSION} (major: ${AVCODEC_VERSION_MAJOR})")
    endif()
    
    if(AVFORMAT_VERSION)
        string(REGEX REPLACE "^([0-9]+)\\..*" "\\1" AVFORMAT_VERSION_MAJOR "${AVFORMAT_VERSION}")
        message(STATUS "Detected libavformat version: ${AVFORMAT_VERSION} (major: ${AVFORMAT_VERSION_MAJOR})")
    endif()
elseif(PLATFORM_MACOS)
    # macOS: tentar pkg-config (Homebrew), depois find_package
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AVCODEC REQUIRED libavcodec)
        pkg_check_modules(AVFORMAT REQUIRED libavformat)
        pkg_check_modules(AVUTIL REQUIRED libavutil)
        pkg_check_modules(SWSCALE REQUIRED libswscale)
        pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    else()
        find_package(FFmpeg QUIET COMPONENTS avcodec avformat avutil swscale swresample)
        if(FFmpeg_FOUND)
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
        else()
            message(FATAL_ERROR "FFmpeg não encontrado. Instale via Homebrew: brew install ffmpeg")
        endif()
    endif()
else()
    # Windows: usar pkg-config (MXE) ou find_package (vcpkg)
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        # MXE fornece pkg-config, então usar isso
        pkg_check_modules(AVCODEC REQUIRED libavcodec)
        pkg_check_modules(AVFORMAT REQUIRED libavformat)
        pkg_check_modules(AVUTIL REQUIRED libavutil)
        pkg_check_modules(SWSCALE REQUIRED libswscale)
        pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    else()
        # Tentar encontrar via find_package (vcpkg)
        find_package(FFmpeg QUIET COMPONENTS avcodec avformat avutil swscale swresample)

        if(FFmpeg_FOUND)
            set(AVCODEC_LIBRARIES avcodec)
            set(AVFORMAT_LIBRARIES avformat)
            set(AVUTIL_LIBRARIES avutil)
            set(SWSCALE_LIBRARIES swscale)
            set(SWRESAMPLE_LIBRARIES swresample)
        else()
            message(FATAL_ERROR "FFmpeg não encontrado. Use MXE (./setup-mxe.sh) ou vcpkg para instalar.")
        endif()

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

# PulseAudio (Linux only). pulse-simple provides the synchronous API
# used by the remote-stream playback sink (AudioPlaybackPulse) — the
# capture path needs only libpulse, but adding pulse-simple here keeps
# both linkers happy when the playback code is compiled in.
if(PLATFORM_LINUX)
    pkg_check_modules(PULSE REQUIRED libpulse)
    pkg_check_modules(PULSE_SIMPLE REQUIRED libpulse-simple)
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

# ImGui e nlohmann/json (usando ExternalProject para compatibilidade com CMake 3.10)
# Verificar se FetchContent está disponível (CMake 3.11+)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.11")
    include(FetchContent)

    # Configurar FetchContent para não tentar fazer update (evita erro de "dubious ownership")
    # Se o diretório já existe, usamos ele; caso contrário, baixamos do zero
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG docking
    )
    FetchContent_MakeAvailable(imgui)

    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(json)

    # IXWebSocket — WS/WSS client for the chat panel (#84). TLS via the
    # platform-native backend (OpenSSL on Linux, SChannel on Windows,
    # SecureTransport on macOS) — we already require OpenSSL elsewhere
    # so set USE_OPEN_SSL explicitly on Linux. USE_WS builds the `ws`
    # CLI demo; we don't need it. USE_ZLIB enables permessage-deflate;
    # off keeps the dep footprint clean.
    set(USE_TLS         ON  CACHE BOOL "ixwebsocket: TLS support"               FORCE)
    set(USE_WS          OFF CACHE BOOL "ixwebsocket: build the ws CLI demo"     FORCE)
    set(USE_ZLIB        OFF CACHE BOOL "ixwebsocket: permessage-deflate"        FORCE)
    if(PLATFORM_LINUX)
        set(USE_OPEN_SSL ON  CACHE BOOL "ixwebsocket: OpenSSL backend"          FORCE)
        set(USE_MBED_TLS OFF CACHE BOOL "ixwebsocket: mbedTLS backend"          FORCE)
    endif()
    FetchContent_Declare(
        ixwebsocket
        GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
        GIT_TAG v11.4.5
    )
    FetchContent_MakeAvailable(ixwebsocket)
else()
    # Fallback para CMake 3.10: baixar manualmente durante configure
    set(IMGUI_DIR ${CMAKE_BINARY_DIR}/_deps/imgui-src)
    set(JSON_DIR ${CMAKE_BINARY_DIR}/_deps/json-src)

    # Baixar ImGui se não existir
    if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
        message(STATUS "Baixando ImGui...")

        # Remover diretório se existir mas estiver vazio ou corrompido
        if(EXISTS ${IMGUI_DIR})
            file(REMOVE_RECURSE ${IMGUI_DIR})
        endif()

        file(MAKE_DIRECTORY ${IMGUI_DIR})
        execute_process(
            COMMAND git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git ${IMGUI_DIR}
            RESULT_VARIABLE GIT_RESULT
        )

        if(NOT GIT_RESULT EQUAL "0")
            message(FATAL_ERROR "Falha ao baixar ImGui")
        endif()
    endif()

    # Baixar nlohmann/json se não existir
    if(NOT EXISTS ${JSON_DIR}/include/nlohmann/json.hpp)
        message(STATUS "Baixando nlohmann/json...")

        # Remover diretório se existir mas estiver vazio ou corrompido
        if(EXISTS ${JSON_DIR})
            file(REMOVE_RECURSE ${JSON_DIR})
        endif()

        file(MAKE_DIRECTORY ${JSON_DIR})
        execute_process(
            COMMAND git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git ${JSON_DIR}
            RESULT_VARIABLE GIT_RESULT
        )

        if(NOT GIT_RESULT EQUAL "0")
            message(FATAL_ERROR "Falha ao baixar nlohmann/json")
        endif()
    endif()

    # IXWebSocket — mirror of the FetchContent branch above for CMake
    # 3.10 (dockcross/Windows). Cache vars must be set before the
    # add_subdirectory call so they take effect on the first configure.
    # On Linux we'd pick OpenSSL explicitly; this branch only runs on
    # the Windows cross-build, where ixwebsocket defaults to SChannel.
    set(IXWS_DIR ${CMAKE_BINARY_DIR}/_deps/ixwebsocket-src)
    if(NOT EXISTS ${IXWS_DIR}/CMakeLists.txt)
        message(STATUS "Baixando IXWebSocket...")
        if(EXISTS ${IXWS_DIR})
            file(REMOVE_RECURSE ${IXWS_DIR})
        endif()
        file(MAKE_DIRECTORY ${IXWS_DIR})
        execute_process(
            COMMAND git clone --depth 1 --branch v11.4.5 https://github.com/machinezone/IXWebSocket.git ${IXWS_DIR}
            RESULT_VARIABLE GIT_RESULT
        )
        if(NOT GIT_RESULT EQUAL "0")
            message(FATAL_ERROR "Falha ao baixar IXWebSocket")
        endif()
    endif()

    # ixwebsocket hardcodes `Crypt32` (capital C) in its Windows link
    # block. On real MSVC the file system is case-insensitive so that
    # works, but the MinGW cross-toolchain in MXE ships the import
    # lib as `libcrypt32.a` and the Linux host FS *is* case-sensitive,
    # so `-lCrypt32` fails to resolve. Rewrite to lowercase in place.
    # `string(REPLACE)` is a no-op once the patch is already applied,
    # so this stays idempotent on subsequent configures.
    file(READ ${IXWS_DIR}/CMakeLists.txt _IXWS_CML)
    string(REPLACE "PRIVATE Crypt32" "PRIVATE crypt32" _IXWS_CML "${_IXWS_CML}")
    file(WRITE ${IXWS_DIR}/CMakeLists.txt "${_IXWS_CML}")
    # ixwebsocket v11.4.5 doesn't auto-pick a Windows backend, it
    # defaults to mbedtls (which MXE doesn't ship). Force OpenSSL
    # since MXE builds it and our HTTPS support already links it.
    set(USE_TLS         ON  CACHE BOOL "ixwebsocket: TLS support"           FORCE)
    set(USE_WS          OFF CACHE BOOL "ixwebsocket: build the ws CLI demo" FORCE)
    set(USE_ZLIB        OFF CACHE BOOL "ixwebsocket: permessage-deflate"    FORCE)
    set(USE_OPEN_SSL    ON  CACHE BOOL "ixwebsocket: OpenSSL backend"       FORCE)
    set(USE_MBED_TLS    OFF CACHE BOOL "ixwebsocket: mbedTLS backend"       FORCE)
    add_subdirectory(${IXWS_DIR} ${CMAKE_BINARY_DIR}/_deps/ixwebsocket-build EXCLUDE_FROM_ALL)

    set(imgui_SOURCE_DIR ${IMGUI_DIR})
    set(json_SOURCE_DIR ${JSON_DIR})
endif()

# Criar target para ImGui
if(BUILD_WITH_SDL2)
    # ImGui com backend SDL2
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
else()
    # ImGui com backend GLFW (padrão)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
endif()

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

if(BUILD_WITH_SDL2)
    target_include_directories(imgui PUBLIC ${SDL2_INCLUDE_DIRS})
endif()

if(BUILD_WITH_SDL2)
    target_link_libraries(imgui PUBLIC
        ${SDL2_LIBRARIES}
        OpenGL::GL
    )
else()
    target_link_libraries(imgui PUBLIC
        glfw
        OpenGL::GL
    )

    if(PLATFORM_LINUX)
        target_link_libraries(imgui PUBLIC X11 dl)
    endif()
endif()

# Fontes
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.mm"
    "src/*.h"
)

# #85 — the platform-specific virtcam filter/plug-in lives under
# its own subdirectory (src/dshow_filter for Windows, src/dal_plugin
# for macOS) and is its OWN target (built via add_subdirectory when
# the matching option is set). Always remove these sources from the
# main retrocapture executable build so the SOURCES glob doesn't
# drag them in.
list(FILTER SOURCES EXCLUDE REGEX ".*/dshow_filter/.*\\.(cpp|h)$")
list(FILTER SOURCES EXCLUDE REGEX ".*/dal_plugin/.*\\.(cpp|h|mm)$")

# Excluir implementações de outras plataformas
if(PLATFORM_LINUX)
    # No Linux, excluir implementações Windows e macOS
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureDS\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*DSFrameGrabber\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*DSPin\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VirtualCameraOutputWin\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VirtualCameraOutputMac\\.(cpp|h)$")
    # #86 tray: the Linux SNI backend needs libdbus; drop it when absent.
    if(NOT DBUS_FOUND)
        list(FILTER SOURCES EXCLUDE REGEX ".*SystemTrayLinux\\.(cpp|h)$")
    endif()
    list(FILTER SOURCES EXCLUDE REGEX ".*SystemTrayWindows\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*SystemTrayMac\\.(mm|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioCaptureWASAPI\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureAVFoundation.*\\.(mm|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureScreen_mac\\.(mm|h)$") # #107 macOS SCK backend
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioCaptureCoreAudio.*\\.(mm|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*SystemAudioCaptureSCK\\.(mm|h)$") 
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioOutputCoreAudio.*\\.(mm|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioPlaybackCoreAudio\\.(cpp|h)$")

    # Excluir WindowManager baseado na opção BUILD_WITH_SDL2
    if(BUILD_WITH_SDL2)
        # Usar SDL2: excluir WindowManager.cpp (GLFW)
        list(FILTER SOURCES EXCLUDE REGEX ".*/WindowManager\\.cpp$")
    else()
        # Usar GLFW: excluir WindowManagerSDL.cpp
        list(FILTER SOURCES EXCLUDE REGEX ".*/WindowManagerSDL\\.cpp$")
    endif()
elseif(PLATFORM_WINDOWS)
    # No Windows, excluir implementações Linux e macOS.
    # `VirtualCameraOutput\\.cpp` (NOT VirtualCameraOutputWin) is the
    # Linux v4l2loopback sink — must be excluded. UIConfigurationVirtualCamera
    # is now compiled on both platforms (#85 Phase 2 surface).
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureV4L2\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VirtualCameraOutput\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VirtualCameraOutputMac\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*SystemTrayLinux\\.(cpp|h)$") # #86 Linux-only tray backend
    list(FILTER SOURCES EXCLUDE REGEX ".*SystemTrayMac\\.(mm|h)$")    # #86 macOS-only tray backend
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioCapturePulse\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*PipeSourcePublisher\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*MonitorPlayback\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioBus\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*V4L2.*\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureAVFoundation.*\\.(mm|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureScreen_mac\\.(mm|h)$") # #107 macOS SCK backend
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioCaptureCoreAudio.*\\.(mm|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*SystemAudioCaptureSCK\\.(mm|h)$") 
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioOutputCoreAudio.*\\.(mm|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioPlaybackCoreAudio\\.(cpp|h)$")

    # Excluir arquivos Linux genéricos que não têm versão Windows
    list(FILTER SOURCES EXCLUDE REGEX ".*/VideoCapture\\.cpp$") # VideoCapture.cpp é Linux
    list(FILTER SOURCES EXCLUDE REGEX ".*/AudioCapture\\.cpp$") # AudioCapture.cpp é Linux

    # HTTPServer.cpp foi adaptado para Windows - incluir no build
    # list(FILTER SOURCES EXCLUDE REGEX ".*/HTTPServer\\.cpp$")      # HTTPServer.cpp agora suporta Windows
    # OpenGLRenderer.cpp e FrameProcessor.cpp agora suportam Windows (usam #ifdef __linux__)
    # list(FILTER SOURCES EXCLUDE REGEX ".*/OpenGLRenderer\\.cpp$") # OpenGLRenderer.cpp agora suporta Windows
    # list(FILTER SOURCES EXCLUDE REGEX ".*/FrameProcessor\\.cpp$") # FrameProcessor.cpp agora suporta Windows
    list(FILTER SOURCES EXCLUDE REGEX ".*/V4L2DeviceScanner\\.(cpp|h)$") # V4L2DeviceScanner é Linux
    list(FILTER SOURCES EXCLUDE REGEX ".*/V4L2ControlMapper\\.(cpp|h)$") # V4L2ControlMapper é Linux
elseif(PLATFORM_MACOS)
    # No macOS, excluir implementações Linux e Windows.
    # `VirtualCameraOutput\\.cpp` (Linux v4l2 sink) and `VirtualCameraOutputWin\\.cpp`
    # (Windows DShow sink) must be excluded. `VirtualCameraOutputMac\\.cpp`
    # stays — it's the new POSIX-shm sink for the DAL plug-in path.
    # UIConfigurationVirtualCamera now compiles on all three (#85).
    # Note: `AudioBus` is platform-agnostic (std::deque + mutex) and is
    # used by both AudioCapturePulse on Linux and AudioCaptureCoreAudio
    # on macOS, so it is NOT excluded here.
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureV4L2\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VirtualCameraOutput\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*VirtualCameraOutputWin\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*SystemTrayLinux\\.(cpp|h)$")   # #86 Linux-only tray backend
    list(FILTER SOURCES EXCLUDE REGEX ".*SystemTrayWindows\\.(cpp|h)$") # #86 Windows-only tray backend
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoCaptureDS\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*DSFrameGrabber\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*DSPin\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioCapturePulse\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*AudioCaptureWASAPI\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*PipeSourcePublisher\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*MonitorPlayback\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*V4L2.*\\.(cpp|h)$")

    # Excluir arquivos Linux genéricos
    list(FILTER SOURCES EXCLUDE REGEX ".*/VideoCapture\\.cpp$") # VideoCapture.cpp é Linux
    list(FILTER SOURCES EXCLUDE REGEX ".*/AudioCapture\\.cpp$") # AudioCapture.cpp é Linux
    list(FILTER SOURCES EXCLUDE REGEX ".*/V4L2DeviceScanner\\.(cpp|h)$")
    list(FILTER SOURCES EXCLUDE REGEX ".*/V4L2ControlMapper\\.(cpp|h)$")

    # macOS usa GLFW (não SDL2 por padrão)
    list(FILTER SOURCES EXCLUDE REGEX ".*/WindowManagerSDL\\.cpp$")
endif()

# Executável principal
if(PLATFORM_WINDOWS)
    # Adicionar arquivo de recursos (.rc) para ícone do executável
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/retrocapture.rc" AND EXISTS "${CMAKE_SOURCE_DIR}/assets/logo.ico")
        # Copiar logo.ico para o diretório de build antes de compilar
        # Usar add_custom_command para garantir que seja copiado durante o build
        add_custom_command(
            OUTPUT "${CMAKE_BINARY_DIR}/logo.ico"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/logo.ico"
            "${CMAKE_BINARY_DIR}/logo.ico"
            DEPENDS "${CMAKE_SOURCE_DIR}/assets/logo.ico"
            COMMENT "Copying logo.ico for resource compilation"
        )

        add_executable(retrocapture
            ${SOURCES}
            ${CMAKE_SOURCE_DIR}/src/retrocapture.rc
            "${CMAKE_BINARY_DIR}/logo.ico"
        )
        message(STATUS "Windows resource file (.rc) added for executable icon")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/src/retrocapture.rc")
        add_executable(retrocapture
            ${SOURCES}
            ${CMAKE_SOURCE_DIR}/src/retrocapture.rc
        )
        message(WARNING "logo.ico not found, executable icon may not work correctly")
    else()
        add_executable(retrocapture
            ${SOURCES}
        )
        message(WARNING "retrocapture.rc not found, executable will not have custom icon")
    endif()
else()
    add_executable(retrocapture
        ${SOURCES}
    )
endif()

# Definir macros de plataforma
if(PLATFORM_LINUX)
    target_compile_definitions(retrocapture PRIVATE PLATFORM_LINUX)
elseif(PLATFORM_WINDOWS)
    target_compile_definitions(retrocapture PRIVATE PLATFORM_WINDOWS)
elseif(PLATFORM_MACOS)
    target_compile_definitions(retrocapture PRIVATE PLATFORM_MACOS)
endif()

# #86 — Linux system-tray backend (StatusNotifierItem over D-Bus).
# Enable the backend + link libdbus only when it was found; otherwise
# the SystemTrayLinux.cpp source was already filtered out above and the
# no-op stub stands in.
if(PLATFORM_LINUX AND DBUS_FOUND)
    target_compile_definitions(retrocapture PRIVATE RETROCAPTURE_TRAY_BACKEND_LINUX)
    target_include_directories(retrocapture PRIVATE ${DBUS_INCLUDE_DIRS})
    target_link_libraries(retrocapture PRIVATE ${DBUS_LIBRARIES})
endif()

# #107 — Linux screen-capture backend (PipeWire stream + portal over
# D-Bus). Gated on both deps; VideoCaptureScreen_linux.cpp keys off
# RETROCAPTURE_SCREEN_PIPEWIRE and the stub stands in when it's absent.
if(PLATFORM_LINUX AND PIPEWIRE_FOUND AND DBUS_FOUND)
    target_compile_definitions(retrocapture PRIVATE RETROCAPTURE_SCREEN_PIPEWIRE)
    target_include_directories(retrocapture PRIVATE ${PIPEWIRE_INCLUDE_DIRS} ${DBUS_INCLUDE_DIRS})
    target_link_libraries(retrocapture PRIVATE ${PIPEWIRE_LIBRARIES} ${DBUS_LIBRARIES})
    if(EGL_FOUND)
        target_compile_definitions(retrocapture PRIVATE RETROCAPTURE_SCREEN_DMABUF)
        target_include_directories(retrocapture PRIVATE ${EGL_INCLUDE_DIRS})
        target_link_libraries(retrocapture PRIVATE ${EGL_LIBRARIES})
    endif()
endif()

# #107 — Windows screen-capture backend (DXGI Desktop Duplication).
# VideoCaptureScreen_win.cpp keys off RETROCAPTURE_SCREEN_DXGI; d3d11/dxgi
# headers + libs ship with MinGW-w64 / MXE.
if(PLATFORM_WINDOWS)
    target_compile_definitions(retrocapture PRIVATE RETROCAPTURE_SCREEN_DXGI)
    target_link_libraries(retrocapture PRIVATE d3d11 dxgi)
endif()

# #107 — macOS screen-capture backend (ScreenCaptureKit). The .mm is
# excluded from non-Apple builds (below); the framework is 12.3+.
if(PLATFORM_MACOS)
    target_compile_definitions(retrocapture PRIVATE RETROCAPTURE_SCREEN_SCK)
    target_link_libraries(retrocapture PRIVATE
        "-framework ScreenCaptureKit"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics")
endif()

if(PLATFORM_WINDOWS)
    # Shell_NotifyIcon + TrackPopupMenu — shell32/user32/gdi32 are part
    # of the standard Win32 link set (MXE links them by default, but be
    # explicit).
    target_compile_definitions(retrocapture PRIVATE RETROCAPTURE_TRAY_BACKEND_WINDOWS)
    target_link_libraries(retrocapture PRIVATE shell32 user32 gdi32)
elseif(PLATFORM_MACOS)
    # NSStatusItem lives in AppKit.
    target_compile_definitions(retrocapture PRIVATE RETROCAPTURE_TRAY_BACKEND_MACOS)
    target_link_libraries(retrocapture PRIVATE "-framework AppKit")
endif()

# Propagar versão do projeto pro código (usado pelo endpoint /meta e afins)
target_compile_definitions(retrocapture PRIVATE
    RETROCAPTURE_VERSION="${PROJECT_VERSION}")

# Threading
find_package(Threads REQUIRED)

# Link libraries
set(LINK_LIBS
    OpenGL::GL
    Threads::Threads
    imgui
)

if(BUILD_WITH_SDL2)
    # Link SDL2 quando usando SDL2
    list(APPEND LINK_LIBS ${SDL2_LIBRARIES})
    target_include_directories(retrocapture PRIVATE ${SDL2_INCLUDE_DIRS})
else()
    # Link GLFW quando usando GLFW (padrão)
    if(glfw3_FOUND)
        list(APPEND LINK_LIBS glfw)
    else()
        list(APPEND LINK_LIBS ${GLFW_LIBRARIES})
    endif()
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
    # X11 is needed for WM_CLASS setting in WindowManager
    find_package(X11 QUIET)

    if(X11_FOUND)
        list(APPEND LINK_LIBS ${X11_LIBRARIES})
        list(APPEND INCLUDE_DIRS ${X11_INCLUDE_DIR})
    else()
        # Fallback: try to find X11 via pkg-config
        find_package(PkgConfig QUIET)

        if(PkgConfig_FOUND)
            pkg_check_modules(X11 QUIET x11)

            if(X11_FOUND)
                list(APPEND LINK_LIBS ${X11_LIBRARIES})
                list(APPEND INCLUDE_DIRS ${X11_INCLUDE_DIRS})
            endif()
        endif()
    endif()

    list(APPEND LINK_LIBS ${V4L2_LIBRARIES} ${PULSE_LIBRARIES} ${PULSE_SIMPLE_LIBRARIES})
elseif(PLATFORM_MACOS)
    # macOS frameworks discovered via find_library; the ones that
    # need -framework syntax (AVFoundation, CoreAudio, etc.) are
    # passed directly to target_link_libraries below since they
    # don't follow the regular library-search-path convention.
    find_library(COCOA_LIB Cocoa)
    find_library(IOKIT_LIB IOKit)
    find_library(COREVIDEO_LIB CoreVideo)
    find_library(VIDEOTOOLBOX_LIB VideoToolbox)

    if(COCOA_LIB)
        list(APPEND LINK_LIBS ${COCOA_LIB})
    endif()
    if(IOKIT_LIB)
        list(APPEND LINK_LIBS ${IOKIT_LIB})
    endif()
    if(COREVIDEO_LIB)
        list(APPEND LINK_LIBS ${COREVIDEO_LIB})
    endif()
    if(VIDEOTOOLBOX_LIB)
        list(APPEND LINK_LIBS ${VIDEOTOOLBOX_LIB})
    endif()
elseif(PLATFORM_WINDOWS)
    # Windows DirectShow libraries
    list(APPEND LINK_LIBS
        strmiids # DirectShow (Streaming Media IDs)
        ole32 # COM
        oleaut32 # COM Automation
    )

    # Windows Audio Session API (WASAPI) libraries
    list(APPEND LINK_LIBS
        avrt

        # propsys não está disponível no MXE - PKEY_Device_FriendlyName está definido estaticamente no código
    )
endif()

target_link_libraries(retrocapture PRIVATE ${LINK_LIBS})

# IXWebSocket — chat WS/WSS transport (#84). FetchContent created the
# `ixwebsocket` target; on macOS it also pulls in Security + Foundation
# automatically via SecureTransport, and on Linux it links OpenSSL we
# already require. Linked PRIVATE because the public headers don't
# expose IXWebSocket types.
target_link_libraries(retrocapture PRIVATE ixwebsocket)

# macOS-specific frameworks (need -framework syntax)
if(PLATFORM_MACOS)
    target_link_libraries(retrocapture PRIVATE
        "-framework AVFoundation"
        "-framework CoreAudio"
        "-framework CoreVideo"
        "-framework CoreMedia"
        "-framework VideoToolbox"
        "-framework Foundation"
        "-framework AudioToolbox"
    )
endif()

# MinGW-w64 precisa linkar com stdc++fs para filesystem (GCC 8+)
# Para GCC < 8, nossa implementação manual usa shlwapi
if(PLATFORM_WINDOWS AND CMAKE_CROSSCOMPILING AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Verificar versão do GCC
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -dumpversion
        OUTPUT_VARIABLE GCC_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(GCC_VERSION VERSION_GREATER_EQUAL "8.0")
        target_link_libraries(retrocapture PRIVATE stdc++fs)
    else()
        # GCC < 8: usar nossa implementação manual que precisa de shlwapi
        target_link_libraries(retrocapture PRIVATE shlwapi)
    endif()
endif()

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
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG")

# Compiler flags base (comuns a todos os builds)
target_compile_options(retrocapture PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Flags específicas por build type e arquitetura
if(ARCH_X86_64)
    # x86_64: usar -march=native para otimização máxima (padrão)
    # Se BUILD_COMPATIBLE_X86_64 estiver ativado, usar baseline portátil (sem AVX/AVX2)
    if(BUILD_COMPATIBLE_X86_64)
        # Preferir -march=x86-64-v2 (GCC 11+ / Clang 12+): baseline padronizado SSE4.2 + popcnt.
        # Em compiladores antigos, cair em flags equivalentes explícitas.
        # Importante: as flags ficam inline em cada genex (não via ${VAR}) porque CMake
        # trata variáveis dentro de genex como tokens únicos e quota com aspas, fazendo
        # o compilador receber tudo como um único valor de -march=.
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=x86-64-v2" COMPILER_SUPPORTS_X86_64_V2)
        if(COMPILER_SUPPORTS_X86_64_V2)
            message(STATUS "Usando baseline portátil x86-64-v2 (SSE4.2, sem AVX/AVX2)")
            target_compile_options(retrocapture PRIVATE
                $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
                $<$<CONFIG:Release>:-O3 -DNDEBUG -march=x86-64-v2 -mtune=generic>
                $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG -march=x86-64-v2 -mtune=generic>
                $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG -march=x86-64-v2 -mtune=generic>
            )
            if(TARGET imgui)
                target_compile_options(imgui PRIVATE
                    $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
                    $<$<CONFIG:Release>:-O3 -DNDEBUG -march=x86-64-v2 -mtune=generic>
                    $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG -march=x86-64-v2 -mtune=generic>
                    $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG -march=x86-64-v2 -mtune=generic>
                )
            endif()
        else()
            message(STATUS "Compilador sem suporte a x86-64-v2: usando flags equivalentes explícitas")
            target_compile_options(retrocapture PRIVATE
                $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
                $<$<CONFIG:Release>:-O3 -DNDEBUG -march=x86-64 -mtune=generic -msse4.2 -mno-avx -mno-avx2>
                $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG -march=x86-64 -mtune=generic -msse4.2 -mno-avx -mno-avx2>
                $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG -march=x86-64 -mtune=generic -msse4.2 -mno-avx -mno-avx2>
            )
            if(TARGET imgui)
                target_compile_options(imgui PRIVATE
                    $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
                    $<$<CONFIG:Release>:-O3 -DNDEBUG -march=x86-64 -mtune=generic -msse4.2 -mno-avx -mno-avx2>
                    $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG -march=x86-64 -mtune=generic -msse4.2 -mno-avx -mno-avx2>
                    $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG -march=x86-64 -mtune=generic -msse4.2 -mno-avx -mno-avx2>
                )
            endif()
        endif()
    else()
        # Otimização máxima: usa todas as instruções da CPU atual
        message(STATUS "Usando otimização máxima para x86-64 (-march=native)")
        target_compile_options(retrocapture PRIVATE
            $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
            $<$<CONFIG:Release>:-O3 -DNDEBUG -march=native>
            $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
            $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
        )
    endif()
elseif(ARCH_ARM32)
    # ARM32 (armv7/armhf): usar flags específicas para ARM
    # -march=armv7-a: suporta ARMv7-A (Raspberry Pi 3)
    # -mfpu=neon-vfpv4: usa NEON e VFPv4 (disponível no Pi 3)
    # -mfloat-abi=hard: usa hardware floating point
    target_compile_options(retrocapture PRIVATE
        $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O3 -DNDEBUG -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard>
        $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard>
    )
elseif(ARCH_ARM64)
    # ARM64 (aarch64): -O2 (não -O3) e ImGui em -O1 + -fno-optimize-sibling-calls
    # como workaround pra segfault do GCC 14+ ao compilar ImGui (independente de portabilidade).
    # Se BUILD_COMPATIBLE_ARM64 estiver ativado, usar baseline portátil ARMv8-A;
    # caso contrário, manter -march=native (uso local).
    if(BUILD_COMPATIBLE_ARM64)
        # Baseline ARMv8-A sem extensões opcionais — roda em qualquer CPU desde
        # Cortex-A53 (Pi 3) até CPUs novas. Importante para builds Docker arm64v8 sob
        # qemu, cujo modelo "max" pode habilitar SVE/SVE2 que crasha em Pi 4/5 reais.
        message(STATUS "Usando baseline portátil ARMv8-A (sem SVE/crypto)")
        target_compile_options(retrocapture PRIVATE
            $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
            $<$<CONFIG:Release>:-O2 -DNDEBUG -march=armv8-a>
            $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG -march=armv8-a>
            $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG -march=armv8-a>
        )
        if(TARGET imgui)
            target_compile_options(imgui PRIVATE
                $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer -fno-optimize-sibling-calls>
                $<$<CONFIG:Release>:-O1 -DNDEBUG -march=armv8-a -fno-optimize-sibling-calls>
                $<$<CONFIG:RelWithDebInfo>:-O1 -g -DNDEBUG -march=armv8-a -fno-optimize-sibling-calls>
                $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG -march=armv8-a -fno-optimize-sibling-calls>
            )
        endif()
    else()
        message(STATUS "Usando otimização nativa para ARM64 (-march=native)")
        target_compile_options(retrocapture PRIVATE
            $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
            $<$<CONFIG:Release>:-O2 -DNDEBUG -march=native>
            $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
            $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
        )
        if(TARGET imgui)
            target_compile_options(imgui PRIVATE
                $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer -fno-optimize-sibling-calls>
                $<$<CONFIG:Release>:-O1 -DNDEBUG -fno-optimize-sibling-calls>
                $<$<CONFIG:RelWithDebInfo>:-O1 -g -DNDEBUG -fno-optimize-sibling-calls>
                $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG -fno-optimize-sibling-calls>
            )
        endif()
    endif()
else()
    # Arquitetura genérica: sem flags específicas de arquitetura
    target_compile_options(retrocapture PRIVATE
        $<$<CONFIG:Debug>:-g -O0 -DDEBUG -fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
        $<$<CONFIG:MinSizeRel>:-Os -DNDEBUG>
    )
endif()

# Linker flags específicas por build type
# target_link_options não disponível no CMake 3.10 (requer 3.13+)
# As flags de linker são opcionais e não críticas para o build

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

    # Resolver conflito de símbolos duplicados entre OpenSSL e GnuTLS (usado pelo FFmpeg)
    # Ambos definem funções AES-NI, então precisamos permitir múltiplas definições
    if(PLATFORM_WINDOWS AND CMAKE_CROSSCOMPILING)
        # Usar --allow-multiple-definition para resolver conflitos de símbolos
        # Isso é necessário porque FFmpeg linka com GnuTLS que tem suas próprias implementações AES-NI
        # que conflitam com as do OpenSSL
        # CMake 3.10 não tem target_link_options, então usamos set_target_properties
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            get_target_property(CURRENT_LINK_FLAGS retrocapture LINK_FLAGS)

            if(CURRENT_LINK_FLAGS)
                set_target_properties(retrocapture PROPERTIES
                    LINK_FLAGS "${CURRENT_LINK_FLAGS} -Wl,--allow-multiple-definition"
                )
            else()
                set_target_properties(retrocapture PROPERTIES
                    LINK_FLAGS "-Wl,--allow-multiple-definition"
                )
            endif()
        endif()
    endif()
endif()

# Instalação e CPack configuration
if(PLATFORM_WINDOWS)
    # CPack configuration for Windows installer
    # IMPORTANTE: Todas as variáveis CPACK_* devem ser definidas ANTES do include(CPack)

    # Basic package information
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_NAME "RetroCapture")
    set(CPACK_PACKAGE_VENDOR "RetroCapture")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Video capture with RetroArch shader support")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

    # Pasta de instalação: apenas "RetroCapture" sem versão
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "RetroCapture")
    set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "RetroCapture")

    # NSIS: garantir que não adicione versão ao diretório de instalação
    set(CPACK_NSIS_INSTALL_DIRECTORY "$PROGRAMFILES\\RetroCapture")

    # Definir nome do arquivo do instalador explicitamente
    # Formato: RetroCapture-{VERSION}-Windows-Setup.exe
    # IMPORTANTE: Deve ser definido ANTES do include(CPack)
    # Usar PROJECT_VERSION diretamente para garantir que está usando a versão correta
    set(CPACK_PACKAGE_FILE_NAME "RetroCapture-${PROJECT_VERSION}-Windows-Setup")
    message(STATUS "CPack package file name: ${CPACK_PACKAGE_FILE_NAME}")

    # License file - incluir texto do LICENSE na etapa de licença do instalador
    if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
        set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
        message(STATUS "License file configurado: ${CPACK_RESOURCE_FILE_LICENSE}")
    else()
        message(WARNING "Arquivo LICENSE não encontrado em ${CMAKE_SOURCE_DIR}/LICENSE")
    endif()

    # Garantir que a página de licença seja exibida (não ignorar)
    set(CPACK_NSIS_IGNORE_LICENSE_PAGE OFF)

    # Icon/logo for installer
    # NSIS prefers .ico files for installer icon
    if(EXISTS "${CMAKE_SOURCE_DIR}/assets/logo.ico")
        set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/assets/logo.ico")
        message(STATUS "Using logo.ico for NSIS installer icon")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/assets/logo.png")
        # Fallback to PNG if .ico is not available
        set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/assets/logo.png")
        message(STATUS "Using logo.png for NSIS installer icon (fallback)")
    endif()

    # NSIS specific settings
    set(CPACK_NSIS_PACKAGE_NAME "RetroCapture")
    set(CPACK_NSIS_DISPLAY_NAME "RetroCapture ${PROJECT_VERSION}")
    set(CPACK_NSIS_HELP_LINK "https://github.com/yourusername/retrocapture")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/yourusername/retrocapture")
    set(CPACK_NSIS_CONTACT "your-email@example.com")
    set(CPACK_NSIS_MODIFY_PATH ON) # Adicionar ao PATH
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON) # Desinstalar versão anterior se existir

    # Configurar caminho do NSIS (CPack precisa encontrar makensis)
    # Tentar encontrar makensis no sistema
    find_program(NSIS_MAKENSIS makensis PATHS /usr/bin /usr/local/bin)

    if(NSIS_MAKENSIS)
        set(CPACK_NSIS_EXECUTABLE "${NSIS_MAKENSIS}")
        message(STATUS "NSIS encontrado: ${NSIS_MAKENSIS}")
    else()
        message(WARNING "NSIS makensis não encontrado. CPack pode falhar ao gerar instalador.")
    endif()

    # Component descriptions (devem estar antes do include(CPack))
    set(CPACK_COMPONENT_APPLICATION_DISPLAY_NAME "RetroCapture Application")
    set(CPACK_COMPONENT_APPLICATION_DESCRIPTION "Main RetroCapture executable and required DLLs")
    set(CPACK_COMPONENT_APPLICATION_REQUIRED ON)

    set(CPACK_COMPONENT_SHADERS_DISPLAY_NAME "Shader Presets")
    set(CPACK_COMPONENT_SHADERS_DESCRIPTION "RetroArch-compatible shader presets")
    set(CPACK_COMPONENT_SHADERS_REQUIRED ON)

    set(CPACK_COMPONENT_ASSETS_DISPLAY_NAME "Assets")
    set(CPACK_COMPONENT_ASSETS_DESCRIPTION "Application assets (icons, logos, etc.)")
    set(CPACK_COMPONENT_ASSETS_REQUIRED ON)

    set(CPACK_COMPONENT_WEB_DISPLAY_NAME "Web Portal")
    set(CPACK_COMPONENT_WEB_DESCRIPTION "Web-based control interface")
    set(CPACK_COMPONENT_WEB_REQUIRED ON)

    set(CPACK_COMPONENT_SSL_DISPLAY_NAME "SSL Certificates")
    set(CPACK_COMPONENT_SSL_DESCRIPTION "SSL certificates for HTTPS support")
    set(CPACK_COMPONENT_SSL_REQUIRED ON)

    # Start Menu + Desktop shortcuts (must be set before include(CPack)).
    # #131 — the previous CreateShortCut used $START_MENU_FOLDER, but the
    # CPack NSIS template's variable is $STARTMENU_FOLDER (no underscore in
    # the middle); the wrong name expanded empty, so no Start Menu shortcut
    # was created. Also add a Desktop shortcut ($DESKTOP is always valid).
    # The shortcut icon comes from the icon embedded in retrocapture.exe.
    # Kept in CREATE_ICONS_EXTRA (not EXTRA_INSTALL_COMMANDS) so it doesn't
    # collide with the virtcam regsvr32 below (#133).
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\RetroCapture.lnk' '$INSTDIR\\\\bin\\\\retrocapture.exe'"
        "CreateShortCut '$DESKTOP\\\\RetroCapture.lnk' '$INSTDIR\\\\bin\\\\retrocapture.exe'"
    )
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\RetroCapture.lnk'"
        "Delete '$DESKTOP\\\\RetroCapture.lnk'"
    )

    # #85 Phase 2 — register / unregister the DirectShow virtual
    # camera filter DLL post-install. NSIS runs elevated
    # (RequestExecutionLevel admin) so regsvr32 has the rights it
    # needs to write under HKCR. /s = silent; the user shouldn't
    # see a "DLL registered successfully" popup on every install.
    # Guarded by RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER so a
    # build without the filter doesn't ship a broken post-install
    # call pointing at a missing DLL.
    if(RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER)
        set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
            "ExecWait 'regsvr32.exe /s \\\"$INSTDIR\\\\bin\\\\RetroCaptureVCam.dll\\\"'")
        set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
            "ExecWait 'regsvr32.exe /u /s \\\"$INSTDIR\\\\bin\\\\RetroCaptureVCam.dll\\\"'")
    endif()

    # Incluir CPack APÓS todas as configurações CPACK_*
    include(CPack)

    # Install components
    # Executável e DLLs
    install(TARGETS retrocapture
        RUNTIME DESTINATION bin
        COMPONENT application
    )

    # Shaders
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/shaders/shaders_glsl"
        DESTINATION share/retrocapture/shaders
        COMPONENT shaders
        FILES_MATCHING PATTERN "*"
    )

    # Assets (logo, etc.)
    if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets"
            DESTINATION share/retrocapture
            COMPONENT assets
            FILES_MATCHING PATTERN "*"
        )
    endif()

    # Web portal
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/web")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/web"
            DESTINATION share/retrocapture
            COMPONENT web
            FILES_MATCHING PATTERN "*"
        )
    endif()

    # SSL certificates
    if(EXISTS "${CMAKE_SOURCE_DIR}/ssl")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/ssl"
            DESTINATION share/retrocapture
            COMPONENT ssl
            FILES_MATCHING PATTERN "*"
        )
    endif()

    # Mensagens de status (após include(CPack) e install)
    message(STATUS "CPack configured for Windows installer (NSIS)")
    message(STATUS "  Package name: ${CPACK_PACKAGE_NAME}")
    message(STATUS "  Version: ${CPACK_PACKAGE_VERSION}")
    message(STATUS "  Generator: ${CPACK_GENERATOR}")

    if(DEFINED CPACK_PACKAGE_FILE_NAME)
        message(STATUS "  Output file: ${CPACK_PACKAGE_FILE_NAME}.exe")
    endif()
else()
    # Instalação Linux: respeita XDG. Por default cai em /usr/local/{bin,share}
    # via CMAKE_INSTALL_PREFIX. Pacotagem (.deb/.rpm) overridea pra /usr.
    install(TARGETS retrocapture
        RUNTIME DESTINATION bin
    )

    # Read-only assets shipados — Paths::getReadOnlyAssetsDir() encontra via
    # XDG_DATA_DIRS (/usr/share, /usr/local/share) → retrocapture/.
    if(EXISTS "${CMAKE_SOURCE_DIR}/shaders/shaders_glsl")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/shaders/shaders_glsl"
            DESTINATION share/retrocapture/shaders
            FILES_MATCHING PATTERN "*"
        )
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/"
            DESTINATION share/retrocapture
            FILES_MATCHING PATTERN "*"
        )
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/src/web")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/web"
            DESTINATION share/retrocapture
            FILES_MATCHING PATTERN "*"
        )
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/ssl")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/ssl"
            DESTINATION share/retrocapture
            FILES_MATCHING PATTERN "*"
        )
    endif()
endif()

# ---------------------------------------------------------------------
# #85 Phase 2 — Windows DirectShow virtual-camera source filter DLL.
#
# Two add_subdirectory calls in order: strmbase first (the DirectShow
# BaseClasses static lib), then the filter sources that link against
# it. Gated by the option so a default Linux build doesn't pay the
# configure-time cost of an irrelevant target tree.
# ---------------------------------------------------------------------
if(RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER)
    if(NOT PLATFORM_WINDOWS)
        message(FATAL_ERROR
            "RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER=ON only makes "
            "sense on a Windows target build (cross-compile via MXE "
            "or native MSVC). Current platform is not Windows.")
    endif()
    add_subdirectory(third_party/baseclasses)
    add_subdirectory(src/dshow_filter)
endif()

if(RETROCAPTURE_BUILD_VIRTCAM_DAL_PLUGIN)
    if(NOT PLATFORM_MACOS)
        message(FATAL_ERROR
            "RETROCAPTURE_BUILD_VIRTCAM_DAL_PLUGIN=ON only makes "
            "sense on a macOS target build. Current platform is "
            "not macOS.")
    endif()
    add_subdirectory(src/dal_plugin)
endif()
