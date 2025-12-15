#pragma once

#ifdef _WIN32

#include <windows.h>
#include <dshow.h>
#include <strmif.h>
#include <vector>
#include <mutex>

// Forward declarations
struct IMemInputPin;
struct IMediaSample;
struct IPin;
struct IEnumPins;
struct IMemAllocator;
struct IReferenceClock;
struct IFilterGraph;

/**
 * @brief Custom DirectShow filter to capture video frames without Sample Grabber
 * Implements IBaseFilter and provides a pin that implements IMemInputPin
 */
class DSFrameGrabber : public IBaseFilter
{
public:
    DSFrameGrabber();
    virtual ~DSFrameGrabber();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();

    // IBaseFilter
    STDMETHOD(GetClassID)(CLSID *pClsID);
    STDMETHOD(Stop)();
    STDMETHOD(Pause)();
    STDMETHOD(Run)(REFERENCE_TIME tStart);
    STDMETHOD(GetState)(DWORD dwMilliSecsTimeout, FILTER_STATE *State);
    STDMETHOD(SetSyncSource)(IReferenceClock *pClock);
    STDMETHOD(GetSyncSource)(IReferenceClock **ppClock);
    STDMETHOD(EnumPins)(IEnumPins **ppEnum);
    STDMETHOD(FindPin)(LPCWSTR Id, IPin **ppPin);
    STDMETHOD(QueryFilterInfo)(FILTER_INFO *pInfo);
    STDMETHOD(JoinFilterGraph)(IFilterGraph *pGraph, LPCWSTR pName);
    STDMETHOD(QueryVendorInfo)(LPWSTR *pVendorInfo);

    // Custom methods
    bool GetLatestFrame(uint8_t *buffer, size_t bufferSize, uint32_t &width, uint32_t &height);
    void ProcessSample(IMediaSample *pSample);

private:
    ULONG m_refCount;
    FILTER_STATE m_state;
    IFilterGraph *m_graph;
    IPin *m_inputPin;
    
    // Frame buffer
    std::vector<uint8_t> m_frameBuffer;
    std::vector<uint8_t> m_rgbBuffer; // Buffer para conversão RGB
    std::mutex m_bufferMutex;
    uint32_t m_width;
    uint32_t m_height;
    bool m_hasFrame;
    GUID m_pixelFormat; // Formato de pixel atual (subtype)
    
    // Helper
    void UpdateDimensionsFromMediaType(const AM_MEDIA_TYPE *pmt);
    void ConvertYUY2ToRGB(const uint8_t *yuy2Data, size_t yuy2Size, uint8_t *rgbData, uint32_t width, uint32_t height);
};

// Forward declaration for pin class
class DSPin;

#endif // _WIN32

