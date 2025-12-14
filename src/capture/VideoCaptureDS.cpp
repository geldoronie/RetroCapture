#include "VideoCaptureDS.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cstring>

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

// Para CLSID_SampleGrabber e ISampleGrabber
#include <qedit.h>

// CLSID_SampleGrabber pode não estar definido no MinGW/MXE, definir manualmente se necessário
#ifndef CLSID_SampleGrabber
// Definir o GUID manualmente (valor do CLSID_SampleGrabber)
static const GUID CLSID_SampleGrabber = 
    { 0xC1F400A0, 0xF5F0, 0x11d0, { 0xA3, 0xBA, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
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
      m_width(0), m_height(0), m_fps(30), m_hasFrame(false), m_pixelFormat(0),
      m_isOpen(false), m_streaming(false), m_dummyMode(false), m_comInitialized(false)
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
    SafeRelease(&m_captureFilter);
    SafeRelease(&m_captureGraphBuilder);
    SafeRelease(&m_graphBuilder);

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
    IBaseFilter *pSampleGrabberFilter = nullptr;
    hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IBaseFilter, (void**)&pSampleGrabberFilter);
    if (SUCCEEDED(hr))
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
    }
    
    // Renderizar o stream de captura
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
    
    // Liberar referência ao Sample Grabber Filter (mantemos ISampleGrabber)
    SafeRelease(&pSampleGrabberFilter);
    
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
        size_t frameSize = width * height * 3;
        m_dummyFrameBuffer.resize(frameSize, 0);

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
    if (m_dummyMode)
    {
        if (m_streaming)
        {
            return true;
        }

        if (m_dummyFrameBuffer.empty() && m_width > 0 && m_height > 0)
        {
            size_t frameSize = m_width * m_height * 3; // RGB24: 3 bytes per pixel
            m_dummyFrameBuffer.resize(frameSize, 0);
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
    HRESULT hr = m_mediaControl->Run();
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao iniciar captura: " + std::to_string(hr));
        return false;
    }

    m_streaming = true;
    m_hasFrame = false;
    LOG_INFO("Captura iniciada");
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

    if (!m_sampleGrabber || !m_streaming)
    {
        return false;
    }

    return readSample(frame);
}

bool VideoCaptureDS::captureLatestFrame(Frame &frame)
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

    if (!m_sampleGrabber || !m_streaming)
    {
        return false;
    }

    // Para DirectShow, apenas ler o frame mais recente
    // ISampleGrabber já mantém o buffer mais recente quando SetBufferSamples(TRUE)
    return readSample(frame);
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
        return;
    }

    frame.data = m_dummyFrameBuffer.data();
    frame.size = m_dummyFrameBuffer.size();
    frame.width = m_width;
    frame.height = m_height;
    frame.format = m_pixelFormat;
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
    // DirectShow interfaces podem expor min/max (TODO: implementar)
    // This would require IAMCameraControl or IAMVideoProcAmp from DirectShow
    // For now, return false
    return false;
}

bool VideoCaptureDS::getControlMax(const std::string &controlName, int32_t &maxValue)
{
    // DirectShow interfaces podem expor min/max (TODO: implementar)
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
    // Usar DirectShow interfaces para controles de hardware
    if (!m_videoProcAmp && !m_cameraControl)
    {
        LOG_WARN("Interfaces de controle não disponíveis: " + controlName);
        return false;
    }
    
    // Mapear nome do controle para propriedade DirectShow
    // Por enquanto, implementação básica
    LOG_INFO("Definindo controle DirectShow: " + controlName + " = " + std::to_string(value));
    // TODO: Implementar mapeamento completo de controles
    return false;
}

bool VideoCaptureDS::getControlDS(const std::string &controlName, int32_t &value)
{
    // Usar DirectShow interfaces para controles de hardware
    if (!m_videoProcAmp && !m_cameraControl)
    {
        LOG_WARN("Interfaces de controle não disponíveis: " + controlName);
        return false;
    }
    
    // Mapear nome do controle para propriedade DirectShow
    LOG_INFO("Obtendo controle DirectShow: " + controlName);
    // TODO: Implementar mapeamento completo de controles
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

#endif // _WIN32
