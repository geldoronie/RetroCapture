#pragma once

#ifdef _WIN32

#include <windows.h>
#include <dshow.h>
#include <strmif.h>

// Forward declarations
struct IMemAllocator;
struct IMediaSample;
struct IReferenceClock;
struct IPin;
struct IEnumMediaTypes;
struct IBaseFilter;

// Forward declaration for DSFrameGrabber
class DSFrameGrabber;

/**
 * @brief Custom DirectShow input pin that receives video samples
 * Implements IPin and IMemInputPin to receive samples from capture filter
 */
class DSPin : public IPin, public IMemInputPin
{
public:
    DSPin(DSFrameGrabber *pFilter, const wchar_t *pName);
    virtual ~DSPin();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();

    // IPin
    STDMETHOD(Connect)(IPin *pReceivePin, const AM_MEDIA_TYPE *pmt);
    STDMETHOD(ReceiveConnection)(IPin *pConnector, const AM_MEDIA_TYPE *pmt);
    STDMETHOD(Disconnect)();
    STDMETHOD(ConnectedTo)(IPin **pPin);
    STDMETHOD(ConnectionMediaType)(AM_MEDIA_TYPE *pmt);
    STDMETHOD(QueryPinInfo)(PIN_INFO *pInfo);
    STDMETHOD(QueryDirection)(PIN_DIRECTION *pPinDir);
    STDMETHOD(QueryId)(LPWSTR *lpId);
    STDMETHOD(QueryAccept)(const AM_MEDIA_TYPE *pmt);
    STDMETHOD(EnumMediaTypes)(IEnumMediaTypes **ppEnum);
    STDMETHOD(QueryInternalConnections)(IPin **apPin, ULONG *nPin);
    STDMETHOD(EndOfStream)();
    STDMETHOD(BeginFlush)();
    STDMETHOD(EndFlush)();
    STDMETHOD(NewSegment)(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate);

    // IMemInputPin
    STDMETHOD(GetAllocator)(IMemAllocator **ppAllocator);
    STDMETHOD(NotifyAllocator)(IMemAllocator *pAllocator, BOOL bReadOnly);
    STDMETHOD(GetAllocatorRequirements)(ALLOCATOR_PROPERTIES *pProps);
    STDMETHOD(Receive)(IMediaSample *pSample);
    STDMETHOD(ReceiveMultiple)(IMediaSample **pSamples, long nSamples, long *nSamplesProcessed);
    STDMETHOD(ReceiveCanBlock)();

private:
    ULONG m_refCount;
    DSFrameGrabber *m_pFilter;
    IPin *m_pConnectedPin;
    AM_MEDIA_TYPE m_mediaType;
    IMemAllocator *m_pAllocator;
    wchar_t *m_pName;
    bool m_bReadOnly;
    
    void FreeMediaType(AM_MEDIA_TYPE &mt);
};

// Helper function declaration
HRESULT CopyMediaType(AM_MEDIA_TYPE *pmtTarget, const AM_MEDIA_TYPE *pmtSource);

#endif // _WIN32

