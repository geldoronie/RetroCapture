# DirectShow BaseClasses (MS, MinGW-patched) → static `strmbase`.
#
# Source list mirrors the upstream's own CMakeLists.txt
# (TianZerL/DirectShow-BaseClasses-MultiCompiler) verbatim — only
# the file is rewritten so it runs under CMake 3.10 (dockcross /
# MXE) and so it integrates as a subdir target instead of a
# stand-alone project.
#
# Consumers (src/dshow_filter/) get the include dir + the lib
# automatically via target_link_libraries(... strmbase).

set(BC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(strmbase STATIC
    ${BC_DIR}/amextra.cpp
    ${BC_DIR}/amfilter.cpp
    ${BC_DIR}/amvideo.cpp
    ${BC_DIR}/arithutil.cpp
    ${BC_DIR}/combase.cpp
    ${BC_DIR}/cprop.cpp
    ${BC_DIR}/ctlutil.cpp
    ${BC_DIR}/ddmm.cpp
    ${BC_DIR}/dllentry.cpp
    ${BC_DIR}/dllsetup.cpp
    ${BC_DIR}/mtype.cpp
    ${BC_DIR}/outputq.cpp
    ${BC_DIR}/perflog.cpp
    ${BC_DIR}/pstream.cpp
    ${BC_DIR}/pullpin.cpp
    ${BC_DIR}/refclock.cpp
    ${BC_DIR}/renbase.cpp
    ${BC_DIR}/schedule.cpp
    ${BC_DIR}/seekpt.cpp
    ${BC_DIR}/source.cpp
    ${BC_DIR}/strmctl.cpp
    ${BC_DIR}/sysclock.cpp
    ${BC_DIR}/transfrm.cpp
    ${BC_DIR}/transip.cpp
    ${BC_DIR}/videoctl.cpp
    ${BC_DIR}/vtrans.cpp
    ${BC_DIR}/winctrl.cpp
    ${BC_DIR}/winutil.cpp
    ${BC_DIR}/wxdebug.cpp
    ${BC_DIR}/wxlist.cpp
    ${BC_DIR}/wxutil.cpp
)

# PUBLIC so anyone linking strmbase picks up `<streams.h>` etc.
target_include_directories(strmbase PUBLIC ${BC_DIR})

# Vendored code triggers an avalanche of -W warnings (unused
# parameters, sign comparisons, deprecated declarations). Silence
# them so they don't drown out warnings from our own code.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(strmbase PRIVATE -w)
endif()

# STRMBASE_BUILD gates the `#undef __in / __out / ...` block at the
# bottom of streams.h's MinGW shim. Without it, every .cpp file
# inside this target loses the SAL annotations as soon as streams.h
# is done including its umbrella headers. The upstream CMake sets
# this same define for the same reason.
target_compile_definitions(strmbase PRIVATE STRMBASE_BUILD)

# strmiids / winmm: the canonical link order for BaseClasses
# consumers. INTERFACE so they propagate to anyone linking strmbase
# (i.e. RetroCaptureVCam.dll).
target_link_libraries(strmbase INTERFACE strmiids winmm)
