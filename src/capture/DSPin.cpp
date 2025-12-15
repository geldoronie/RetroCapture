#include "DSPin.h"
#include "DSFrameGrabber.h"
#include "../utils/Logger.h"
#include <cstring>
#include <vector>

#ifdef _WIN32

DSPin::DSPin(DSFrameGrabber *pFilter, const wchar_t *pName)
    : m_refCount(1), m_pFilter(pFilter), m_pConnectedPin(nullptr), m_pAllocator(nullptr),
      m_bReadOnly(FALSE)
{
    ZeroMemory(&m_mediaType, sizeof(AM_MEDIA_TYPE));
    
    if (pName)
    {
        size_t len = wcslen(pName) + 1;
        m_pName = new wchar_t[len];
        wcscpy_s(m_pName, len, pName);
    }
    else
    {
        m_pName = new wchar_t[3];
        wcscpy_s(m_pName, 3, L"In");
    }
    
    if (m_pFilter)
    {
        m_pFilter->AddRef();
    }
}

DSPin::~DSPin()
{
    FreeMediaType(m_mediaType);
    
    if (m_pConnectedPin)
    {
        m_pConnectedPin->Release();
        m_pConnectedPin = nullptr;
    }
    
    if (m_pAllocator)
    {
        m_pAllocator->Release();
        m_pAllocator = nullptr;
    }
    
    if (m_pName)
    {
        delete[] m_pName;
        m_pName = nullptr;
    }
    
    if (m_pFilter)
    {
        m_pFilter->Release();
        m_pFilter = nullptr;
    }
}

// IUnknown
STDMETHODIMP DSPin::QueryInterface(REFIID riid, void **ppv)
{
    if (riid == IID_IUnknown || riid == IID_IPin)
    {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IMemInputPin)
    {
        *ppv = static_cast<IMemInputPin*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DSPin::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) DSPin::Release()
{
    ULONG refCount = InterlockedDecrement(&m_refCount);
    if (refCount == 0)
    {
        delete this;
    }
    return refCount;
}

// IPin
STDMETHODIMP DSPin::Connect(IPin *pReceivePin, const AM_MEDIA_TYPE *pmt)
{
    // Para um pin de entrada, Connect não deve ser chamado diretamente
    // O DirectShow deve chamar ReceiveConnection no pin de entrada
    // Mas alguns graph builders podem chamar Connect primeiro
    
    if (!pReceivePin)
        return E_POINTER;
    
    // Se um tipo de mídia foi especificado, usar ele; caso contrário, tentar negociar
    if (pmt)
    {
        return ReceiveConnection(pReceivePin, pmt);
    }
    else
    {
        // Tentar negociar tipo de mídia
        // Obter tipo de mídia do pin de saída
        IEnumMediaTypes *pEnumMediaTypes = nullptr;
        HRESULT hr = pReceivePin->EnumMediaTypes(&pEnumMediaTypes);
        if (SUCCEEDED(hr) && pEnumMediaTypes)
        {
            AM_MEDIA_TYPE *pmtNegotiate = nullptr;
            ULONG fetched = 0;
            
            if (pEnumMediaTypes->Next(1, &pmtNegotiate, &fetched) == S_OK && fetched > 0)
            {
                hr = ReceiveConnection(pReceivePin, pmtNegotiate);
                FreeMediaType(*pmtNegotiate);
                CoTaskMemFree(pmtNegotiate);
            }
            else
            {
                hr = VFW_E_NO_ACCEPTABLE_TYPES;
            }
            if (pEnumMediaTypes)
                pEnumMediaTypes->Release();
            return hr;
        }
        return VFW_E_NO_ACCEPTABLE_TYPES;
    }
}

STDMETHODIMP DSPin::ReceiveConnection(IPin *pConnector, const AM_MEDIA_TYPE *pmt)
{
    if (!pConnector || !pmt)
        return E_POINTER;
    
    if (m_pConnectedPin)
    {
        LOG_WARN("[DSPin] ReceiveConnection: já conectado");
        return VFW_E_ALREADY_CONNECTED;
    }
    
    // Verificar se aceitamos este tipo de mídia
    if (pmt->majortype != MEDIATYPE_Video)
    {
        LOG_WARN("[DSPin] ReceiveConnection: tipo de mídia não aceito");
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    
    // Aceitar qualquer subtipo de vídeo
    // Copiar media type
    FreeMediaType(m_mediaType);
    HRESULT hr = CopyMediaType(&m_mediaType, pmt);
    if (FAILED(hr))
    {
        LOG_ERROR("[DSPin] ReceiveConnection: falha ao copiar media type: " + std::to_string(hr));
        return hr;
    }
    
    m_pConnectedPin = pConnector;
    m_pConnectedPin->AddRef();
    
    LOG_INFO("[DSPin] ReceiveConnection: conexão aceita com sucesso");
    return S_OK;
}

STDMETHODIMP DSPin::Disconnect()
{
    if (!m_pConnectedPin)
        return S_FALSE;
    
    if (m_pConnectedPin)
    {
        m_pConnectedPin->Release();
        m_pConnectedPin = nullptr;
    }
    FreeMediaType(m_mediaType);
    ZeroMemory(&m_mediaType, sizeof(AM_MEDIA_TYPE));
    
    return S_OK;
}

STDMETHODIMP DSPin::ConnectedTo(IPin **pPin)
{
    if (!pPin)
        return E_POINTER;
    
    if (!m_pConnectedPin)
    {
        *pPin = nullptr;
        return VFW_E_NOT_CONNECTED;
    }
    
    *pPin = m_pConnectedPin;
    m_pConnectedPin->AddRef();
    return S_OK;
}

STDMETHODIMP DSPin::ConnectionMediaType(AM_MEDIA_TYPE *pmt)
{
    if (!pmt)
        return E_POINTER;
    
    if (!m_pConnectedPin)
        return VFW_E_NOT_CONNECTED;
    
    return CopyMediaType(pmt, &m_mediaType);
}

STDMETHODIMP DSPin::QueryPinInfo(PIN_INFO *pInfo)
{
    if (!pInfo)
        return E_POINTER;
    
    pInfo->pFilter = m_pFilter;
    if (m_pFilter)
        m_pFilter->AddRef();
    
    pInfo->dir = PINDIR_INPUT;
    
    if (m_pName)
    {
        wcscpy_s(pInfo->achName, m_pName);
    }
    else
    {
        wcscpy_s(pInfo->achName, L"In");
    }
    
    return S_OK;
}

STDMETHODIMP DSPin::QueryDirection(PIN_DIRECTION *pPinDir)
{
    if (!pPinDir)
        return E_POINTER;
    *pPinDir = PINDIR_INPUT;
    return S_OK;
}

STDMETHODIMP DSPin::QueryId(LPWSTR *lpId)
{
    if (!lpId)
        return E_POINTER;
    
    size_t len = (m_pName ? wcslen(m_pName) : 2) + 1;
    *lpId = (LPWSTR)CoTaskMemAlloc(len * sizeof(wchar_t));
    if (!*lpId)
        return E_OUTOFMEMORY;
    
    wcscpy_s(*lpId, len, m_pName ? m_pName : L"In");
    return S_OK;
}

STDMETHODIMP DSPin::QueryAccept(const AM_MEDIA_TYPE *pmt)
{
    if (!pmt)
        return E_POINTER;
    
    // Aceitar apenas vídeo (qualquer subtipo)
    if (pmt->majortype != MEDIATYPE_Video)
    {
        return S_FALSE;
    }
    
    return S_OK;
}

STDMETHODIMP DSPin::EnumMediaTypes(IEnumMediaTypes **ppEnum)
{
    if (!ppEnum)
        return E_POINTER;
    
    // Criar um enumerador simples que retorna tipos de mídia de vídeo genéricos
    class SimpleEnumMediaTypes : public IEnumMediaTypes
    {
    public:
        SimpleEnumMediaTypes() : m_refCount(1), m_index(0)
        {
            // Criar lista de tipos de mídia aceitos
            // Aceitar MEDIATYPE_Video com subtipos comuns de vídeo
            // O DirectShow vai negociar o tipo específico durante a conexão
            m_mediaTypes.push_back({MEDIATYPE_Video, MEDIASUBTYPE_RGB24});
            m_mediaTypes.push_back({MEDIATYPE_Video, MEDIASUBTYPE_RGB32});
            m_mediaTypes.push_back({MEDIATYPE_Video, MEDIASUBTYPE_YUY2});
            m_mediaTypes.push_back({MEDIATYPE_Video, MEDIASUBTYPE_UYVY});
            m_mediaTypes.push_back({MEDIATYPE_Video, MEDIASUBTYPE_NV12});
            m_mediaTypes.push_back({MEDIATYPE_Video, MEDIASUBTYPE_NULL}); // Aceitar qualquer subtipo
        }
        
        virtual ~SimpleEnumMediaTypes() {}
        
        STDMETHOD(QueryInterface)(REFIID riid, void **ppv)
        {
            if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes)
            {
                *ppv = static_cast<IEnumMediaTypes*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        
        STDMETHOD_(ULONG, AddRef)()
        {
            return InterlockedIncrement(&m_refCount);
        }
        
        STDMETHOD_(ULONG, Release)()
        {
            ULONG refCount = InterlockedDecrement(&m_refCount);
            if (refCount == 0)
            {
                delete this;
            }
            return refCount;
        }
        
        STDMETHOD(Next)(ULONG cMediaTypes, AM_MEDIA_TYPE **ppMediaTypes, ULONG *pcFetched)
        {
            if (!ppMediaTypes)
                return E_POINTER;
            
            ULONG fetched = 0;
            
            if (m_index < m_mediaTypes.size() && cMediaTypes > 0)
            {
                AM_MEDIA_TYPE *pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
                if (!pmt)
                    return E_OUTOFMEMORY;
                
                ZeroMemory(pmt, sizeof(AM_MEDIA_TYPE));
                pmt->majortype = m_mediaTypes[m_index].majortype;
                pmt->subtype = m_mediaTypes[m_index].subtype;
                pmt->bFixedSizeSamples = TRUE;
                pmt->bTemporalCompression = FALSE;
                pmt->lSampleSize = 0;
                pmt->formattype = GUID_NULL;
                pmt->pUnk = nullptr;
                pmt->cbFormat = 0;
                pmt->pbFormat = nullptr;
                
                ppMediaTypes[0] = pmt;
                fetched = 1;
                m_index++;
            }
            
            if (pcFetched)
                *pcFetched = fetched;
            
            return (fetched == cMediaTypes) ? S_OK : S_FALSE;
        }
        
        STDMETHOD(Skip)(ULONG cMediaTypes)
        {
            m_index += cMediaTypes;
            return (m_index <= m_mediaTypes.size()) ? S_OK : S_FALSE;
        }
        
        STDMETHOD(Reset)()
        {
            m_index = 0;
            return S_OK;
        }
        
        STDMETHOD(Clone)(IEnumMediaTypes **ppEnum)
        {
            if (!ppEnum)
                return E_POINTER;
            
            SimpleEnumMediaTypes *pNew = new SimpleEnumMediaTypes();
            if (!pNew)
                return E_OUTOFMEMORY;
            
            pNew->m_index = m_index;
            *ppEnum = pNew;
            return S_OK;
        }
        
    private:
        ULONG m_refCount;
        ULONG m_index;
        struct MediaTypeInfo
        {
            GUID majortype;
            GUID subtype;
        };
        std::vector<MediaTypeInfo> m_mediaTypes;
    };
    
    SimpleEnumMediaTypes *pEnum = new SimpleEnumMediaTypes();
    if (!pEnum)
        return E_OUTOFMEMORY;
    
    *ppEnum = pEnum;
    return S_OK;
}

STDMETHODIMP DSPin::QueryInternalConnections(IPin **apPin, ULONG *nPin)
{
    return E_NOTIMPL;
}

STDMETHODIMP DSPin::EndOfStream()
{
    return S_OK;
}

STDMETHODIMP DSPin::BeginFlush()
{
    return S_OK;
}

STDMETHODIMP DSPin::EndFlush()
{
    return S_OK;
}

STDMETHODIMP DSPin::NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate)
{
    return S_OK;
}

// IMemInputPin
STDMETHODIMP DSPin::GetAllocator(IMemAllocator **ppAllocator)
{
    if (!ppAllocator)
        return E_POINTER;
    
    if (m_pAllocator)
    {
        *ppAllocator = m_pAllocator;
        m_pAllocator->AddRef();
        return S_OK;
    }
    
    *ppAllocator = nullptr;
    return VFW_E_NO_ALLOCATOR;
}

STDMETHODIMP DSPin::NotifyAllocator(IMemAllocator *pAllocator, BOOL bReadOnly)
{
    if (m_pAllocator)
    {
        m_pAllocator->Release();
        m_pAllocator = nullptr;
    }
    
    if (pAllocator)
    {
        m_pAllocator = pAllocator;
        m_pAllocator->AddRef();
        m_bReadOnly = bReadOnly;
    }
    
    return S_OK;
}

STDMETHODIMP DSPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES *pProps)
{
    return E_NOTIMPL;
}

STDMETHODIMP DSPin::Receive(IMediaSample *pSample)
{
    if (!pSample)
        return E_POINTER;
    
    if (m_pFilter)
    {
        m_pFilter->ProcessSample(pSample);
    }
    
    return S_OK;
}

STDMETHODIMP DSPin::ReceiveMultiple(IMediaSample **pSamples, long nSamples, long *nSamplesProcessed)
{
    if (!pSamples || !nSamplesProcessed)
        return E_POINTER;
    
    *nSamplesProcessed = 0;
    
    for (long i = 0; i < nSamples; i++)
    {
        if (SUCCEEDED(Receive(pSamples[i])))
        {
            (*nSamplesProcessed)++;
        }
        else
        {
            break;
        }
    }
    
    return (*nSamplesProcessed > 0) ? S_OK : E_FAIL;
}

STDMETHODIMP DSPin::ReceiveCanBlock()
{
    return S_FALSE; // Não bloqueia
}

void DSPin::FreeMediaType(AM_MEDIA_TYPE &mt)
{
    if (mt.cbFormat != 0)
    {
        CoTaskMemFree((PVOID)mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk != nullptr)
    {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

// Helper function para copiar media type
HRESULT CopyMediaType(AM_MEDIA_TYPE *pmtTarget, const AM_MEDIA_TYPE *pmtSource)
{
    if (!pmtTarget || !pmtSource)
        return E_POINTER;
    
    *pmtTarget = *pmtSource;
    
    if (pmtSource->cbFormat != 0)
    {
        pmtTarget->pbFormat = (PBYTE)CoTaskMemAlloc(pmtSource->cbFormat);
        if (!pmtTarget->pbFormat)
        {
            pmtTarget->cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        memcpy(pmtTarget->pbFormat, pmtSource->pbFormat, pmtSource->cbFormat);
    }
    
    if (pmtSource->pUnk)
    {
        pmtTarget->pUnk = pmtSource->pUnk;
        pmtTarget->pUnk->AddRef();
    }
    
    return S_OK;
}

#endif // _WIN32

