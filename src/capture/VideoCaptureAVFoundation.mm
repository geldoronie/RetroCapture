#include "VideoCaptureAVFoundation.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cstring>
#include <unistd.h> // for usleep

#ifdef __APPLE__
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>

// Helper class para callback do AVFoundation
@interface VideoCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
    VideoCaptureAVFoundation* m_capture;
}

- (id)initWithCapture:(VideoCaptureAVFoundation*)capture;
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection;

@end

@implementation VideoCaptureDelegate

- (id)initWithCapture:(VideoCaptureAVFoundation*)capture
{
    self = [super init];
    if (self)
    {
        m_capture = capture;
    }
    return self;
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection
{
    (void)output;
    (void)connection;
    
    CVImageBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (pixelBuffer)
    {
        // IMPORTANT: Verify format from sample buffer matches activeFormat
        // This helps diagnose format mismatch issues
        CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (formatDesc)
        {
            CMVideoDimensions sampleDims = CMVideoFormatDescriptionGetDimensions(formatDesc);
            size_t pixelBufferWidth = CVPixelBufferGetWidth(pixelBuffer);
            size_t pixelBufferHeight = CVPixelBufferGetHeight(pixelBuffer);
            
            // Log format mismatch (only first few times)
            static int formatCheckCount = 0;
            if (formatCheckCount++ < 3)
            {
                LOG_INFO("=== SAMPLE BUFFER FORMAT CHECK ===");
                LOG_INFO("SampleBuffer format: " + std::to_string(sampleDims.width) + "x" + std::to_string(sampleDims.height));
                LOG_INFO("PixelBuffer dimensions: " + std::to_string(pixelBufferWidth) + "x" + std::to_string(pixelBufferHeight));
                LOG_INFO("==================================");
            }
        }
        
        // Não fazer Retain aqui - onFrameCaptured fará o Retain necessário
        // O sampleBuffer mantém o pixelBuffer vivo durante o callback
        m_capture->onFrameCaptured(pixelBuffer);
    }
}

@end
#endif

VideoCaptureAVFoundation::VideoCaptureAVFoundation()
#ifdef __APPLE__
    : m_captureSession(nil)
    , m_captureDevice(nil)
    , m_videoOutput(nil)
    , m_captureQueue(nil)
    , m_latestPixelBuffer(nullptr)
    , m_delegate(nil)
    , m_frameBuffer(nullptr)
    , m_frameBufferSize(0)
    , m_bufferMutex()
    , m_width(0)
    , m_height(0)
    , m_pixelFormat(0)
    , m_isOpen(false)
    , m_isCapturing(false)
    , m_dummyMode(false)
#else
    : m_width(0)
    , m_height(0)
    , m_pixelFormat(0)
    , m_isOpen(false)
    , m_isCapturing(false)
    , m_dummyMode(false)
#endif
{
}

VideoCaptureAVFoundation::~VideoCaptureAVFoundation()
{
    close();
}

#ifdef __APPLE__
void VideoCaptureAVFoundation::onFrameCaptured(CVPixelBufferRef pixelBuffer)
{
    if (!pixelBuffer)
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    if (m_latestPixelBuffer)
    {
        CVPixelBufferRelease(m_latestPixelBuffer);
    }
    
    CVPixelBufferRetain(pixelBuffer);
    m_latestPixelBuffer = pixelBuffer;
}
#endif

bool VideoCaptureAVFoundation::open(const std::string &device)
{
    if (m_isOpen)
    {
        LOG_WARN("Dispositivo já aberto, fechando primeiro");
        close();
    }

    if (m_dummyMode)
    {
        m_isOpen = true;
        LOG_INFO("Modo dummy ativado");
        return true;
    }

#ifdef __APPLE__
    @autoreleasepool {
        // Criar capture session
        m_captureSession = [[AVCaptureSession alloc] init];
        if (!m_captureSession)
        {
            LOG_ERROR("Falha ao criar AVCaptureSession");
            return false;
        }
        
        // IMPORTANT: On macOS, we don't set a sessionPreset to allow manual format control
        // The activeFormat of the device will determine the resolution
        // Setting a preset (like .high) would force a specific resolution
        // We'll configure the format explicitly via activeFormat before starting the session
        // Note: AVCaptureSessionPresetInputPriority is NOT available on macOS (iOS/iPadOS only)
        // So we explicitly set a low preset to avoid forcing a resolution
        
        // Try to set a low preset that won't interfere with activeFormat
        // If we can't set a low preset, we'll leave it unset (default may be High, which is bad)
        if ([m_captureSession canSetSessionPreset:AVCaptureSessionPresetLow])
        {
            m_captureSession.sessionPreset = AVCaptureSessionPresetLow;
            LOG_INFO("Session created - using AVCaptureSessionPresetLow (format controlled via activeFormat)");
        }
        else if ([m_captureSession canSetSessionPreset:AVCaptureSessionPresetMedium])
        {
            m_captureSession.sessionPreset = AVCaptureSessionPresetMedium;
            LOG_INFO("Session created - using AVCaptureSessionPresetMedium (format controlled via activeFormat)");
        }
        else
        {
            // If we can't set a low/medium preset, we'll try to leave it unset
            // But AVFoundation may default to High, which will override activeFormat
            LOG_WARN("Session created - could not set low preset, default may override activeFormat!");
            LOG_WARN("This may cause the device to ignore the requested format");
        }
        
        // Selecionar dispositivo
        AVCaptureDevice* deviceObj = nil;
        
        if (device.empty() || device == "default")
        {
            // Usar dispositivo padrão
            deviceObj = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        }
        else
        {
            // Buscar dispositivo usando AVCaptureDeviceDiscoverySession (API moderna)
            AVCaptureDeviceDiscoverySession* discoverySession = [AVCaptureDeviceDiscoverySession 
                discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternalUnknown]
                mediaType:AVMediaTypeVideo
                position:AVCaptureDevicePositionUnspecified];
            NSArray* devices = [discoverySession devices];
            for (AVCaptureDevice* dev in devices)
            {
                NSString* uniqueID = [dev uniqueID];
                NSString* localizedName = [dev localizedName];
                
                if ([uniqueID UTF8String] == device || 
                    [localizedName UTF8String] == device)
                {
                    deviceObj = dev;
                    break;
                }
            }
        }
        
        if (!deviceObj)
        {
            LOG_ERROR("Dispositivo não encontrado: " + device);
            [m_captureSession release];
            m_captureSession = nil;
            return false;
        }
        
        m_captureDevice = deviceObj;
        
        // CRITICAL: Set format BEFORE adding input to session
        // This is the only way to ensure AVFoundation respects the format
        // If m_width/m_height are already set (from previous setFormat call), use them
        // Otherwise, we'll set format to default and it will be changed in setFormat()
        if (m_width > 0 && m_height > 0)
        {
            LOG_INFO("Setting format during open() before adding input: " + 
                     std::to_string(m_width) + "x" + std::to_string(m_height));
            
            // Lock device for configuration
            NSError* configError = nil;
            if ([deviceObj lockForConfiguration:&configError])
            {
                // Find exact format match
                AVCaptureDeviceFormat* exactFormat = nil;
                for (AVCaptureDeviceFormat* format in [deviceObj formats])
                {
                    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
                    if (dims.width == static_cast<int32_t>(m_width) && 
                        dims.height == static_cast<int32_t>(m_height))
                    {
                        exactFormat = format;
                        break;
                    }
                }
                
                if (exactFormat)
                {
                    deviceObj.activeFormat = exactFormat;
                    LOG_INFO("Format set during open(): " + std::to_string(m_width) + "x" + std::to_string(m_height));
                }
                else
                {
                    LOG_WARN("Exact format " + std::to_string(m_width) + "x" + std::to_string(m_height) + 
                             " not found during open(), will use device default");
                }
                
                [deviceObj unlockForConfiguration];
            }
            else
            {
                LOG_WARN("Could not lock device for configuration during open(): " + 
                         std::string([[configError localizedDescription] UTF8String]));
            }
        }
        else
        {
            LOG_INFO("No format dimensions set yet, will be configured in setFormat()");
        }
        
        // Criar input (device format should now be set)
        NSError* error = nil;
        AVCaptureDeviceInput* input = [[AVCaptureDeviceInput alloc] initWithDevice:deviceObj error:&error];
        if (!input)
        {
            LOG_ERROR("Falha ao criar AVCaptureDeviceInput: " + std::string([[error localizedDescription] UTF8String]));
            [m_captureSession release];
            m_captureSession = nil;
            m_captureDevice = nil;
            return false;
        }
        
        if (![m_captureSession canAddInput:input])
        {
            LOG_ERROR("Não é possível adicionar input à sessão");
            [input release];
            [m_captureSession release];
            m_captureSession = nil;
            m_captureDevice = nil;
            return false;
        }
        
        [m_captureSession addInput:input];
        [input release];
        
        // Criar output
        m_videoOutput = [[AVCaptureVideoDataOutput alloc] init];
        if (!m_videoOutput)
        {
            LOG_ERROR("Falha ao criar AVCaptureVideoDataOutput");
            [m_captureSession release];
            m_captureSession = nil;
            m_captureDevice = nil;
            return false;
        }
        
        // Configurar formato de saída (BGRA para compatibilidade)
        NSDictionary* videoSettings = @{
            (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)
        };
        [m_videoOutput setVideoSettings:videoSettings];
        
        // Criar queue para callbacks
        m_captureQueue = dispatch_queue_create("com.retrocapture.capture", DISPATCH_QUEUE_SERIAL);
        
        // Criar delegate
        m_delegate = [[VideoCaptureDelegate alloc] initWithCapture:this];
        [m_videoOutput setSampleBufferDelegate:m_delegate queue:m_captureQueue];
        
        if (![m_captureSession canAddOutput:m_videoOutput])
        {
            LOG_ERROR("Não é possível adicionar output à sessão");
            [m_videoOutput release];
            [m_delegate release];
            dispatch_release(m_captureQueue);
            m_videoOutput = nil;
            m_delegate = nil;
            m_captureQueue = nil;
            [m_captureSession release];
            m_captureSession = nil;
            m_captureDevice = nil;
    return false;
        }
        
        [m_captureSession addOutput:m_videoOutput];
        
        m_isOpen = true;
        LOG_INFO("Dispositivo aberto: " + std::string([[deviceObj localizedName] UTF8String]));
        return true;
    }
#else
    LOG_ERROR("AVFoundation só está disponível no macOS");
    return false;
#endif
}

void VideoCaptureAVFoundation::close()
{
    if (m_isCapturing)
    {
        stopCapture();
    }

#ifdef __APPLE__
    @autoreleasepool {
    if (m_captureSession)
    {
            if ([m_captureSession isRunning])
            {
                [m_captureSession stopRunning];
            }
            [m_captureSession release];
            m_captureSession = nil;
        }
        
    if (m_videoOutput)
    {
            [m_videoOutput setSampleBufferDelegate:nil queue:nil];
            [m_videoOutput release];
            m_videoOutput = nil;
        }
        
        if (m_delegate)
        {
            [m_delegate release];
            m_delegate = nil;
        }
        
    if (m_captureQueue)
    {
            dispatch_release(m_captureQueue);
            m_captureQueue = nil;
    }
        
        m_captureDevice = nil;
        
        std::lock_guard<std::mutex> lock(m_bufferMutex);
    if (m_latestPixelBuffer)
    {
        CVPixelBufferRelease(m_latestPixelBuffer);
        m_latestPixelBuffer = nullptr;
        }
        
        if (m_frameBuffer)
        {
            delete[] m_frameBuffer;
            m_frameBuffer = nullptr;
            m_frameBufferSize = 0;
        }
    }
#endif

    m_isOpen = false;
    m_dummyFrameBuffer.clear();
    LOG_INFO("Dispositivo fechado");
}

bool VideoCaptureAVFoundation::isOpen() const
{
    return m_isOpen || m_dummyMode;
}

bool VideoCaptureAVFoundation::setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
    if (m_dummyMode)
    {
        m_width = width;
        m_height = height;
        m_pixelFormat = pixelFormat != 0 ? pixelFormat : 0x56595559; // YUYV equivalent

        size_t frameSize = width * height * 2;
        m_dummyFrameBuffer.resize(frameSize, 0);

        LOG_INFO("Formato dummy definido: " + std::to_string(m_width) + "x" +
                 std::to_string(m_height));
        return true;
    }

    if (!m_isOpen)
    {
        LOG_ERROR("Dispositivo não aberto");
        return false;
    }

#ifdef __APPLE__
    @autoreleasepool {
        // Configurar formato do dispositivo
        if (m_captureDevice)
        {
            NSError* error = nil;
            if ([m_captureDevice lockForConfiguration:&error])
            {
                // Calcular aspect ratio solicitado
                float requestedAspect = static_cast<float>(width) / static_cast<float>(height);
                
                // Tentar encontrar formato que corresponda à resolução desejada
                NSArray* formats = [m_captureDevice formats];
                AVCaptureDeviceFormat* exactFormat = nil;
                AVCaptureDeviceFormat* aspectFormat = nil;
                float bestAspectDiff = 999.0f;
                uint32_t bestAspectWidth = 0;
                uint32_t bestAspectHeight = 0;
                
                for (AVCaptureDeviceFormat* format in formats)
                {
                    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
                    
                    // Verificar formato exato
                    if ((int32_t)dims.width == (int32_t)width && (int32_t)dims.height == (int32_t)height)
                    {
                        exactFormat = format;
                        break; // Formato exato encontrado, usar este
                    }
                    
                    // Se não encontrou formato exato, procurar por formato com mesmo aspect ratio
                    float formatAspect = static_cast<float>(dims.width) / static_cast<float>(dims.height);
                    float aspectDiff = std::abs(formatAspect - requestedAspect);
                    
                    if (aspectDiff < bestAspectDiff)
                    {
                        bestAspectDiff = aspectDiff;
                        aspectFormat = format;
                        bestAspectWidth = dims.width;
                        bestAspectHeight = dims.height;
                    }
                }
                
                if (exactFormat)
                {
                    m_captureDevice.activeFormat = exactFormat;
                    m_width = width;
                    m_height = height;
                    LOG_INFO("Formato exato encontrado: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                }
                else if (aspectFormat && bestAspectDiff < 0.1f) // Tolerância de 0.1 para aspect ratio
                {
                    m_captureDevice.activeFormat = aspectFormat;
                    m_width = bestAspectWidth;
                    m_height = bestAspectHeight;
                    LOG_WARN("Formato exato não encontrado, usando formato com mesmo aspect ratio: " + 
                             std::to_string(m_width) + "x" + std::to_string(m_height) + 
                             " (solicitado: " + std::to_string(width) + "x" + std::to_string(height) + ")");
                }
                else
                {
                    LOG_WARN("Formato com aspect ratio similar não encontrado, usando formato do dispositivo");
                    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([m_captureDevice.activeFormat formatDescription]);
                    m_width = dims.width;
                    m_height = dims.height;
                    LOG_WARN("Formato do dispositivo: " + std::to_string(m_width) + "x" + std::to_string(m_height) + 
                             " (solicitado: " + std::to_string(width) + "x" + std::to_string(height) + ")");
                }
                
                [m_captureDevice unlockForConfiguration];
                
                // IMPORTANT: If session is already running, we need to stop it, remove inputs/outputs,
                // re-add them, and restart. This is the most reliable way to apply format changes.
                if (m_captureSession && [m_captureSession isRunning])
                {
                    LOG_INFO("Session is running - stopping to apply format change");
                    [m_captureSession stopRunning];
                    
                    // Remove all inputs and outputs
                    NSArray* inputs = [[m_captureSession inputs] copy];
                    for (AVCaptureInput* input in inputs)
                    {
                        [m_captureSession removeInput:input];
                    }
                    [inputs release];
                    
                    NSArray* outputs = [[m_captureSession outputs] copy];
                    for (AVCaptureOutput* output in outputs)
                    {
                        [m_captureSession removeOutput:output];
                    }
                    [outputs release];
                    
                    // Small delay to ensure format is fully applied
                    usleep(100000); // 100ms
                    
                    // Re-add input with new format
                    NSError* inputError = nil;
                    AVCaptureDeviceInput* newInput = [[AVCaptureDeviceInput alloc] initWithDevice:m_captureDevice error:&inputError];
                    if (newInput && [m_captureSession canAddInput:newInput])
                    {
                        [m_captureSession addInput:newInput];
                        [newInput release];
                        LOG_INFO("Re-added input with new format");
                    }
                    else
                    {
                        LOG_ERROR("Failed to re-add input: " + std::string([[inputError localizedDescription] UTF8String]));
                        [newInput release];
                    }
                    
                    // Re-add output
                    if (m_videoOutput && [m_captureSession canAddOutput:m_videoOutput])
                    {
                        [m_captureSession addOutput:m_videoOutput];
                        [m_videoOutput setSampleBufferDelegate:m_delegate queue:m_captureQueue];
                        LOG_INFO("Re-added output with new format: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                    }
                    else
                    {
                        LOG_ERROR("Cannot re-add output to session - format may not be compatible");
                    }
                    
                    // Session will be restarted by startCapture()
                }
                else if (m_videoOutput && m_captureSession && ![m_captureSession isRunning])
                {
                    // Session not running yet - just ensure output is added
                    // Format was set before input was added (in open()), so it should be respected
                    if (![m_captureSession.outputs containsObject:m_videoOutput])
                    {
                        if ([m_captureSession canAddOutput:m_videoOutput])
                        {
                            [m_captureSession addOutput:m_videoOutput];
                            [m_videoOutput setSampleBufferDelegate:m_delegate queue:m_captureQueue];
                            LOG_INFO("Added output to session with format: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                        }
                    }
                }
                
                // IMPORTANT: Format is now set
                // If session was running, it will be restarted by startCapture()
                // If session was not running, format was set before input was added, so it should be respected
                LOG_INFO("Format configured: " + std::to_string(m_width) + "x" + std::to_string(m_height) + 
                         " (session will use this format when started)");
            }
            else
            {
                LOG_ERROR("Falha ao bloquear dispositivo para configuração");
                return false;
            }
        }
    }
#else
    m_width = width;
    m_height = height;
    m_pixelFormat = pixelFormat;
    LOG_INFO("Formato definido: " + std::to_string(m_width) + "x" + std::to_string(m_height));
#endif

    return true;
}

bool VideoCaptureAVFoundation::setFramerate(uint32_t fps)
{
    if (m_dummyMode)
    {
        return true;
    }

    if (!m_isOpen)
    {
        LOG_ERROR("Dispositivo não aberto");
        return false;
    }

#ifdef __APPLE__
    @autoreleasepool {
        if (m_captureDevice)
        {
            NSError* error = nil;
            if ([m_captureDevice lockForConfiguration:&error])
            {
                // Configurar framerate
                AVFrameRateRange* bestRange = nil;
                for (AVFrameRateRange* range in [m_captureDevice.activeFormat videoSupportedFrameRateRanges])
                {
                    if (fps >= range.minFrameRate && fps <= range.maxFrameRate)
                    {
                        bestRange = range;
                        break;
                    }
                }
                
                if (bestRange)
                {
                    CMTime frameDuration = CMTimeMake(1, fps);
                    m_captureDevice.activeVideoMinFrameDuration = frameDuration;
                    m_captureDevice.activeVideoMaxFrameDuration = frameDuration;
    LOG_INFO("Framerate definido: " + std::to_string(fps) + " fps");
                }
                else
                {
                    LOG_WARN("Framerate não suportado, usando padrão do dispositivo");
                }
                
                [m_captureDevice unlockForConfiguration];
            }
        }
    }
#endif

    return true;
}

bool VideoCaptureAVFoundation::captureFrame(Frame &frame)
{
    return captureLatestFrame(frame);
}

bool VideoCaptureAVFoundation::captureLatestFrame(Frame &frame)
{
    if (m_dummyMode)
    {
        generateDummyFrame(frame);
        return true;
    }

    if (!m_isOpen || !m_isCapturing)
    {
        return false;
    }

#ifdef __APPLE__
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    if (m_latestPixelBuffer)
    {
        return convertPixelBufferToFrame(m_latestPixelBuffer, frame);
    }
#endif

    return false;
}

bool VideoCaptureAVFoundation::setControl(const std::string &controlName, int32_t value)
{
#ifdef __APPLE__
    if (!m_captureDevice)
    {
        return false;
    }
    
    @autoreleasepool {
        NSError* error = nil;
        if ([m_captureDevice lockForConfiguration:&error])
        {
            bool success = false;
            
            if (controlName == "brightness")
            {
                if ([m_captureDevice isExposureModeSupported:AVCaptureExposureModeAutoExpose])
                {
                    // Brightness não é diretamente controlável, usar exposure
                    success = true;
                }
            }
            else if (controlName == "contrast")
            {
                // Contrast não é diretamente controlável via AVFoundation
                success = false;
            }
            else if (controlName == "saturation")
            {
                // Saturation não é diretamente controlável via AVFoundation
                success = false;
            }
            else             if (controlName == "exposure")
            {
                // Exposure controls não estão disponíveis no macOS via AVFoundation
                // Apenas iOS/iPadOS suportam controle manual de exposição
                success = false;
            }
            
            [m_captureDevice unlockForConfiguration];
            return success;
        }
    }
#endif
    
    return false;
}

bool VideoCaptureAVFoundation::getControl(const std::string &controlName, int32_t &value)
{
    (void)controlName;
    (void)value;
    // TODO: Implementar leitura de controles
    return false;
}

bool VideoCaptureAVFoundation::getControlMin(const std::string &controlName, int32_t &minValue)
{
    (void)controlName;
    (void)minValue;
    return false;
}

bool VideoCaptureAVFoundation::getControlMax(const std::string &controlName, int32_t &maxValue)
{
    (void)controlName;
    (void)maxValue;
    return false;
}

bool VideoCaptureAVFoundation::getControlDefault(const std::string &controlName, int32_t &defaultValue)
{
    (void)controlName;
    (void)defaultValue;
    return false;
}

std::vector<DeviceInfo> VideoCaptureAVFoundation::listDevices()
{
    std::vector<DeviceInfo> devices;

#ifdef __APPLE__
    @autoreleasepool {
        // Sempre listar dispositivos reais, mesmo em dummy mode
        // Isso permite que o usuário selecione um dispositivo mesmo quando está em dummy mode
        // Nota: UltraWide e Telephoto são apenas iOS, não macOS
        AVCaptureDeviceDiscoverySession* discoverySession = [AVCaptureDeviceDiscoverySession 
            discoverySessionWithDeviceTypes:@[
                AVCaptureDeviceTypeBuiltInWideAngleCamera,
                AVCaptureDeviceTypeExternalUnknown
            ]
            mediaType:AVMediaTypeVideo
            position:AVCaptureDevicePositionUnspecified];
        NSArray* videoDevices = [discoverySession devices];
        
        LOG_INFO("Encontrados " + std::to_string([videoDevices count]) + " dispositivos AVFoundation");
        
        for (AVCaptureDevice* device in videoDevices)
        {
            DeviceInfo info;
            NSString* uniqueID = [device uniqueID];
            NSString* localizedName = [device localizedName];
            
            info.id = std::string([uniqueID UTF8String]);
            info.name = std::string([localizedName UTF8String]);
            info.available = [device isConnected];
            info.driver = "AVFoundation";
            devices.push_back(info);
            
            LOG_INFO("Dispositivo encontrado: " + info.name + " (ID: " + info.id + ", Connected: " + (info.available ? "sim" : "não") + ")");
        }
        
        // Se não encontrou dispositivos, pode ser problema de permissões
        if ([videoDevices count] == 0)
        {
            LOG_WARN("Nenhum dispositivo de vídeo encontrado. Verifique as permissões da câmera nas Preferências do Sistema.");
        }
    }
#endif

    return devices;
}

void VideoCaptureAVFoundation::setDummyMode(bool enabled)
{
    if (m_isOpen && !enabled)
    {
        close();
    }
    m_dummyMode = enabled;
    LOG_INFO("Modo dummy: " + std::string(enabled ? "ativado" : "desativado"));
}

bool VideoCaptureAVFoundation::isDummyMode() const
{
    return m_dummyMode;
}

bool VideoCaptureAVFoundation::startCapture()
{
    if (m_dummyMode)
    {
        m_isCapturing = true;
        return true;
    }

    if (!m_isOpen)
    {
        LOG_ERROR("Dispositivo não aberto");
        return false;
    }

#ifdef __APPLE__
    @autoreleasepool {
        if (m_captureSession && ![m_captureSession isRunning])
        {
            // IMPORTANT: Verify format is set before starting session
            // This ensures the session uses the correct format
            if (m_captureDevice)
            {
                CMVideoDimensions activeDims = CMVideoFormatDescriptionGetDimensions([m_captureDevice.activeFormat formatDescription]);
                LOG_INFO("=== BEFORE STARTING SESSION ===");
                LOG_INFO("ActiveFormat: " + std::to_string(activeDims.width) + "x" + std::to_string(activeDims.height));
                LOG_INFO("Requested: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                NSString* preset = m_captureSession.sessionPreset;
                LOG_INFO("Session preset: " + std::string([preset UTF8String]));
                
                // CRITICAL: If preset is High, it will override activeFormat!
                if ([preset isEqualToString:AVCaptureSessionPresetHigh] || 
                    [preset isEqualToString:AVCaptureSessionPreset1920x1080])
                {
                    LOG_ERROR("Session preset is High/1920x1080 - this WILL override activeFormat!");
                    LOG_ERROR("We need to change the preset before starting the session");
                    
                    // Try to change to Low preset
                    if ([m_captureSession canSetSessionPreset:AVCaptureSessionPresetLow])
                    {
                        [m_captureSession setSessionPreset:AVCaptureSessionPresetLow];
                        LOG_INFO("Changed session preset to Low to avoid overriding activeFormat");
                    }
                    else
                    {
                        LOG_ERROR("Cannot change session preset - format may be overridden!");
                    }
                }
                
                // Verify format matches requested dimensions
                if (m_width > 0 && m_height > 0)
                {
                    if ((int32_t)activeDims.width != (int32_t)m_width || (int32_t)activeDims.height != (int32_t)m_height)
                    {
                        LOG_WARN("ActiveFormat dimensions (" + std::to_string(activeDims.width) + "x" + std::to_string(activeDims.height) + 
                                 ") differ from requested (" + std::to_string(m_width) + "x" + std::to_string(m_height) + 
                                 ") - aspect ratio will use requested dimensions");
                    }
                    else
                    {
                        LOG_INFO("ActiveFormat matches requested dimensions - format should be respected");
                    }
                }
            }
            
            [m_captureSession startRunning];
            
            // Verificar activeFormat DEPOIS de iniciar (alguns dispositivos podem mudar)
            usleep(100000); // 100ms delay para sessão estabilizar
            if (m_captureDevice && m_width > 0 && m_height > 0)
            {
                CMVideoDimensions activeDimsAfter = CMVideoFormatDescriptionGetDimensions([m_captureDevice.activeFormat formatDescription]);
                LOG_INFO("=== AFTER STARTING SESSION ===");
                LOG_INFO("ActiveFormat: " + std::to_string(activeDimsAfter.width) + "x" + std::to_string(activeDimsAfter.height));
                
                // Se o formato mudou após iniciar, tentar corrigir
                if (activeDimsAfter.width != (int32_t)m_width || activeDimsAfter.height != (int32_t)m_height)
                {
                    LOG_WARN("ActiveFormat changed after starting session!");
                    LOG_WARN("Requested: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                    LOG_WARN("Actual: " + std::to_string(activeDimsAfter.width) + "x" + std::to_string(activeDimsAfter.height));
                    LOG_WARN("Attempting to fix by stopping, reconfiguring format, and restarting...");
                    
                    // Parar a sessão
                    [m_captureSession stopRunning];
                    usleep(50000); // 50ms delay
                    
                    // Reconfigurar o formato
                    NSError* configError = nil;
                    if ([m_captureDevice lockForConfiguration:&configError])
                    {
                        // Encontrar formato exato novamente
                        AVCaptureDeviceFormat* exactFormat = nil;
                        for (AVCaptureDeviceFormat* format in [m_captureDevice formats])
                        {
                            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
                            if (dims.width == static_cast<int32_t>(m_width) && 
                                dims.height == static_cast<int32_t>(m_height))
                            {
                                exactFormat = format;
                                break;
                            }
                        }
                        
                        if (exactFormat)
                        {
                            m_captureDevice.activeFormat = exactFormat;
                            LOG_INFO("Reconfigured activeFormat to: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                        }
                        else
                        {
                            LOG_WARN("Could not find exact format for reconfiguration");
                        }
                        
                        [m_captureDevice unlockForConfiguration];
                    }
                    else
                    {
                        LOG_ERROR("Could not lock device for reconfiguration: " + 
                                 std::string([[configError localizedDescription] UTF8String]));
                    }
                    
                    // Reiniciar a sessão
                    usleep(50000); // 50ms delay
                    [m_captureSession startRunning];
                    
                    // Verificar novamente após reiniciar
                    usleep(100000); // 100ms delay
                    CMVideoDimensions activeDimsFinal = CMVideoFormatDescriptionGetDimensions([m_captureDevice.activeFormat formatDescription]);
                    LOG_INFO("=== AFTER RESTARTING SESSION ===");
                    LOG_INFO("ActiveFormat: " + std::to_string(activeDimsFinal.width) + "x" + std::to_string(activeDimsFinal.height));
                    
                    if (activeDimsFinal.width != (int32_t)m_width || activeDimsFinal.height != (int32_t)m_height)
                    {
                        LOG_ERROR("ActiveFormat STILL does not match after restart!");
                        LOG_ERROR("This device may not support the requested format, or the driver is forcing 1920x1080");
                        LOG_ERROR("Aspect ratio will use requested dimensions (" + std::to_string(m_width) + "x" + std::to_string(m_height) + ")");
                    }
                }
                else
                {
                    LOG_INFO("ActiveFormat matches requested dimensions after starting - format is correct!");
                }
            }
    m_isCapturing = true;
            LOG_INFO("Captura iniciada");
    return true;
        }
    }
#endif

    return false;
}

void VideoCaptureAVFoundation::stopCapture()
{
    m_isCapturing = false;
#ifdef __APPLE__
    @autoreleasepool {
        if (m_captureSession && [m_captureSession isRunning])
        {
            [m_captureSession stopRunning];
    LOG_INFO("Captura parada");
        }
    }
#endif
}

void VideoCaptureAVFoundation::generateDummyFrame(Frame &frame)
{
    if (m_dummyFrameBuffer.empty())
    {
        frame.data = nullptr;
        frame.size = 0;
        return;
    }

    frame.data = m_dummyFrameBuffer.data();
    frame.size = m_dummyFrameBuffer.size();
    frame.width = m_width;
    frame.height = m_height;
    frame.format = m_pixelFormat;

    // Gerar padrão de teste simples
    static uint32_t frameCount = 0;
    frameCount++;
    for (size_t i = 0; i < m_dummyFrameBuffer.size(); i += 2)
    {
        m_dummyFrameBuffer[i] = (frameCount + i) % 256;
        m_dummyFrameBuffer[i + 1] = ((frameCount + i) * 2) % 256;
    }
}

bool VideoCaptureAVFoundation::convertPixelBufferToFrame(CVPixelBufferRef pixelBuffer, Frame &frame)
{
#ifdef __APPLE__
    if (!pixelBuffer)
    {
        return false;
    }

    size_t width = CVPixelBufferGetWidth(pixelBuffer);
    size_t height = CVPixelBufferGetHeight(pixelBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
    
    // IMPORTANT: Log dimensões reais do pixelBuffer para debug
    // O pixelBuffer pode ter dimensões diferentes do que foi solicitado
    static int dimensionLogCount = 0;
    if (dimensionLogCount++ < 5)
    {
        LOG_INFO("=== PIXEL BUFFER DIMENSIONS ===");
        LOG_INFO("PixelBuffer: " + std::to_string(width) + "x" + std::to_string(height));
        LOG_INFO("Requested (m_width/m_height): " + std::to_string(m_width) + "x" + std::to_string(m_height));
        if (m_width > 0 && m_height > 0)
        {
            float requestedAspect = static_cast<float>(m_width) / static_cast<float>(m_height);
            float actualAspect = static_cast<float>(width) / static_cast<float>(height);
            LOG_INFO("Requested aspect: " + std::to_string(requestedAspect));
            LOG_INFO("Actual aspect: " + std::to_string(actualAspect));
            if (std::abs(requestedAspect - actualAspect) > 0.01f)
            {
                LOG_WARN("Aspect ratio mismatch! Requested: " + std::to_string(requestedAspect) + 
                         ", Actual: " + std::to_string(actualAspect));
            }
        }
        LOG_INFO("==============================");
    }
    
    // Converter BGRA para YUYV
    // Alocar buffer se necessário
    size_t yuyvSize = width * height * 2; // YUYV é 2 bytes por pixel
    
    if (!m_frameBuffer || m_frameBufferSize < yuyvSize)
    {
        if (m_frameBuffer)
        {
            delete[] m_frameBuffer;
        }
        m_frameBuffer = new uint8_t[yuyvSize];
        m_frameBufferSize = yuyvSize;
    }

    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    uint8_t* baseAddress = (uint8_t*)CVPixelBufferGetBaseAddress(pixelBuffer);
    
    // Converter BGRA para YUYV
    for (size_t y = 0; y < height; ++y)
    {
        uint8_t* srcRow = baseAddress + y * bytesPerRow;
        uint8_t* dstRow = m_frameBuffer + y * width * 2;
        
        for (size_t x = 0; x < width; x += 2)
        {
            // Ler 2 pixels BGRA
            uint8_t b1 = srcRow[x * 4];
            uint8_t g1 = srcRow[x * 4 + 1];
            uint8_t r1 = srcRow[x * 4 + 2];
            
            uint8_t b2 = srcRow[(x + 1) * 4];
            uint8_t g2 = srcRow[(x + 1) * 4 + 1];
            uint8_t r2 = srcRow[(x + 1) * 4 + 2];
            
            // Converter RGB para YUV
            // Y = 0.299*R + 0.587*G + 0.114*B
            // U = -0.169*R - 0.331*G + 0.5*B + 128
            // V = 0.5*R - 0.419*G - 0.081*B + 128
            
            int y1 = (int)(0.299f * r1 + 0.587f * g1 + 0.114f * b1);
            int u = (int)(-0.169f * r1 - 0.331f * g1 + 0.5f * b1 + 128);
            int y2 = (int)(0.299f * r2 + 0.587f * g2 + 0.114f * b2);
            int v = (int)(0.5f * r1 - 0.419f * g1 - 0.081f * b1 + 128);
            
            // Clamp valores
            y1 = std::max(0, std::min(255, y1));
            u = std::max(0, std::min(255, u));
            y2 = std::max(0, std::min(255, y2));
            v = std::max(0, std::min(255, v));
            
            // YUYV: Y0 U Y1 V
            dstRow[x * 2] = (uint8_t)y1;
            dstRow[x * 2 + 1] = (uint8_t)u;
            dstRow[x * 2 + 2] = (uint8_t)y2;
            dstRow[x * 2 + 3] = (uint8_t)v;
        }
    }
    
    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    frame.data = m_frameBuffer;
    frame.size = yuyvSize;
    // IMPORTANT: Use actual pixelBuffer dimensions, not requested dimensions
    // This ensures aspect ratio is calculated correctly based on actual frame dimensions
    frame.width = (uint32_t)width;
    frame.height = (uint32_t)height;
    frame.format = 0x56595559; // YUYV
    
    // IMPORTANT: Do NOT update m_width/m_height to match actual pixelBuffer dimensions
    // m_width/m_height should remain as REQUESTED dimensions for aspect ratio calculation
    // The actual frame dimensions (frame.width/height) are used for rendering,
    // but requested dimensions (m_width/m_height) are used for aspect ratio
    // This ensures maintainAspect uses the requested aspect ratio, not the actual device aspect ratio
    if (m_width != (uint32_t)width || m_height != (uint32_t)height)
    {
        if (dimensionLogCount <= 5)
        {
            LOG_WARN("PixelBuffer dimensions differ from requested: " + 
                     std::to_string(width) + "x" + std::to_string(height) + 
                     " (requested: " + std::to_string(m_width) + "x" + std::to_string(m_height) + ")");
            LOG_WARN("Using REQUESTED dimensions for aspect ratio calculation");
        }
        // Do NOT update m_width/m_height - keep requested dimensions for aspect ratio
    }
    
    return true;
#else
    return false;
#endif
}
