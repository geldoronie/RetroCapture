# RetroCaptureVCam.plugin — CoreMediaIO DAL bundle (#85 macOS path).
#
# Built only when the host CMakeLists.txt sets
# RETROCAPTURE_BUILD_VIRTCAM_DAL_PLUGIN=ON AND PLATFORM_MACOS. The
# top-level gates this in turn so a Linux/Windows configure pass
# never enters this directory.

set(DAL_SOURCES
    Plugin.mm
    PluginConstants.h
    SharedMemoryReader.cpp
    SharedMemoryReader.h
    Info.plist.in
)

# `MODULE` + `BUNDLE TRUE` produces a CFBundle layout
# (Contents/Info.plist + Contents/MacOS/<binary>). BUNDLE_EXTENSION
# overrides the default `bundle` so the output is `.plugin` —
# what /Library/CoreMediaIO/Plug-Ins/DAL/ expects to find.
add_library(retrocapture_dal_plugin MODULE ${DAL_SOURCES})

set_target_properties(retrocapture_dal_plugin PROPERTIES
    BUNDLE                              TRUE
    BUNDLE_EXTENSION                    "plugin"
    OUTPUT_NAME                         "RetroCaptureVCam"
    MACOSX_BUNDLE_INFO_PLIST            "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER        "com.retrocapture.virtualcamera"
    MACOSX_BUNDLE_BUNDLE_NAME           "RetroCapture Virtual Camera"
    MACOSX_BUNDLE_BUNDLE_VERSION        "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING  "${PROJECT_VERSION}"
)

# Frameworks the plug-in calls into. CoreMediaIO is the API
# surface; CoreMedia + CoreVideo provide the CMSampleBuffer /
# CVPixelBuffer plumbing; CoreFoundation is the
# CFString/CFUUID/CFAllocator base.
target_link_libraries(retrocapture_dal_plugin PRIVATE
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreMediaIO"
    "-framework CoreVideo"
)

# So `#include "../output/VirtcamIpcLayout.h"` resolves.
target_include_directories(retrocapture_dal_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

# ARC isn't needed (we don't allocate Obj-C objects). Keep the
# default MRC behaviour explicit so a future maintainer doesn't
# enable ARC and break the CFRetain/CFRelease balance.
target_compile_options(retrocapture_dal_plugin PRIVATE -fno-objc-arc)

# Plug-in lives next to retrocapture in the build tree so
# build-macos-bundle.sh can pick it up by relative path.
set_target_properties(retrocapture_dal_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Camera Extension follow-up: when we add the System Extension
# variant for macOS 12.3+, it'll live in a sibling directory
# (src/camera_extension/) gated by its own option and ALSO
# built into the .app bundle's Contents/Library/SystemExtensions/.
# Keep this DAL target around either way — it's still the only
# path that works without an Apple Developer cert.
