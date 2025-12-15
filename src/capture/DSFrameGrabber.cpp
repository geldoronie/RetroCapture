#include "DSFrameGrabber.h"
#include "DSPin.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <initguid.h>

// Helper function declaration
void FreeMediaType(AM_MEDIA_TYPE &mt);

// Definir CLSID para nosso filtro customizado
// {12345678-1234-1234-1234-123456789ABC}
DEFINE_GUID(CLSID_DSFrameGrabber,
    0x12345678, 0x1234, 0x1234, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0x00, 0x00);

#ifdef _WIN32

DSFrameGrabber::DSFrameGrabber()
    : m_refCount(1), m_state(State_Stopped), m_graph(nullptr), m_inputPin(nullptr),
      m_width(0), m_height(0), m_hasFrame(false)
{
    ZeroMemory(&m_pixelFormat, sizeof(GUID));
    m_inputPin = new DSPin(this, L"In");
    if (m_inputPin)
    {
        m_inputPin->AddRef();
    }
}

DSFrameGrabber::~DSFrameGrabber()
{
    if (m_inputPin)
    {
        m_inputPin->Release();
        m_inputPin = nullptr;
    }
}

// IUnknown
STDMETHODIMP DSFrameGrabber::QueryInterface(REFIID riid, void **ppv)
{
    if (riid == IID_IUnknown || riid == IID_IBaseFilter)
    {
        *ppv = static_cast<IBaseFilter*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DSFrameGrabber::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) DSFrameGrabber::Release()
{
    ULONG refCount = InterlockedDecrement(&m_refCount);
    if (refCount == 0)
    {
        delete this;
    }
    return refCount;
}

// IBaseFilter
STDMETHODIMP DSFrameGrabber::GetClassID(CLSID *pClsID)
{
    if (!pClsID)
        return E_POINTER;
    *pClsID = CLSID_DSFrameGrabber;
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::Stop()
{
    m_state = State_Stopped;
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::Pause()
{
    m_state = State_Paused;
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::Run(REFERENCE_TIME tStart)
{
    m_state = State_Running;
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::GetState(DWORD dwMilliSecsTimeout, FILTER_STATE *State)
{
    if (!State)
        return E_POINTER;
    *State = m_state;
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::SetSyncSource(IReferenceClock *pClock)
{
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::GetSyncSource(IReferenceClock **ppClock)
{
    if (ppClock)
        *ppClock = nullptr;
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::EnumPins(IEnumPins **ppEnum)
{
    if (!ppEnum)
        return E_POINTER;
    
    // Criar um enumerador simples que retorna apenas o pin de entrada
    // Usar uma classe helper inline para implementar IEnumPins
    class SimpleEnumPins : public IEnumPins
    {
    public:
        SimpleEnumPins(IPin *pPin) : m_refCount(1), m_pPin(pPin), m_index(0)
        {
            if (m_pPin)
                m_pPin->AddRef();
        }
        
        virtual ~SimpleEnumPins()
        {
            if (m_pPin)
                m_pPin->Release();
        }
        
        STDMETHOD(QueryInterface)(REFIID riid, void **ppv)
        {
            if (riid == IID_IUnknown || riid == IID_IEnumPins)
            {
                *ppv = static_cast<IEnumPins*>(this);
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
        
        STDMETHOD(Next)(ULONG cPins, IPin **ppPins, ULONG *pcFetched)
        {
            if (!ppPins)
                return E_POINTER;
            
            ULONG fetched = 0;
            
            if (m_index == 0 && m_pPin && cPins > 0)
            {
                *ppPins = m_pPin;
                m_pPin->AddRef();
                fetched = 1;
                m_index = 1;
            }
            
            if (pcFetched)
                *pcFetched = fetched;
            
            return (fetched == cPins) ? S_OK : S_FALSE;
        }
        
        STDMETHOD(Skip)(ULONG cPins)
        {
            m_index += cPins;
            return (m_index <= 1) ? S_OK : S_FALSE;
        }
        
        STDMETHOD(Reset)()
        {
            m_index = 0;
            return S_OK;
        }
        
        STDMETHOD(Clone)(IEnumPins **ppEnum)
        {
            if (!ppEnum)
                return E_POINTER;
            
            SimpleEnumPins *pNew = new SimpleEnumPins(m_pPin);
            if (!pNew)
                return E_OUTOFMEMORY;
            
            pNew->m_index = m_index;
            *ppEnum = pNew;
            return S_OK;
        }
        
    private:
        ULONG m_refCount;
        IPin *m_pPin;
        ULONG m_index;
    };
    
    SimpleEnumPins *pEnum = new SimpleEnumPins(m_inputPin);
    if (!pEnum)
        return E_OUTOFMEMORY;
    
    *ppEnum = pEnum;
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::FindPin(LPCWSTR Id, IPin **ppPin)
{
    if (!ppPin)
        return E_POINTER;
    
    // Aceitar "In" ou nullptr (para encontrar qualquer pin de entrada)
    if ((!Id || wcscmp(Id, L"In") == 0) && m_inputPin)
    {
        *ppPin = m_inputPin;
        m_inputPin->AddRef();
        return S_OK;
    }
    
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP DSFrameGrabber::QueryFilterInfo(FILTER_INFO *pInfo)
{
    if (!pInfo)
        return E_POINTER;
    
    wcscpy_s(pInfo->achName, L"Frame Grabber");
    if (pInfo->pGraph)
        pInfo->pGraph->AddRef();
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::JoinFilterGraph(IFilterGraph *pGraph, LPCWSTR pName)
{
    m_graph = pGraph;
    return S_OK;
}

STDMETHODIMP DSFrameGrabber::QueryVendorInfo(LPWSTR *pVendorInfo)
{
    return E_NOTIMPL;
}

bool DSFrameGrabber::GetLatestFrame(uint8_t *buffer, size_t bufferSize, uint32_t &width, uint32_t &height)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    if (!m_hasFrame)
    {
        return false;
    }
    
    width = m_width;
    height = m_height;
    
    if (!buffer)
    {
        return true; // Apenas verificar se há frame
    }
    
    // Se for YUY2, retornar RGB convertido
    if (IsEqualGUID(m_pixelFormat, MEDIASUBTYPE_YUY2) && !m_rgbBuffer.empty())
    {
        size_t rgbSize = width * height * 3;
        if (bufferSize >= rgbSize)
        {
            memcpy(buffer, m_rgbBuffer.data(), rgbSize);
            return true;
        }
        return false;
    }
    
    // Para outros formatos, retornar dados originais
    if (bufferSize >= m_frameBuffer.size())
    {
        memcpy(buffer, m_frameBuffer.data(), m_frameBuffer.size());
        return true;
    }
    
    return false;
}

void DSFrameGrabber::ProcessSample(IMediaSample *pSample)
{
    static int processCount = 0;
    processCount++;
    
    if (!pSample)
        return;
    
    BYTE *pData = nullptr;
    long dataLength = 0;
    
    HRESULT hr = pSample->GetPointer(&pData);
    if (FAILED(hr) || !pData)
        return;
    
    dataLength = pSample->GetActualDataLength();
    if (dataLength <= 0)
        return;
    
    // Obter informações do formato do sample (se disponível)
    AM_MEDIA_TYPE *pmt = nullptr;
    GUID subtype = GUID_NULL;
    uint32_t width = 0, height = 0;
    
    if (SUCCEEDED(pSample->GetMediaType(&pmt)) && pmt)
    {
        subtype = pmt->subtype;
        UpdateDimensionsFromMediaType(pmt);
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            width = m_width;
            height = m_height;
        }
        FreeMediaType(*pmt);
        CoTaskMemFree(pmt);
    }
    else if (m_inputPin)
    {
        // Se não houver media type no sample, tentar obter do pin conectado
        AM_MEDIA_TYPE mt;
        ZeroMemory(&mt, sizeof(AM_MEDIA_TYPE));
        if (SUCCEEDED(m_inputPin->ConnectionMediaType(&mt)))
        {
            subtype = mt.subtype;
            UpdateDimensionsFromMediaType(&mt);
            {
                std::lock_guard<std::mutex> lock(m_bufferMutex);
                width = m_width;
                height = m_height;
            }
            FreeMediaType(mt);
        }
    }
    
    // Copiar dados para buffer e converter se necessário
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        
        // Armazenar formato de pixel
        m_pixelFormat = subtype;
        
        // Se for YUY2, converter para RGB
        if (IsEqualGUID(subtype, MEDIASUBTYPE_YUY2) && width > 0 && height > 0)
        {
            size_t rgbSize = width * height * 3;
            if (m_rgbBuffer.size() < rgbSize)
            {
                m_rgbBuffer.resize(rgbSize);
            }
            
            // Armazenar dados YUY2 originais
            if (m_frameBuffer.size() < static_cast<size_t>(dataLength))
            {
                m_frameBuffer.resize(dataLength);
            }
            memcpy(m_frameBuffer.data(), pData, dataLength);
            
            // Converter YUY2 para RGB
            ConvertYUY2ToRGB(pData, dataLength, m_rgbBuffer.data(), width, height);
            m_hasFrame = true;
        }
        else if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB24) || IsEqualGUID(subtype, MEDIASUBTYPE_RGB32))
        {
            // RGB - copiar diretamente
            if (m_frameBuffer.size() < static_cast<size_t>(dataLength))
            {
                m_frameBuffer.resize(dataLength);
            }
            memcpy(m_frameBuffer.data(), pData, dataLength);
            m_hasFrame = true;
        }
        else
        {
            // Outro formato - copiar como está (pode não funcionar, mas vamos tentar)
            if (m_frameBuffer.size() < static_cast<size_t>(dataLength))
            {
                m_frameBuffer.resize(dataLength);
            }
            memcpy(m_frameBuffer.data(), pData, dataLength);
            m_hasFrame = true;
            
            if (processCount <= 10)
            {
                LOG_WARN("[DSFrameGrabber] Formato de pixel desconhecido: " + std::to_string(subtype.Data1) + 
                         " - copiando dados brutos (pode não funcionar)");
            }
        }
    }
}

void DSFrameGrabber::UpdateDimensionsFromMediaType(const AM_MEDIA_TYPE *pmt)
{
    if (!pmt || pmt->formattype != FORMAT_VideoInfo || pmt->cbFormat < sizeof(VIDEOINFOHEADER))
        return;
    
    VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER*)pmt->pbFormat;
    if (pvi)
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_width = pvi->bmiHeader.biWidth;
        m_height = abs(pvi->bmiHeader.biHeight);
        m_pixelFormat = pmt->subtype;
    }
}

void DSFrameGrabber::ConvertYUY2ToRGB(const uint8_t *yuy2Data, size_t yuy2Size, uint8_t *rgbData, uint32_t width, uint32_t height)
{
    if (!yuy2Data || !rgbData || width == 0 || height == 0)
        return;
    
    // Conversão YUY2 para RGB
    // YUY2: Y0 U0 Y1 V0 Y2 U2 Y3 V2 ... (2 pixels por 4 bytes)
    // RGB: R0 G0 B0 R1 G1 B1 ... (1 pixel por 3 bytes)
    
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x += 2)
        {
            size_t yuy2Idx = (y * width + x) * 2;
            if (yuy2Idx + 3 >= yuy2Size)
                break;
            
            int Y0 = yuy2Data[yuy2Idx];
            int U = yuy2Data[yuy2Idx + 1];
            int Y1 = yuy2Data[yuy2Idx + 2];
            int V = yuy2Data[yuy2Idx + 3];
            
            // Converter YUV para RGB (fórmula ITU-R BT.601)
            auto yuvToRgb = [](int Y, int U, int V, int &R, int &G, int &B) {
                int C = Y - 16;
                int D = U - 128;
                int E = V - 128;
                
                R = (298 * C + 409 * E + 128) >> 8;
                G = (298 * C - 100 * D - 208 * E + 128) >> 8;
                B = (298 * C + 516 * D + 128) >> 8;
                
                R = std::max(0, std::min(255, R));
                G = std::max(0, std::min(255, G));
                B = std::max(0, std::min(255, B));
            };
            
            // Pixel 0
            int R0, G0, B0;
            yuvToRgb(Y0, U, V, R0, G0, B0);
            size_t rgbIdx0 = (y * width + x) * 3;
            rgbData[rgbIdx0] = static_cast<uint8_t>(R0);
            rgbData[rgbIdx0 + 1] = static_cast<uint8_t>(G0);
            rgbData[rgbIdx0 + 2] = static_cast<uint8_t>(B0);
            
            // Pixel 1 (se existir)
            if (x + 1 < width)
            {
                int R1, G1, B1;
                yuvToRgb(Y1, U, V, R1, G1, B1);
                size_t rgbIdx1 = (y * width + x + 1) * 3;
                rgbData[rgbIdx1] = static_cast<uint8_t>(R1);
                rgbData[rgbIdx1 + 1] = static_cast<uint8_t>(G1);
                rgbData[rgbIdx1 + 2] = static_cast<uint8_t>(B1);
            }
        }
    }
}

// Helper function
void FreeMediaType(AM_MEDIA_TYPE &mt)
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

#endif // _WIN32

