#include "VideoCaptureDS.h"
#include "DSFrameGrabber.h"
#include "DSPin.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cstring>
#include <iostream>

#ifdef _WIN32

// DirectShow headers - ordem é importante
#include <windows.h>
#include <comdef.h>
#include <objbase.h>
// propkey.h e propvarutil.h podem não estar disponíveis no MinGW
// Remover se não forem usados ou tornar opcional
#ifdef HAVE_PROPVARUTIL_H
#include <propkey.h>
#include <propvarutil.h>
#endif
// DirectShow headers - implementação principal
#include <dshow.h>
#include <strmif.h>
#include <comdef.h>
#include <objidl.h>
#include <oaidl.h>
#include <uuids.h>
#include <initguid.h> // Para garantir que os GUIDs estejam definidos

// Forward declaration for IClassFactory
struct IClassFactory;

// Para CLSID_SampleGrabber e ISampleGrabber
#include <qedit.h>

// CLSID_SampleGrabber pode não estar definido no MinGW/MXE, definir manualmente se necessário
#ifndef CLSID_SampleGrabber
// Definir o GUID manualmente (valor do CLSID_SampleGrabber)
static const GUID CLSID_SampleGrabber = 
    { 0xC1F400A0, 0xF5F0, 0x11d0, { 0xA3, 0xBA, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
#endif

// CLSID_NullRenderer pode não estar definido no MinGW/MXE, definir manualmente se necessário
#ifndef CLSID_NullRenderer
// Definir o GUID manualmente (valor do CLSID_NullRenderer)
static const GUID CLSID_NullRenderer = 
    { 0xC1F400A4, 0xF5F0, 0x11d0, { 0xA3, 0xBA, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
#endif

// Variáveis globais e funções do Media Foundation removidas - agora usamos apenas DirectShow

// Helper functions para DirectShow (DeleteMediaType e FreeMediaType)
// Essas funções não estão disponíveis no MinGW, então definimos aqui
void FreeMediaType(AM_MEDIA_TYPE& mt)
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

void DeleteMediaType(AM_MEDIA_TYPE *pmt)
{
    if (pmt != nullptr)
    {
        FreeMediaType(*pmt);
        CoTaskMemFree(pmt);
    }
}

// Helper macro for COM error checking
#define CHECK_HR(hr, msg)                                        \
    if (FAILED(hr))                                              \
    {                                                            \
        _com_error err(hr);                                      \
        LOG_ERROR(msg + std::string(": ") + err.ErrorMessage()); \
        return false;                                            \
    }

// Helper macro for COM error checking
#define CHECK_HR(hr, msg)                                        \
    if (FAILED(hr))                                              \
    {                                                            \
        _com_error err(hr);                                      \
        LOG_ERROR(msg + std::string(": ") + err.ErrorMessage()); \
        return false;                                            \
    }

// Helper to release COM objects
template <typename T>
void SafeRelease(T **ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}

VideoCaptureDS::VideoCaptureDS()
    : m_graphBuilder(nullptr), m_captureGraphBuilder(nullptr), m_captureFilter(nullptr),
      m_sampleGrabber(nullptr), m_mediaControl(nullptr), m_mediaEvent(nullptr),
      m_streamConfig(nullptr), m_videoProcAmp(nullptr), m_cameraControl(nullptr),
      m_frameBuffer(), m_bufferMutex(), m_latestFrame(), m_hasFrame(false),
      m_capturePin(nullptr), m_useAlternativeCapture(false), m_customGrabberFilter(nullptr),
      m_width(0), m_height(0), m_fps(30), m_pixelFormat(0),
      m_isOpen(false), m_streaming(false), m_dummyMode(false), m_deviceId(""),
      m_dummyFrameBuffer(), m_comInitialized(false)
{
    LOG_INFO("VideoCaptureDS: Iniciando construtor (DirectShow)...");
    // Inicializar COM para DirectShow
    if (!initializeCOM())
    {
        LOG_WARN("Falha ao inicializar COM - usando modo dummy");
        m_dummyMode = true;
    }
    LOG_INFO("VideoCaptureDS: Construtor concluído");
}

VideoCaptureDS::~VideoCaptureDS()
{
    close();
    shutdownCOM();
}

bool VideoCaptureDS::initializeCOM()
{
    // Inicializar COM para DirectShow
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        LOG_WARN("Falha ao inicializar COM: " + std::to_string(hr));
        return false;
    }
    
    m_comInitialized = true;
    LOG_INFO("COM inicializado com sucesso para DirectShow");
    return true;
}

void VideoCaptureDS::shutdownCOM()
{
    if (m_comInitialized)
    {
        CoUninitialize();
        m_comInitialized = false;
    }
}

bool VideoCaptureDS::open(const std::string &device)
{
    LOG_INFO("VideoCaptureDS::open() chamado com device: " + device);
    
    if (m_isOpen)
    {
        LOG_WARN("Dispositivo já aberto, fechando primeiro");
        close();
    }

    m_deviceId = device;

    // Em modo dummy, apenas marcar como aberto
    if (m_dummyMode)
    {
        m_isOpen = true;
        LOG_INFO("Modo dummy ativado para Windows");
        return true;
    }

    if (!createCaptureGraph(device))
    {
        LOG_ERROR("Falha ao criar graph de captura para dispositivo: " + device);
        return false;
    }

    if (!configureCaptureFormat())
    {
        LOG_ERROR("Falha ao configurar formato de captura");
        close();
        return false;
    }

    m_isOpen = true;
    LOG_INFO("Dispositivo aberto: " + device);
    return true;
}

void VideoCaptureDS::close()
{
    if (!m_isOpen)
    {
        return;
    }

    stopCapture();

    // Liberar objetos DirectShow
    SafeRelease(&m_cameraControl);
    SafeRelease(&m_videoProcAmp);
    SafeRelease(&m_streamConfig);
    SafeRelease(&m_mediaEvent);
    SafeRelease(&m_mediaControl);
    SafeRelease(&m_sampleGrabber);
    SafeRelease(&m_capturePin);
    if (m_customGrabberFilter)
    {
        m_customGrabberFilter->Release(); // Liberar referência COM
        m_customGrabberFilter = nullptr;
    }
    SafeRelease(&m_captureFilter);
    SafeRelease(&m_captureGraphBuilder);
    SafeRelease(&m_graphBuilder);
    
    m_useAlternativeCapture = false;

    m_isOpen = false;
    m_hasFrame = false;
    m_frameBuffer.clear();

    if (!m_dummyMode)
    {
        m_dummyFrameBuffer.clear();
    }

    LOG_INFO("Dispositivo fechado");
}

bool VideoCaptureDS::isOpen() const
{
    return m_isOpen || m_dummyMode;
}

bool VideoCaptureDS::createCaptureGraph(const std::string &deviceId)
{
    LOG_INFO("Criando graph de captura DirectShow para dispositivo: " + deviceId);
    
    HRESULT hr = S_OK;
    ICreateDevEnum *pDevEnum = nullptr;
    IEnumMoniker *pEnum = nullptr;
    IMoniker *pMoniker = nullptr;
    
    // Criar Filter Graph Manager
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, (void**)&m_graphBuilder);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao criar Filter Graph: " + std::to_string(hr));
        return false;
    }
    
    // Criar Capture Graph Builder
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICaptureGraphBuilder2, (void**)&m_captureGraphBuilder);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao criar Capture Graph Builder: " + std::to_string(hr));
        SafeRelease(&m_graphBuilder);
        return false;
    }
    
    // Associar Filter Graph ao Capture Graph Builder
    hr = m_captureGraphBuilder->SetFiltergraph(m_graphBuilder);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao associar Filter Graph: " + std::to_string(hr));
        SafeRelease(&m_captureGraphBuilder);
        SafeRelease(&m_graphBuilder);
        return false;
    }
    
    // Criar Device Enumerator
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao criar Device Enumerator: " + std::to_string(hr));
        SafeRelease(&m_captureGraphBuilder);
        SafeRelease(&m_graphBuilder);
        return false;
    }
    
    // Criar enumerador para categoria de captura de vídeo
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (FAILED(hr) || hr == S_FALSE || !pEnum)
    {
        LOG_ERROR("Falha ao criar enumerador de dispositivos de vídeo: " + std::to_string(hr));
        SafeRelease(&pDevEnum);
        SafeRelease(&m_captureGraphBuilder);
        SafeRelease(&m_graphBuilder);
        return false;
    }
    
    // Encontrar o dispositivo pelo ID (índice)
    UINT32 deviceIndex = 0;
    if (!deviceId.empty() && deviceId != "default")
    {
        try
        {
            deviceIndex = static_cast<UINT32>(std::stoul(deviceId));
        }
        catch (...)
        {
            LOG_WARN("ID de dispositivo inválido: " + deviceId + ", usando índice 0");
            deviceIndex = 0;
        }
    }
    
    // Avançar até o dispositivo desejado
    pEnum->Reset();
    for (UINT32 i = 0; i <= deviceIndex; i++)
    {
        SafeRelease(&pMoniker);
        if (pEnum->Next(1, &pMoniker, nullptr) != S_OK)
        {
            LOG_ERROR("Dispositivo não encontrado no índice: " + std::to_string(deviceIndex));
            SafeRelease(&pEnum);
            SafeRelease(&pDevEnum);
            SafeRelease(&m_captureGraphBuilder);
            SafeRelease(&m_graphBuilder);
            return false;
        }
    }
    
    // Criar filtro de captura a partir do moniker
    hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&m_captureFilter);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao criar filtro de captura: " + std::to_string(hr));
        SafeRelease(&pMoniker);
        SafeRelease(&pEnum);
        SafeRelease(&pDevEnum);
        SafeRelease(&m_captureGraphBuilder);
        SafeRelease(&m_graphBuilder);
        return false;
    }
    
    // Adicionar filtro ao graph
    hr = m_graphBuilder->AddFilter(m_captureFilter, L"Video Capture");
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao adicionar filtro ao graph: " + std::to_string(hr));
        SafeRelease(&m_captureFilter);
        SafeRelease(&pMoniker);
        SafeRelease(&pEnum);
        SafeRelease(&pDevEnum);
        SafeRelease(&m_captureGraphBuilder);
        SafeRelease(&m_graphBuilder);
        return false;
    }
    
    // Obter interfaces necessárias
    hr = m_graphBuilder->QueryInterface(IID_IMediaControl, (void**)&m_mediaControl);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao obter IMediaControl: " + std::to_string(hr));
    }
    
    hr = m_graphBuilder->QueryInterface(IID_IMediaEventEx, (void**)&m_mediaEvent);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao obter IMediaEventEx: " + std::to_string(hr));
    }
    
    // Obter IAMStreamConfig para configurar formato
    hr = m_captureGraphBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                              m_captureFilter, IID_IAMStreamConfig, (void**)&m_streamConfig);
    if (FAILED(hr))
    {
        LOG_WARN("Falha ao obter IAMStreamConfig: " + std::to_string(hr));
    }
    
    // Obter interfaces de controle de vídeo
    hr = m_captureFilter->QueryInterface(IID_IAMVideoProcAmp, (void**)&m_videoProcAmp);
    if (FAILED(hr))
    {
        LOG_WARN("Falha ao obter IAMVideoProcAmp: " + std::to_string(hr));
    }
    
    hr = m_captureFilter->QueryInterface(IID_IAMCameraControl, (void**)&m_cameraControl);
    if (FAILED(hr))
    {
        LOG_WARN("Falha ao obter IAMCameraControl: " + std::to_string(hr));
    }
    
    // Criar Sample Grabber para capturar frames
    // Tentar carregar dinamicamente de qedit.dll se CoCreateInstance falhar
    IBaseFilter *pSampleGrabberFilter = nullptr;
    hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IBaseFilter, (void**)&pSampleGrabberFilter);
    
    // Se falhar, tentar carregar de qedit.dll dinamicamente
    if (FAILED(hr))
    {
        HMODULE hQEdit = LoadLibraryA("qedit.dll");
        if (hQEdit)
        {
            typedef HRESULT (WINAPI *DllGetClassObjectProc)(REFCLSID rclsid, REFIID riid, LPVOID *ppv);
            DllGetClassObjectProc pDllGetClassObject = (DllGetClassObjectProc)GetProcAddress(hQEdit, "DllGetClassObject");
            if (pDllGetClassObject)
            {
                IClassFactory *pClassFactory = nullptr;
                hr = pDllGetClassObject(CLSID_SampleGrabber, IID_IClassFactory, (void**)&pClassFactory);
                if (SUCCEEDED(hr) && pClassFactory)
                {
                    hr = pClassFactory->CreateInstance(nullptr, IID_IBaseFilter, (void**)&pSampleGrabberFilter);
                    pClassFactory->Release();
                }
            }
            // Não liberar a DLL aqui - ela precisa permanecer carregada enquanto o Sample Grabber estiver em uso
        }
    }
    
    if (SUCCEEDED(hr) && pSampleGrabberFilter)
    {
        hr = m_graphBuilder->AddFilter(pSampleGrabberFilter, L"Sample Grabber");
        if (SUCCEEDED(hr))
        {
            hr = pSampleGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&m_sampleGrabber);
            if (SUCCEEDED(hr))
            {
                // Configurar Sample Grabber para capturar frames
                AM_MEDIA_TYPE mt;
                ZeroMemory(&mt, sizeof(AM_MEDIA_TYPE));
                mt.majortype = MEDIATYPE_Video;
                mt.subtype = MEDIASUBTYPE_RGB24;
                hr = m_sampleGrabber->SetMediaType(&mt);
                if (SUCCEEDED(hr))
                {
                    hr = m_sampleGrabber->SetBufferSamples(TRUE);
                    hr = m_sampleGrabber->SetOneShot(FALSE);
                    LOG_INFO("Sample Grabber configurado");
                }
            }
        }
    }
    else
    {
        LOG_WARN("Falha ao criar Sample Grabber: " + std::to_string(hr));
        LOG_WARN("Sample Grabber não está disponível - tentando usar Null Renderer diretamente");
    }
    
    // Se Sample Grabber estiver disponível, usar para captura
    // Caso contrário, conectar diretamente ao Null Renderer para evitar janela
    if (pSampleGrabberFilter && m_sampleGrabber)
    {
        // Obter interface IGraphBuilder para conectar pins
        IPin *pCapturePin = nullptr;
        IPin *pGrabberInputPin = nullptr;
        
        // Encontrar o pin de captura do filtro de captura
        hr = m_captureGraphBuilder->FindPin(m_captureFilter, PINDIR_OUTPUT, 
                                            &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, 
                                            FALSE, 0, &pCapturePin);
        if (SUCCEEDED(hr) && pCapturePin)
        {
            // Encontrar o pin de entrada do Sample Grabber
            IEnumPins *pEnumPins = nullptr;
            hr = pSampleGrabberFilter->EnumPins(&pEnumPins);
            if (SUCCEEDED(hr) && pEnumPins)
            {
                IPin *pPin = nullptr;
                ULONG fetched = 0;
                PIN_DIRECTION dir;
                
                // Procurar pelo pin de entrada
                while (pEnumPins->Next(1, &pPin, &fetched) == S_OK && fetched > 0)
                {
                    hr = pPin->QueryDirection(&dir);
                    if (SUCCEEDED(hr) && dir == PINDIR_INPUT)
                    {
                        pGrabberInputPin = pPin;
                        break;
                    }
                    SafeRelease(&pPin);
                }
                SafeRelease(&pEnumPins);
            }
            
            // Conectar os pins diretamente
            if (pCapturePin && pGrabberInputPin)
            {
                // Tentar conectar diretamente sem negociar formato (mais simples)
                hr = m_graphBuilder->ConnectDirect(pCapturePin, pGrabberInputPin, nullptr);
                if (FAILED(hr))
                {
                    // Se ConnectDirect falhar, tentar Connect (que negocia formato)
                    hr = m_graphBuilder->Connect(pCapturePin, pGrabberInputPin);
                    if (FAILED(hr))
                    {
                        LOG_ERROR("Falha ao conectar pins: " + std::to_string(hr));
                        SafeRelease(&pGrabberInputPin);
                        SafeRelease(&pCapturePin);
                        SafeRelease(&pSampleGrabberFilter);
                        SafeRelease(&pMoniker);
                        SafeRelease(&pEnum);
                        SafeRelease(&pDevEnum);
                        return false;
                    }
                }
                
                // Verificar se há pin de saída no Sample Grabber e desconectá-lo se necessário
                // Com SetBufferSamples(TRUE), não precisamos conectar o pin de saída
                IEnumPins *pEnumPinsOut = nullptr;
                hr = pSampleGrabberFilter->EnumPins(&pEnumPinsOut);
                if (SUCCEEDED(hr) && pEnumPinsOut)
                {
                    IPin *pPinOut = nullptr;
                    ULONG fetched = 0;
                    PIN_DIRECTION dir;
                    
                    while (pEnumPinsOut->Next(1, &pPinOut, &fetched) == S_OK && fetched > 0)
                    {
                        hr = pPinOut->QueryDirection(&dir);
                        if (SUCCEEDED(hr) && dir == PINDIR_OUTPUT)
                        {
                            // Verificar se está conectado e desconectar se necessário
                            IPin *pConnected = nullptr;
                            hr = pPinOut->ConnectedTo(&pConnected);
                            if (SUCCEEDED(hr) && pConnected)
                            {
                                // Está conectado - desconectar para evitar renderização
                                hr = m_graphBuilder->Disconnect(pPinOut);
                                hr = m_graphBuilder->Disconnect(pConnected);
                                SafeRelease(&pConnected);
                                LOG_INFO("Pin de saída do Sample Grabber desconectado para evitar janela");
                            }
                            SafeRelease(&pConnected);
                        }
                        SafeRelease(&pPinOut);
                    }
                    SafeRelease(&pEnumPinsOut);
                }
                
                LOG_INFO("Pins conectados manualmente - sem janela de preview");
            }
            else
            {
                LOG_WARN("Falha ao encontrar pins para conexão manual");
                // Fallback: usar RenderStream (pode criar janela)
                hr = m_captureGraphBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                                         m_captureFilter, pSampleGrabberFilter, nullptr);
                if (FAILED(hr))
                {
                    LOG_ERROR("Falha ao renderizar stream de captura: " + std::to_string(hr));
                    SafeRelease(&pGrabberInputPin);
                    SafeRelease(&pCapturePin);
                    SafeRelease(&pSampleGrabberFilter);
                    SafeRelease(&pMoniker);
                    SafeRelease(&pEnum);
                    SafeRelease(&pDevEnum);
                    return false;
                }
                LOG_WARN("Usando RenderStream (pode criar janela de preview)");
            }
            
            SafeRelease(&pGrabberInputPin);
            SafeRelease(&pCapturePin);
        }
        else
        {
            LOG_WARN("Falha ao encontrar pin de captura: " + std::to_string(hr));
            // Fallback: usar RenderStream
            hr = m_captureGraphBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                                     m_captureFilter, pSampleGrabberFilter, nullptr);
            if (FAILED(hr))
            {
                LOG_ERROR("Falha ao renderizar stream de captura: " + std::to_string(hr));
                SafeRelease(&pSampleGrabberFilter);
                SafeRelease(&pMoniker);
                SafeRelease(&pEnum);
                SafeRelease(&pDevEnum);
                return false;
            }
        }
    }
    else
    {
        // Sample Grabber não disponível - conectar diretamente ao Null Renderer
        // Isso evita janela de preview, mas não permite captura de frames ainda
        LOG_WARN("Sample Grabber não disponível - conectando ao Null Renderer para evitar janela");
        
        // Criar Null Renderer
        IBaseFilter *pNullRenderer = nullptr;
        hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IBaseFilter, (void**)&pNullRenderer);
        if (SUCCEEDED(hr))
        {
            hr = m_graphBuilder->AddFilter(pNullRenderer, L"Null Renderer");
            if (SUCCEEDED(hr))
            {
                // Encontrar pin de captura
                IPin *pCapturePin = nullptr;
                hr = m_captureGraphBuilder->FindPin(m_captureFilter, PINDIR_OUTPUT, 
                                                    &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, 
                                                    FALSE, 0, &pCapturePin);
                if (SUCCEEDED(hr) && pCapturePin)
                {
                    // Encontrar pin de entrada do Null Renderer
                    IEnumPins *pEnumPins = nullptr;
                    hr = pNullRenderer->EnumPins(&pEnumPins);
                    if (SUCCEEDED(hr) && pEnumPins)
                    {
                        IPin *pPin = nullptr;
                        ULONG fetched = 0;
                        PIN_DIRECTION dir;
                        
                        while (pEnumPins->Next(1, &pPin, &fetched) == S_OK && fetched > 0)
                        {
                            hr = pPin->QueryDirection(&dir);
                            if (SUCCEEDED(hr) && dir == PINDIR_INPUT)
                            {
                                // Conectar Capture -> Null Renderer
                                hr = m_graphBuilder->Connect(pCapturePin, pPin);
                                if (SUCCEEDED(hr))
                                {
                                    LOG_INFO("Conectado ao Null Renderer (sem Sample Grabber)");
                                }
                                else
                                {
                                    LOG_WARN("Falha ao conectar ao Null Renderer: " + std::to_string(hr));
                                }
                                SafeRelease(&pPin);
                                break;
                            }
                            SafeRelease(&pPin);
                        }
                        SafeRelease(&pEnumPins);
                    }
                    SafeRelease(&pCapturePin);
                }
            }
            SafeRelease(&pNullRenderer);
        }
        else
        {
            LOG_WARN("Falha ao criar Null Renderer: " + std::to_string(hr));
            // Ainda assim, permitir que o graph seja criado
            // O dispositivo pode ser configurado mesmo sem renderização
        }
        
        // Mesmo sem Sample Grabber, criar filtro customizado para capturar frames
        LOG_WARN("Sample Grabber não disponível - criando filtro customizado para captura");
        
        m_customGrabberFilter = new DSFrameGrabber();
        if (m_customGrabberFilter)
        {
            hr = m_graphBuilder->AddFilter(m_customGrabberFilter, L"Frame Grabber");
            if (SUCCEEDED(hr))
            {
                // Encontrar pin de captura
                IPin *pCapturePin = nullptr;
                hr = m_captureGraphBuilder->FindPin(m_captureFilter, PINDIR_OUTPUT, 
                                                    &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, 
                                                    FALSE, 0, &pCapturePin);
                if (SUCCEEDED(hr) && pCapturePin)
                {
                    // Tentar conexão manual primeiro (mais controle)
                    IPin *pGrabberInputPin = nullptr;
                    hr = m_customGrabberFilter->FindPin(L"In", &pGrabberInputPin);
                    if (SUCCEEDED(hr) && pGrabberInputPin)
                    {
                        
                        // Obter o tipo de mídia do pin de captura
                        IEnumMediaTypes *pEnumMediaTypes = nullptr;
                        hr = pCapturePin->EnumMediaTypes(&pEnumMediaTypes);
                        if (SUCCEEDED(hr) && pEnumMediaTypes)
                        {
                            AM_MEDIA_TYPE *pmt = nullptr;
                            ULONG fetched = 0;
                            
                            // Tentar o primeiro tipo de mídia disponível
                            if (pEnumMediaTypes->Next(1, &pmt, &fetched) == S_OK && fetched > 0)
                            {
                                // Verificar se o pin aceita este tipo de mídia
                                HRESULT acceptHr = pGrabberInputPin->QueryAccept(pmt);
                                
                                if (SUCCEEDED(acceptHr) && acceptHr == S_OK)
                                {
                                    // Tentar ConnectDirect primeiro (mais direto)
                                    hr = m_graphBuilder->ConnectDirect(pCapturePin, pGrabberInputPin, pmt);
                                    
                                    if (FAILED(hr))
                                    {
                                        // Se ConnectDirect falhar, tentar Connect (que negocia formato automaticamente)
                                        hr = m_graphBuilder->Connect(pCapturePin, pGrabberInputPin);
                                        
                                        if (FAILED(hr))
                                        {
                                            // Última tentativa: chamar Connect diretamente no pin de saída
                                            hr = pCapturePin->Connect(pGrabberInputPin, pmt);
                                        }
                                    }
                                    
                                    if (SUCCEEDED(hr))
                                    {
                                        m_useAlternativeCapture = true;
                                    }
                                    else
                                    {
                                        LOG_WARN("Todas as tentativas de conexão falharam: " + std::to_string(hr));
                                    }
                                }
                                else
                                {
                                    LOG_WARN("Pin não aceita tipo de mídia (QueryAccept retornou: " + std::to_string(acceptHr) + ")");
                                }
                                DeleteMediaType(pmt);
                            }
                            else
                            {
                                LOG_WARN("Falha ao obter primeiro tipo de mídia do pin de captura (hr: " + std::to_string(hr) + ")");
                            }
                            SafeRelease(&pEnumMediaTypes);
                        }
                        else
                        {
                            LOG_WARN("Falha ao enumerar tipos de mídia do pin de captura (hr: " + std::to_string(hr) + ") - tentando Connect direto");
                            // Se não conseguir enumerar tipos de mídia, tentar Connect normal
                            hr = m_graphBuilder->Connect(pCapturePin, pGrabberInputPin);
                            if (SUCCEEDED(hr))
                            {
                                m_useAlternativeCapture = true;
                            }
                            else
                            {
                                LOG_WARN("Falha ao conectar filtro customizado manualmente: " + std::to_string(hr));
                            }
                        }
                        SafeRelease(&pGrabberInputPin);
                    }
                    else
                    {
                        LOG_ERROR("Falha ao encontrar pin de entrada do filtro customizado (hr: " + std::to_string(hr) + ")");
                    }
                    SafeRelease(&pCapturePin);
                }
                else
                {
                    LOG_ERROR("Falha ao encontrar pin de captura: " + std::to_string(hr));
                }
            }
            else
            {
                LOG_ERROR("Falha ao adicionar filtro customizado ao graph: " + std::to_string(hr));
                if (m_customGrabberFilter)
                {
                    m_customGrabberFilter->Release(); // Liberar referência COM
                    m_customGrabberFilter = nullptr;
                }
            }
        }
        else
        {
            LOG_ERROR("Falha ao criar filtro customizado");
        }
        
        if (!m_useAlternativeCapture)
        {
            LOG_WARN("Graph criado sem Sample Grabber - captura de frames não estará disponível");
        }
    }
    
    // Liberar referência ao Sample Grabber Filter (mantemos ISampleGrabber)
    SafeRelease(&pSampleGrabberFilter);
    
    // Remover qualquer filtro de renderização que possa ter sido adicionado automaticamente
    // Isso evita que janelas de preview sejam criadas
    IEnumFilters *pEnumFilters = nullptr;
    hr = m_graphBuilder->EnumFilters(&pEnumFilters);
    if (SUCCEEDED(hr) && pEnumFilters)
    {
        IBaseFilter *pFilter = nullptr;
        ULONG fetched = 0;
        
        // Lista de CLSIDs de filtros de renderização que devem ser removidos
        const GUID* rendererCLSIDs[] = {
            &CLSID_VideoRenderer,
            &CLSID_VideoRendererDefault,
            &CLSID_VideoMixingRenderer,
            &CLSID_VideoMixingRenderer9,
            &CLSID_OverlayMixer
        };
        
        while (pEnumFilters->Next(1, &pFilter, &fetched) == S_OK && fetched > 0)
        {
            CLSID filterCLSID;
            hr = pFilter->GetClassID(&filterCLSID);
            if (SUCCEEDED(hr))
            {
                // Verificar se é um filtro de renderização
                for (size_t i = 0; i < sizeof(rendererCLSIDs) / sizeof(rendererCLSIDs[0]); i++)
                {
                    if (IsEqualGUID(filterCLSID, *rendererCLSIDs[i]))
                    {
                        LOG_INFO("Removendo filtro de renderização do graph para evitar janela de preview");
                        // Desconectar todos os pins antes de remover
                        IEnumPins *pEnumPins = nullptr;
                        hr = pFilter->EnumPins(&pEnumPins);
                        if (SUCCEEDED(hr) && pEnumPins)
                        {
                            IPin *pPin = nullptr;
                            ULONG pinFetched = 0;
                            while (pEnumPins->Next(1, &pPin, &pinFetched) == S_OK && pinFetched > 0)
                            {
                                m_graphBuilder->Disconnect(pPin);
                                SafeRelease(&pPin);
                            }
                            SafeRelease(&pEnumPins);
                        }
                        // Remover o filtro do graph
                        m_graphBuilder->RemoveFilter(pFilter);
                        break;
                    }
                }
            }
            SafeRelease(&pFilter);
        }
        SafeRelease(&pEnumFilters);
    }
    
    // Cleanup
    SafeRelease(&pMoniker);
    SafeRelease(&pEnum);
    SafeRelease(&pDevEnum);
    
    LOG_INFO("Graph de captura DirectShow criado com sucesso");
    return true;
}

bool VideoCaptureDS::configureCaptureFormat()
{
    if (!m_streamConfig)
    {
        LOG_WARN("IAMStreamConfig não disponível - usando formato padrão do dispositivo");
        return true; // Não é um erro fatal
    }
    
    LOG_INFO("Configurando formato de captura: " + std::to_string(m_width) + "x" + std::to_string(m_height) + " @ " + std::to_string(m_fps) + "fps");
    
    HRESULT hr = S_OK;
    AM_MEDIA_TYPE *pmt = nullptr;
    
    // Obter o formato atual
    hr = m_streamConfig->GetFormat(&pmt);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao obter formato atual: " + std::to_string(hr));
        return false;
    }
    
    // Verificar se é vídeo
    if (pmt->majortype != MEDIATYPE_Video)
    {
        LOG_ERROR("Formato não é vídeo");
        DeleteMediaType(pmt);
        return false;
    }
    
    // Obter VIDEOINFOHEADER
    if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER))
    {
        VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER*)pmt->pbFormat;
        
        // Configurar tamanho
        pvi->bmiHeader.biWidth = m_width;
        pvi->bmiHeader.biHeight = m_height;
        pvi->bmiHeader.biSizeImage = m_width * m_height * 3; // RGB24
        
        // Configurar formato (RGB24 por padrão)
        if (m_pixelFormat == 0)
        {
            pvi->bmiHeader.biCompression = BI_RGB;
            pvi->bmiHeader.biBitCount = 24;
        }
        
        // Configurar framerate
        if (m_fps > 0)
        {
            pvi->AvgTimePerFrame = 10000000LL / m_fps; // 100-nanosecond units
        }
        
        // Aplicar o formato
        hr = m_streamConfig->SetFormat(pmt);
        if (FAILED(hr))
        {
            LOG_WARN("Falha ao definir formato personalizado, usando formato padrão: " + std::to_string(hr));
            // Não é fatal - o dispositivo pode usar seu formato padrão
        }
        else
        {
            LOG_INFO("Formato configurado com sucesso");
        }
    }
    
    DeleteMediaType(pmt);
    return true;
}

// Método antigo removido - agora usamos configureCaptureFormat

bool VideoCaptureDS::setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
    if (m_dummyMode)
    {
        m_width = width;
        m_height = height;
        m_pixelFormat = pixelFormat;

        // Create dummy buffer (RGB24: 3 bytes per pixel)
        // Preencher com verde (0, 255, 0) para depuração
        size_t frameSize = width * height * 3;
        m_dummyFrameBuffer.resize(frameSize);
        // Preencher com verde: RGB(0, 255, 0)
        for (size_t i = 0; i < frameSize; i += 3)
        {
            m_dummyFrameBuffer[i] = 0;     // R
            m_dummyFrameBuffer[i + 1] = 255; // G
            m_dummyFrameBuffer[i + 2] = 0;   // B
        }

        LOG_INFO("Formato dummy definido: " + std::to_string(m_width) + "x" + std::to_string(m_height));
        return true;
    }

    m_width = width;
    m_height = height;
    m_pixelFormat = pixelFormat != 0 ? pixelFormat : 0; // 0 = RGB24

    // Se o graph já foi criado, reconfigurar o formato
    if (m_isOpen && m_streamConfig)
    {
        return configureCaptureFormat();
    }

    LOG_INFO("Formato definido: " + std::to_string(m_width) + "x" + std::to_string(m_height));
    return true;
}

bool VideoCaptureDS::setFramerate(uint32_t fps)
{
    if (m_dummyMode)
    {
        m_fps = fps;
        LOG_INFO("Framerate dummy configurado: " + std::to_string(fps) + "fps");
        return true;
    }

    m_fps = fps;

    // Se o graph já foi criado, reconfigurar o formato
    if (m_isOpen && m_streamConfig)
    {
        return configureCaptureFormat();
    }

    LOG_INFO("Framerate definido: " + std::to_string(fps) + "fps");
    return true;
}

bool VideoCaptureDS::startCapture()
{
    LOG_INFO("VideoCaptureDS::startCapture() chamado - m_dummyMode: " + std::string(m_dummyMode ? "true" : "false") +
             ", m_isOpen: " + std::string(m_isOpen ? "true" : "false"));
    
    if (m_dummyMode)
    {
        if (m_streaming)
        {
            return true;
        }

        if (m_dummyFrameBuffer.empty() && m_width > 0 && m_height > 0)
        {
            size_t frameSize = m_width * m_height * 3; // RGB24: 3 bytes per pixel
            m_dummyFrameBuffer.resize(frameSize);
            // Preencher com verde: RGB(0, 255, 0) para depuração
            for (size_t i = 0; i < frameSize; i += 3)
            {
                m_dummyFrameBuffer[i] = 0;     // R
                m_dummyFrameBuffer[i + 1] = 255; // G
                m_dummyFrameBuffer[i + 2] = 0;   // B
            }
        }

        m_streaming = true;
        LOG_INFO("Captura dummy iniciada: " + std::to_string(m_width) + "x" + std::to_string(m_height));
        return true;
    }

    if (!m_mediaControl)
    {
        LOG_ERROR("Media Control não está disponível");
        return false;
    }

    if (m_streaming)
    {
        return true;
    }

    // Iniciar o graph
    LOG_INFO("Iniciando graph DirectShow (Run)...");
    HRESULT hr = m_mediaControl->Run();
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao iniciar captura (Run falhou): " + std::to_string(hr));
        
        // Tentar obter mais informações sobre o estado do graph
        OAFilterState state;
        if (SUCCEEDED(m_mediaControl->GetState(100, &state)))
        {
            LOG_ERROR("Estado do graph: " + std::string(
                state == State_Stopped ? "Stopped" :
                state == State_Paused ? "Paused" :
                state == State_Running ? "Running" : "Unknown"));
        }
        
        return false;
    }

    m_streaming = true;
    m_hasFrame = false;
    LOG_INFO("Captura iniciada - graph está rodando (luz da câmera deve estar ligada)");
    
    // Verificar estado após iniciar
    OAFilterState state;
    if (SUCCEEDED(m_mediaControl->GetState(100, &state)))
    {
        LOG_INFO("Estado do graph após Run: " + std::string(
            state == State_Stopped ? "Stopped" :
            state == State_Paused ? "Paused" :
            state == State_Running ? "Running" : "Unknown"));
    }
    
    return true;
}

void VideoCaptureDS::stopCapture()
{
    if (!m_streaming)
    {
        return;
    }

    if (m_dummyMode)
    {
        m_streaming = false;
        LOG_INFO("Captura dummy parada");
        return;
    }

    if (m_mediaControl)
    {
        m_mediaControl->Stop();
    }

    m_streaming = false;
    m_hasFrame = false;
    LOG_INFO("Captura parada");
}

bool VideoCaptureDS::captureFrame(Frame &frame)
{
    if (m_dummyMode)
    {
        if (!m_streaming || m_dummyFrameBuffer.empty())
        {
            return false;
        }
        generateDummyFrame(frame);
        return true;
    }

    if (!m_streaming)
    {
        return false;
    }
    
    // Se Sample Grabber estiver disponível, usar método normal
    if (m_sampleGrabber)
    {
        return readSample(frame);
    }
    
    // Tentar captura alternativa sem Sample Grabber
    if (m_useAlternativeCapture && m_capturePin)
    {
        // Por enquanto, retornar false - implementação alternativa requer callback
        // Para capturar frames sem Sample Grabber, precisamos implementar um filtro customizado
        // que recebe samples via IMemInputPin. Isso é complexo e requer implementar várias
        // interfaces COM (IBaseFilter, IPin, IMemInputPin, etc.)
        static bool logged = false;
        if (!logged)
        {
            LOG_ERROR("Captura de frames não disponível: Sample Grabber não está disponível");
            LOG_ERROR("Para habilitar captura, é necessário implementar um filtro customizado ou usar Sample Grabber");
            logged = true;
        }
        return false;
    }
    
    return false;
}

bool VideoCaptureDS::captureLatestFrame(Frame &frame)
{
    if (m_dummyMode)
    {
        if (!m_streaming)
        {
            return false;
        }
        if (m_dummyFrameBuffer.empty())
        {
            return false;
        }
        generateDummyFrame(frame);
        if (!frame.data || frame.size == 0)
        {
            return false;
        }
        return true;
    }

    if (!m_streaming)
    {
        return false;
    }
    
    // Se Sample Grabber estiver disponível, usar método normal
    if (m_sampleGrabber)
    {
        // Para DirectShow, apenas ler o frame mais recente
        // ISampleGrabber já mantém o buffer mais recente quando SetBufferSamples(TRUE)
        return readSample(frame);
    }
    
    // Tentar captura alternativa sem Sample Grabber usando filtro customizado
    if (m_useAlternativeCapture && m_customGrabberFilter)
    {
        DSFrameGrabber *pGrabber = static_cast<DSFrameGrabber*>(m_customGrabberFilter);
        if (pGrabber)
        {
            uint32_t width = 0, height = 0;
            if (pGrabber->GetLatestFrame(nullptr, 0, width, height))
            {
                
                // Alocar buffer se necessário
                size_t frameSize = width * height * 3; // RGB24
                if (m_frameBuffer.size() < frameSize)
                {
                    m_frameBuffer.resize(frameSize);
                }
                
                // Obter frame do filtro customizado
                if (pGrabber->GetLatestFrame(m_frameBuffer.data(), m_frameBuffer.size(), width, height))
                {
                    m_width = width;
                    m_height = height;
                    
                    frame.data = m_frameBuffer.data();
                    frame.size = frameSize;
                    frame.width = width;
                    frame.height = height;
                    frame.format = m_pixelFormat; // RGB24
                    
                    m_hasFrame = true;
                    m_latestFrame = frame;
                    return true;
                }
            }
        }
        return false;
    }
    
    return false;
}

bool VideoCaptureDS::readSample(Frame &frame)
{
    if (!m_sampleGrabber)
    {
        return false;
    }

    HRESULT hr = S_OK;
    long bufferSize = 0;
    
    // Obter o buffer do Sample Grabber
    hr = m_sampleGrabber->GetCurrentBuffer(&bufferSize, nullptr);
    if (FAILED(hr) || bufferSize == 0)
    {
        return false;
    }
    
    // Alocar buffer se necessário
    if (m_frameBuffer.size() < static_cast<size_t>(bufferSize))
    {
        m_frameBuffer.resize(bufferSize);
    }
    
    // Obter os dados do buffer
    hr = m_sampleGrabber->GetCurrentBuffer(&bufferSize, (long*)m_frameBuffer.data());
    if (FAILED(hr))
    {
        return false;
    }
    
    // Obter informações do formato
    AM_MEDIA_TYPE mt;
    hr = m_sampleGrabber->GetConnectedMediaType(&mt);
    if (SUCCEEDED(hr) && mt.formattype == FORMAT_VideoInfo)
    {
        VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER*)mt.pbFormat;
        if (pvi)
        {
            m_width = pvi->bmiHeader.biWidth;
            m_height = abs(pvi->bmiHeader.biHeight); // Height pode ser negativo
        }
        FreeMediaType(mt);
    }
    
    // Preencher frame
    frame.data = m_frameBuffer.data();
    frame.size = bufferSize;
    frame.width = m_width;
    frame.height = m_height;
    frame.format = m_pixelFormat; // RGB24
    
    m_hasFrame = true;
    m_latestFrame = frame;
    
    return true;
}

void VideoCaptureDS::generateDummyFrame(Frame &frame)
{
    if (m_dummyFrameBuffer.empty() || m_width == 0 || m_height == 0)
    {
        LOG_WARN("generateDummyFrame: Buffer vazio ou dimensões inválidas (buffer: " + 
                 std::to_string(m_dummyFrameBuffer.size()) + 
                 ", dim: " + std::to_string(m_width) + "x" + std::to_string(m_height) + ")");
        return;
    }

    frame.data = m_dummyFrameBuffer.data();
    frame.size = m_dummyFrameBuffer.size();
    frame.width = m_width;
    frame.height = m_height;
    frame.format = m_pixelFormat; // 0 = RGB24
    
    // Log para depuração (apenas primeira vez)
    static bool firstLog = true;
    if (firstLog)
    {
        LOG_INFO("Dummy frame gerado: " + std::to_string(frame.width) + "x" + 
                 std::to_string(frame.height) + ", size: " + std::to_string(frame.size) + 
                 ", format: " + std::to_string(frame.format));
        firstLog = false;
    }
}

uint32_t VideoCaptureDS::getPixelFormat() const
{
    return m_pixelFormat;
}

bool VideoCaptureDS::setControl(const std::string &controlName, int32_t value)
{
    if (m_dummyMode)
    {
        return true; // Silently succeed in dummy mode
    }

    return setControlDS(controlName, value);
}

bool VideoCaptureDS::getControl(const std::string &controlName, int32_t &value)
{
    if (m_dummyMode)
    {
        return false; // No controls in dummy mode
    }

    return getControlDS(controlName, value);
}

bool VideoCaptureDS::getControlMin(const std::string &controlName, int32_t &minValue)
{
    if (!m_videoProcAmp && !m_cameraControl)
    {
        return false;
    }
    
    HRESULT hr = E_FAIL;
    long min = 0, max = 0, step = 0, defaultValue = 0, flags = 0;
    
    // Mapear nome do controle para propriedade DirectShow
    if (controlName == "Brightness")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Brightness, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Contrast")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Contrast, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Saturation")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Saturation, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Hue")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Hue, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Sharpness")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Sharpness, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Gamma")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Gamma, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Gain")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->GetRange(static_cast<CameraControlProperty>(1), &min, &max, &step, &defaultValue, &flags); // CameraControl_Gain = 1
        }
    }
    else if (controlName == "Exposure")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->GetRange(CameraControl_Exposure, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "White Balance")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->GetRange(static_cast<CameraControlProperty>(4), &min, &max, &step, &defaultValue, &flags); // CameraControl_WhiteBalance = 4
        }
    }
    else
    {
        return false;
    }
    
    if (SUCCEEDED(hr))
    {
        minValue = static_cast<int32_t>(min);
        return true;
    }
    
    return false;
}

bool VideoCaptureDS::getControlMax(const std::string &controlName, int32_t &maxValue)
{
    if (!m_videoProcAmp && !m_cameraControl)
    {
        return false;
    }
    
    HRESULT hr = E_FAIL;
    long min = 0, max = 0, step = 0, defaultValue = 0, flags = 0;
    
    // Mapear nome do controle para propriedade DirectShow
    if (controlName == "Brightness")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Brightness, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Contrast")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Contrast, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Saturation")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Saturation, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Hue")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Hue, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Sharpness")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Sharpness, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Gamma")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->GetRange(VideoProcAmp_Gamma, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "Gain")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->GetRange(static_cast<CameraControlProperty>(1), &min, &max, &step, &defaultValue, &flags); // CameraControl_Gain = 1
        }
    }
    else if (controlName == "Exposure")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->GetRange(CameraControl_Exposure, &min, &max, &step, &defaultValue, &flags);
        }
    }
    else if (controlName == "White Balance")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->GetRange(static_cast<CameraControlProperty>(4), &min, &max, &step, &defaultValue, &flags); // CameraControl_WhiteBalance = 4
        }
    }
    else
    {
        return false;
    }
    
    if (SUCCEEDED(hr))
    {
        maxValue = static_cast<int32_t>(max);
        return true;
    }
    
    return false;
}

bool VideoCaptureDS::getControlDefault(const std::string &controlName, int32_t &defaultValue)
{
    // DirectShow interfaces podem expor valores padrão (TODO: implementar)
    return false;
}

std::string VideoCaptureDS::getControlNameFromDS(const std::string &controlName)
{
    // Map control names to DirectShow property keys
    // Mapeamento simplificado - usar IAMCameraControl/IAMVideoProcAmp do DirectShow
    if (controlName == "brightness")
    {
        return "Brightness";
    }
    else if (controlName == "contrast")
    {
        return "Contrast";
    }
    else if (controlName == "saturation")
    {
        return "Saturation";
    }
    // Add more mappings as needed
    return controlName;
}

bool VideoCaptureDS::setControlDS(const std::string &controlName, int32_t value)
{
    if (!m_videoProcAmp && !m_cameraControl)
    {
        return false;
    }
    
    HRESULT hr = E_FAIL;
    long propertyValue = value;
    
    // Mapear nome do controle para propriedade DirectShow
    // IAMVideoProcAmp properties
    if (controlName == "Brightness")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Set(VideoProcAmp_Brightness, propertyValue, VideoProcAmp_Flags_Manual);
        }
    }
    else if (controlName == "Contrast")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Set(VideoProcAmp_Contrast, propertyValue, VideoProcAmp_Flags_Manual);
        }
    }
    else if (controlName == "Saturation")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Set(VideoProcAmp_Saturation, propertyValue, VideoProcAmp_Flags_Manual);
        }
    }
    else if (controlName == "Hue")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Set(VideoProcAmp_Hue, propertyValue, VideoProcAmp_Flags_Manual);
        }
    }
    else if (controlName == "Sharpness")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Set(VideoProcAmp_Sharpness, propertyValue, VideoProcAmp_Flags_Manual);
        }
    }
    else if (controlName == "Gamma")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Set(VideoProcAmp_Gamma, propertyValue, VideoProcAmp_Flags_Manual);
        }
    }
    // IAMCameraControl properties
    else if (controlName == "Gain")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->Set(static_cast<CameraControlProperty>(1), propertyValue, CameraControl_Flags_Manual); // CameraControl_Gain = 1
        }
    }
    else if (controlName == "Exposure")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->Set(CameraControl_Exposure, propertyValue, CameraControl_Flags_Manual);
        }
    }
    else if (controlName == "White Balance")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->Set(static_cast<CameraControlProperty>(4), propertyValue, CameraControl_Flags_Manual); // CameraControl_WhiteBalance = 4
        }
    }
    else
    {
        return false;
    }
    
    if (SUCCEEDED(hr))
    {
        return true;
    }
    
    return false;
}

bool VideoCaptureDS::getControlDS(const std::string &controlName, int32_t &value)
{
    if (!m_videoProcAmp && !m_cameraControl)
    {
        return false;
    }
    
    HRESULT hr = E_FAIL;
    long propertyValue = 0;
    long flags = 0;
    
    // Mapear nome do controle para propriedade DirectShow
    // IAMVideoProcAmp properties
    if (controlName == "Brightness")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Get(VideoProcAmp_Brightness, &propertyValue, &flags);
        }
    }
    else if (controlName == "Contrast")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Get(VideoProcAmp_Contrast, &propertyValue, &flags);
        }
    }
    else if (controlName == "Saturation")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Get(VideoProcAmp_Saturation, &propertyValue, &flags);
        }
    }
    else if (controlName == "Hue")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Get(VideoProcAmp_Hue, &propertyValue, &flags);
        }
    }
    else if (controlName == "Sharpness")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Get(VideoProcAmp_Sharpness, &propertyValue, &flags);
        }
    }
    else if (controlName == "Gamma")
    {
        if (m_videoProcAmp)
        {
            hr = m_videoProcAmp->Get(VideoProcAmp_Gamma, &propertyValue, &flags);
        }
    }
    // IAMCameraControl properties
    else if (controlName == "Gain")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->Get(static_cast<CameraControlProperty>(1), &propertyValue, &flags); // CameraControl_Gain = 1
        }
    }
    else if (controlName == "Exposure")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->Get(CameraControl_Exposure, &propertyValue, &flags);
        }
    }
    else if (controlName == "White Balance")
    {
        if (m_cameraControl)
        {
            hr = m_cameraControl->Get(static_cast<CameraControlProperty>(4), &propertyValue, &flags); // CameraControl_WhiteBalance = 4
        }
    }
    else
    {
        return false;
    }
    
    if (SUCCEEDED(hr))
    {
        value = static_cast<int32_t>(propertyValue);
        return true;
    }
    
    return false;
}

// Função auxiliar para enumerar dispositivos usando DirectShow
// Retorna true em *pSuccess se a enumeração foi executada com sucesso (mesmo que não encontre dispositivos)
static std::vector<DeviceInfo> EnumerateDevicesDirectShow(bool *pSuccess = nullptr)
{
    std::vector<DeviceInfo> devices;
    
    if (pSuccess)
        *pSuccess = false;
    
    LOG_INFO("Enumerando dispositivos via DirectShow...");
    
    HRESULT hr = S_OK;
    ICreateDevEnum *pDevEnum = nullptr;
    IEnumMoniker *pEnum = nullptr;
    
    // Inicializar COM se necessário
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInitializedHere = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        LOG_ERROR("Falha ao inicializar COM para DirectShow: " + std::to_string(hr) + " (0x" + 
                  std::to_string(static_cast<unsigned int>(hr)) + ")");
        return devices;
    }
    if (hr == RPC_E_CHANGED_MODE)
    {
        LOG_INFO("COM já estava inicializado para DirectShow - continuando...");
    }
    
    // Criar Device Enumerator
    #ifndef IID_PPV_ARGS
        hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ICreateDevEnum, (void**)&pDevEnum);
    #else
        hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&pDevEnum));
    #endif
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao criar Device Enumerator: " + std::to_string(hr) + " (0x" + 
                  std::to_string(static_cast<unsigned int>(hr)) + ")");
        _com_error err(hr);
        LOG_ERROR("Descrição do erro: " + std::string(err.ErrorMessage()));
        if (comInitializedHere)
        {
            CoUninitialize();
        }
        return devices;
    }
    
    LOG_INFO("Device Enumerator criado com sucesso");
    
    // Criar enumerador para categoria de captura de vídeo
    // CreateClassEnumerator retorna S_FALSE (1) quando não há dispositivos, mas isso não é um erro
    LOG_INFO("Criando enumerador para categoria de dispositivos de vídeo...");
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    
    // S_FALSE (1) significa que não há dispositivos, mas não é um erro
    if (hr == S_FALSE)
    {
        LOG_INFO("Nenhum dispositivo de vídeo encontrado via DirectShow (S_FALSE)");
        SafeRelease(&pDevEnum);
        if (comInitializedHere)
        {
            CoUninitialize();
        }
        return devices; // Retornar lista vazia, não é um erro
    }
    
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao criar enumerador de dispositivos de vídeo: " + std::to_string(hr) + " (0x" + 
                  std::to_string(static_cast<unsigned int>(hr)) + ")");
        _com_error err(hr);
        LOG_ERROR("Descrição do erro: " + std::string(err.ErrorMessage()));
        SafeRelease(&pDevEnum);
        if (comInitializedHere)
        {
            CoUninitialize();
        }
        return devices;
    }
    
    if (!pEnum)
    {
        LOG_WARN("Enumerador criado mas ponteiro é nulo");
        SafeRelease(&pDevEnum);
        if (comInitializedHere)
        {
            CoUninitialize();
        }
        return devices;
    }
    
    LOG_INFO("Enumerador de dispositivos criado com sucesso, enumerando...");
    
    // Enumerar dispositivos
    IMoniker *pMoniker = nullptr;
    UINT deviceIndex = 0;
    
    while (pEnum->Next(1, &pMoniker, nullptr) == S_OK)
    {
        DeviceInfo info;
        info.id = std::to_string(deviceIndex);
        
        // Obter propriedades do dispositivo
        IPropertyBag *pPropBag = nullptr;
        hr = pMoniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pPropBag);
        
        if (SUCCEEDED(hr))
        {
            VARIANT varName;
            VariantInit(&varName);
            
            // Obter nome amigável
            hr = pPropBag->Read(L"FriendlyName", &varName, nullptr);
            if (SUCCEEDED(hr) && varName.vt == VT_BSTR)
            {
                char nameBuffer[512];
                WideCharToMultiByte(CP_UTF8, 0, varName.bstrVal, -1, nameBuffer, sizeof(nameBuffer), nullptr, nullptr);
                info.name = std::string(nameBuffer);
                VariantClear(&varName);
            }
            else
            {
                info.name = "Dispositivo " + std::to_string(deviceIndex);
            }
            
            SafeRelease(&pPropBag);
        }
        else
        {
            info.name = "Dispositivo " + std::to_string(deviceIndex);
        }
        
        info.available = true;
        devices.push_back(info);
        
        SafeRelease(&pMoniker);
        deviceIndex++;
    }
    
    SafeRelease(&pEnum);
    SafeRelease(&pDevEnum);
    
    if (comInitializedHere)
    {
        CoUninitialize();
    }
    
    if (pSuccess)
        *pSuccess = true; // Enumeração executada com sucesso (mesmo que não encontre dispositivos)
    
    LOG_INFO("DirectShow encontrou " + std::to_string(devices.size()) + " dispositivo(s)");
    return devices;
}

std::vector<DeviceInfo> VideoCaptureDS::listDevices()
{
    // Usar apenas DirectShow para enumeração
    bool success = false;
    std::vector<DeviceInfo> devices = EnumerateDevicesDirectShow(&success);
    
    if (success)
    {
        LOG_INFO("Enumeração de dispositivos concluída via DirectShow");
    }
    else
    {
        LOG_WARN("Falha ao enumerar dispositivos via DirectShow");
    }
    
    return devices;
}

// Métodos de controle de hardware usando DirectShow

void VideoCaptureDS::setDummyMode(bool enabled)
{
    m_dummyMode = enabled;
}

bool VideoCaptureDS::isDummyMode() const
{
    return m_dummyMode;
}

std::vector<std::pair<uint32_t, uint32_t>> VideoCaptureDS::getSupportedResolutions(const std::string &deviceId)
{
    std::vector<std::pair<uint32_t, uint32_t>> resolutions;
    
    HRESULT hr = S_OK;
    ICreateDevEnum *pDevEnum = nullptr;
    IEnumMoniker *pEnum = nullptr;
    IMoniker *pMoniker = nullptr;
    IBaseFilter *pTempFilter = nullptr;
    IGraphBuilder *pTempGraph = nullptr;
    ICaptureGraphBuilder2 *pTempCaptureGraphBuilder = nullptr;
    IAMStreamConfig *pTempStreamConfig = nullptr;
    
    // Criar Filter Graph temporário
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, (void**)&pTempGraph);
    if (FAILED(hr))
    {
        LOG_WARN("Falha ao criar Filter Graph temporário para obter resoluções: " + std::to_string(hr));
        return resolutions;
    }
    
    // Criar Capture Graph Builder temporário
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICaptureGraphBuilder2, (void**)&pTempCaptureGraphBuilder);
    if (FAILED(hr))
    {
        SafeRelease(&pTempGraph);
        return resolutions;
    }
    
    hr = pTempCaptureGraphBuilder->SetFiltergraph(pTempGraph);
    if (FAILED(hr))
    {
        SafeRelease(&pTempCaptureGraphBuilder);
        SafeRelease(&pTempGraph);
        return resolutions;
    }
    
    // Criar Device Enumerator
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr))
    {
        SafeRelease(&pTempCaptureGraphBuilder);
        SafeRelease(&pTempGraph);
        return resolutions;
    }
    
    // Criar enumerador para categoria de captura de vídeo
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (FAILED(hr) || hr == S_FALSE || !pEnum)
    {
        SafeRelease(&pDevEnum);
        SafeRelease(&pTempCaptureGraphBuilder);
        SafeRelease(&pTempGraph);
        return resolutions;
    }
    
    // Encontrar o dispositivo pelo ID (índice)
    UINT32 deviceIndex = 0;
    if (!deviceId.empty() && deviceId != "default")
    {
        try
        {
            deviceIndex = static_cast<UINT32>(std::stoul(deviceId));
        }
        catch (...)
        {
            deviceIndex = 0;
        }
    }
    
    // Avançar até o dispositivo desejado
    pEnum->Reset();
    for (UINT32 i = 0; i <= deviceIndex; i++)
    {
        SafeRelease(&pMoniker);
        if (pEnum->Next(1, &pMoniker, nullptr) != S_OK)
        {
            SafeRelease(&pEnum);
            SafeRelease(&pDevEnum);
            SafeRelease(&pTempCaptureGraphBuilder);
            SafeRelease(&pTempGraph);
            return resolutions;
        }
    }
    
    // Criar filtro de captura temporário
    hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&pTempFilter);
    if (FAILED(hr))
    {
        SafeRelease(&pMoniker);
        SafeRelease(&pEnum);
        SafeRelease(&pDevEnum);
        SafeRelease(&pTempCaptureGraphBuilder);
        SafeRelease(&pTempGraph);
        return resolutions;
    }
    
    // Adicionar filtro ao graph temporário
    hr = pTempGraph->AddFilter(pTempFilter, L"Temp Video Capture");
    if (FAILED(hr))
    {
        SafeRelease(&pTempFilter);
        SafeRelease(&pMoniker);
        SafeRelease(&pEnum);
        SafeRelease(&pDevEnum);
        SafeRelease(&pTempCaptureGraphBuilder);
        SafeRelease(&pTempGraph);
        return resolutions;
    }
    
    // Obter IAMStreamConfig
    hr = pTempCaptureGraphBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                                  pTempFilter, IID_IAMStreamConfig, (void**)&pTempStreamConfig);
    if (SUCCEEDED(hr) && pTempStreamConfig)
    {
        int iCount = 0, iSize = 0;
        hr = pTempStreamConfig->GetNumberOfCapabilities(&iCount, &iSize);
        if (SUCCEEDED(hr) && iCount > 0)
        {
            // Alocar buffer para capabilities
            BYTE *pSCC = new BYTE[iSize];
            if (pSCC)
            {
                for (int i = 0; i < iCount; i++)
                {
                    AM_MEDIA_TYPE *pmt = nullptr;
                    hr = pTempStreamConfig->GetStreamCaps(i, &pmt, pSCC);
                    if (SUCCEEDED(hr) && pmt)
                    {
                        if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER))
                        {
                            VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER*)pmt->pbFormat;
                            if (pvi)
                            {
                                uint32_t width = pvi->bmiHeader.biWidth;
                                uint32_t height = abs(pvi->bmiHeader.biHeight);
                                
                                // Adicionar resolução se ainda não estiver na lista
                                bool found = false;
                                for (const auto &res : resolutions)
                                {
                                    if (res.first == width && res.second == height)
                                    {
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found)
                                {
                                    resolutions.push_back({width, height});
                                }
                            }
                        }
                        DeleteMediaType(pmt);
                    }
                }
                delete[] pSCC;
            }
        }
    }
    
    // Cleanup
    SafeRelease(&pTempStreamConfig);
    SafeRelease(&pTempFilter);
    SafeRelease(&pMoniker);
    SafeRelease(&pEnum);
    SafeRelease(&pDevEnum);
    SafeRelease(&pTempCaptureGraphBuilder);
    SafeRelease(&pTempGraph);
    
    return resolutions;
}

#endif // _WIN32
