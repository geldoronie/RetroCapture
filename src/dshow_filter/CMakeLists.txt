# RetroCaptureVCam.dll — DirectShow source filter (#85 Phase 2).
#
# Built only when the host CMakeLists.txt sets
# RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER=ON. Links against the
# vendored strmbase from third_party/baseclasses/.

# Filter implementation source list — populated as the files land.
# Sentinel `_placeholder_` keeps this CMakeLists.txt syntactically
# valid (zero-source add_library is a CMake error) while the rest
# of the work proceeds. Remove once the real .cpp files exist.
set(RCVCAM_SOURCES
    # SharedMemoryReader.cpp
    # CRetroCaptureStream.cpp
    # CRetroCaptureFilter.cpp
    # CLSIDs.cpp
    # DllEntry.cpp
)

if(NOT RCVCAM_SOURCES)
    message(STATUS "src/dshow_filter: no filter sources yet — only "
                   "strmbase will be built. Add filter sources to "
                   "RCVCAM_SOURCES once SharedMemoryReader.cpp etc. exist.")
    return()
endif()

add_library(retrocapture_vcam MODULE ${RCVCAM_SOURCES})
set_target_properties(retrocapture_vcam PROPERTIES
    PREFIX        ""              # no `lib` prefix
    OUTPUT_NAME   "RetroCaptureVCam"
    SUFFIX        ".dll"
)
target_link_libraries(retrocapture_vcam PRIVATE strmbase)
