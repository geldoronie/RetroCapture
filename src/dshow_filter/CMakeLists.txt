# RetroCaptureVCam.dll — DirectShow source filter (#85 Phase 2).
#
# Built only when the host CMakeLists.txt sets
# RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER=ON. Links against the
# vendored strmbase from third_party/baseclasses/.

# Filter implementation source list — populated as the files land.
# Each addition tightens the dependency graph: SharedMemoryReader
# is standalone (Win32 only), then the CSource subclasses pull in
# strmbase, then DllEntry pulls everything together.
set(RCVCAM_SOURCES
    SharedMemoryReader.cpp
    CRetroCaptureStream.cpp
    CRetroCaptureFilter.cpp
    DllEntry.cpp
)

add_library(retrocapture_vcam MODULE
    ${RCVCAM_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/RetroCaptureVCam.def
)
target_link_libraries(retrocapture_vcam PRIVATE strmbase)
target_include_directories(retrocapture_vcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

# MinGW also needs the COM export libs the BaseClasses themselves
# don't pull in. amstrmid covers IBaseFilter / IMediaSample IIDs;
# uuid is the umbrella for everything else; ole32/oleaut32 for COM
# allocation; advapi32 for the registry helpers AMovieDllRegisterServer2
# uses.
target_link_libraries(retrocapture_vcam PRIVATE
    strmiids
    uuid
    ole32
    oleaut32
    advapi32
    winmm
)

# Strip the `lib` prefix MinGW adds to shared libs, produce
# RetroCaptureVCam.dll instead of libretrocapture_vcam.dll.
set_target_properties(retrocapture_vcam PROPERTIES
    PREFIX        ""
    OUTPUT_NAME   "RetroCaptureVCam"
    SUFFIX        ".dll"
)
