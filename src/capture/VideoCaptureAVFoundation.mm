#include "VideoCaptureAVFoundation.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cstring>
#include <unistd.h> // for usleep
#include <chrono> // for frame rate measurement

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
    
    // CRITICAL: Minimize work in delegate - any delay here will cause frame drops
    // OBS Studio approach: delegate should do MINIMAL work, just pass the buffer
    CVImageBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (pixelBuffer)
    {
        // IMPORTANT: Call onFrameCaptured IMMEDIATELY, no logging or checks
        // The sampleBuffer keeps the pixelBuffer alive during the callback
        // Any delay here will cause AVFoundation to drop frames or reduce framerate
        m_capture->onFrameCaptured(pixelBuffer);
        
        // CRITICAL: Do NOT do any logging or measurement in the delegate!
        // This will block the delegate and cause frame drops
        // OBS Studio approach: delegate does MINIMAL work, just passes the buffer
        // All logging and measurement should be done in the main thread, not in the delegate
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
    , m_fps(30) // Default to 30 fps
    , m_isOpen(false)
    , m_isCapturing(false)
    , m_dummyMode(false)
    , m_formatSelectedViaUI(false)
    , m_selectedFormatId("")
#else
    : m_width(0)
    , m_height(0)
    , m_pixelFormat(0)
    , m_fps(30) // Default to 30 fps
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
    
    // CRITICAL: Minimize lock time - this is called from the delegate queue
    // Any delay here will cause AVFoundation to drop frames or reduce framerate
    // OBS Studio approach: minimal work, just swap the buffer reference
    CVPixelBufferRef oldBuffer = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        oldBuffer = m_latestPixelBuffer;
        // IMPORTANT: Retain BEFORE assigning to ensure buffer stays alive
        // The delegate's sampleBuffer will release its reference when callback returns
        CVPixelBufferRetain(pixelBuffer);
        m_latestPixelBuffer = pixelBuffer;
    }
    
    // Release old buffer OUTSIDE the lock to minimize blocking
    // This prevents the delegate from being blocked by CVPixelBufferRelease
    if (oldBuffer)
    {
        CVPixelBufferRelease(oldBuffer);
    }
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
        
        // IMPORTANT: OBS Studio approach - do NOT set sessionPreset when using manual format control
        // The activeFormat of the device will determine the resolution
        // Setting a preset (even Low) can interfere with activeFormat on some devices
        // We'll configure the format explicitly via activeFormat within beginConfiguration/commitConfiguration
        // Note: AVCaptureSessionPresetInputPriority is NOT available on macOS (iOS/iPadOS only)
        // OBS Studio does not set sessionPreset when using manual format configuration
        LOG_INFO("Session created - format will be controlled via activeFormat (no preset)");
        
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
        
        // IMPORTANT: OBS Studio approach - use beginConfiguration/commitConfiguration
        // Configure format and add input ALL within the same configuration block
        [m_captureSession beginConfiguration];
        
        // CRITICAL: OBS Studio approach - configure format WITHIN beginConfiguration/commitConfiguration
        // This ensures the format is set atomically with the input addition
        // If m_width/m_height are already set (from previous setFormat call), use them
        if (m_width > 0 && m_height > 0)
        {
            LOG_INFO("Setting format during open() before adding input: " + 
                     std::to_string(m_width) + "x" + std::to_string(m_height));
            
            // Lock device for configuration (within beginConfiguration block)
            NSError* configError = nil;
            if ([deviceObj lockForConfiguration:&configError])
            {
                // IMPORTANT: OBS Studio approach - find format matching dimensions AND prefer native formats (NV12/YUY2)
                // OBS prioritizes formats with native pixel formats (NV12 420v, YUY2 yuvs) over BGRA
                // This ensures the device uses its native format, which is more efficient and reliable
                AVCaptureDeviceFormat* exactFormat = nil;
                AVCaptureDeviceFormat* exactFormatNative = nil; // Prefer native formats
                
                for (AVCaptureDeviceFormat* format in [deviceObj formats])
                {
                    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
                    if (dims.width == static_cast<int32_t>(m_width) && 
                        dims.height == static_cast<int32_t>(m_height))
                    {
                        // Check pixel format - prefer native formats (NV12, YUY2)
                        CMFormatDescriptionRef formatDesc = [format formatDescription];
                        FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(formatDesc);
                        
                        // IMPORTANT: OBS Studio approach - prefer native formats
                        // NV12 (420v) = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange (0x34323076)
                        // YUY2 (yuvs) = kCVPixelFormatType_422YpCbCr8_yuvs (0x79757673)
                        // Also check for other common YUV formats
                        if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                            pixelFormat == kCVPixelFormatType_422YpCbCr8_yuvs ||
                            pixelFormat == kCVPixelFormatType_422YpCbCr8 ||
                            pixelFormat == 0x79757673 || // 'yuvs' (YUY2)
                            pixelFormat == 0x34323076)   // '420v' (NV12)
                        {
                            exactFormatNative = format; // Prefer native format
                        }
                        else if (!exactFormat)
                        {
                            exactFormat = format; // Fallback to any matching format
                        }
                    }
                }
                
                // Use native format if found, otherwise use any matching format
                if (exactFormatNative)
                {
                    exactFormat = exactFormatNative;
                }
                
                if (exactFormat)
                {
                    // IMPORTANT: Set format WITHIN beginConfiguration/commitConfiguration (OBS Studio approach)
                    deviceObj.activeFormat = exactFormat;
                    
                    // Log pixel format for debugging (OBS Studio approach)
                    CMFormatDescriptionRef formatDesc = [exactFormat formatDescription];
                    FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(formatDesc);
                    const char* formatName = "Unknown";
                    if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
                        formatName = "NV12 (420v)";
                    else if (pixelFormat == kCVPixelFormatType_422YpCbCr8_yuvs)
                        formatName = "YUY2 (yuvs)";
                    else if (pixelFormat == kCVPixelFormatType_32BGRA)
                        formatName = "BGRA";
                    
                    LOG_INFO("Format set during open(): " + std::to_string(m_width) + "x" + std::to_string(m_height) + 
                             " (pixel format: " + formatName + ", 0x" + std::to_string(pixelFormat) + ")");
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
            [m_captureSession commitConfiguration];
            [m_captureSession release];
            m_captureSession = nil;
            m_captureDevice = nil;
            return false;
        }
        
        if (![m_captureSession canAddInput:input])
        {
            LOG_ERROR("Não é possível adicionar input à sessão");
            [input release];
            [m_captureSession commitConfiguration];
            [m_captureSession release];
            m_captureSession = nil;
            m_captureDevice = nil;
            return false;
        }
        
        // IMPORTANT: Add input within beginConfiguration/commitConfiguration (OBS Studio approach)
        // This is done AFTER setting the format, all within the same configuration block
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
        
        // IMPORTANT: OBS Studio approach - do NOT force pixel format in videoSettings
        // Let the device use its native format (NV12, YUY2, etc.) from activeFormat
        // Forcing BGRA can cause format mismatches and performance issues
        // The activeFormat's native pixel format will be used automatically
        // If we need BGRA, we can convert from the native format in the delegate
        // For now, let's try without forcing a format to see if it helps with format selection
        NSDictionary* videoSettings = @{
            (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)
        };
        [m_videoOutput setVideoSettings:videoSettings];
        
        // IMPORTANT: Try NO to allow buffering - this may help with framerate
        // When YES, frames are discarded if processing is delayed, which can cause low framerate
        // When NO, frames are buffered and delivered even if processing is slightly delayed
        // This is a trade-off: YES = lower latency but may drop frames, NO = higher latency but more stable framerate
        m_videoOutput.alwaysDiscardsLateVideoFrames = NO; // Buffer frames to maintain framerate
        
        // IMPORTANT: OBS Studio approach - use a simple serial queue (not high priority)
        // High priority queues can cause scheduling issues and may not help with framerate
        // OBS Studio uses: dispatch_queue_create(nil, nil) - simple serial queue
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
            [m_captureSession commitConfiguration];
            [m_captureSession release];
            m_captureSession = nil;
            m_captureDevice = nil;
            return false;
        }
        
        // IMPORTANT: Add output within beginConfiguration/commitConfiguration (OBS Studio approach)
        [m_captureSession addOutput:m_videoOutput];
        
        // IMPORTANT: Commit configuration changes atomically (OBS Studio approach)
        [m_captureSession commitConfiguration];
        
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
    // CRITICAL: If a format was selected via UI (setFormatById), do NOT override it
    // This prevents setFormat from overwriting the user's explicit format selection
    if (m_formatSelectedViaUI && !m_selectedFormatId.empty())
    {
        LOG_INFO("Format was selected via UI - ignoring setFormat() call to preserve user selection");
        LOG_INFO("Selected format ID: " + m_selectedFormatId);
        return true; // Return success to avoid errors, but don't change the format
    }
    
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
                
                // IMPORTANT: OBS Studio approach - prefer native formats (NV12/YUY2)
                AVCaptureDeviceFormat* exactFormatNative = nil;
                
                for (AVCaptureDeviceFormat* format in formats)
                {
                    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
                    
                    // Verificar formato exato
                    if ((int32_t)dims.width == (int32_t)width && (int32_t)dims.height == (int32_t)height)
                    {
                        // Check pixel format - prefer native formats (NV12, YUY2)
                        CMFormatDescriptionRef formatDesc = [format formatDescription];
                        FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(formatDesc);
                        
                        // IMPORTANT: OBS Studio approach - prefer native formats
                        // NV12 (420v) = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange (0x34323076)
                        // YUY2 (yuvs) = kCVPixelFormatType_422YpCbCr8_yuvs (0x79757673)
                        // Also check for other common YUV formats
                        if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                            pixelFormat == kCVPixelFormatType_422YpCbCr8_yuvs ||
                            pixelFormat == kCVPixelFormatType_422YpCbCr8 ||
                            pixelFormat == 0x79757673 || // 'yuvs' (YUY2)
                            pixelFormat == 0x34323076)   // '420v' (NV12)
                        {
                            exactFormatNative = format; // Prefer native format
                        }
                        else if (!exactFormat)
                        {
                            exactFormat = format; // Fallback to any matching format
                        }
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
                
                // Use native format if found, otherwise use any matching format
                if (exactFormatNative)
                {
                    exactFormat = exactFormatNative;
                }
                
                if (exactFormat)
                {
                    m_captureDevice.activeFormat = exactFormat;
    m_width = width;
    m_height = height;
                    
                    // Log pixel format for debugging
                    CMFormatDescriptionRef formatDesc = [exactFormat formatDescription];
                    FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(formatDesc);
                    LOG_INFO("Formato exato encontrado: " + std::to_string(m_width) + "x" + std::to_string(m_height) + 
                             " (pixel format: 0x" + std::to_string(pixelFormat) + ")");
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
                
                // IMPORTANT: Configure framerate on DEVICE BEFORE unlocking
                // This ensures format and framerate are configured together atomically
                // The framerate MUST be set on the device while it's still locked
                if (m_width > 0 && m_height > 0 && m_fps > 0)
                {
                    NSArray* frameRateRanges = [m_captureDevice.activeFormat videoSupportedFrameRateRanges];
                    if (frameRateRanges && [frameRateRanges count] > 0)
                    {
                        // Find best framerate (requested or max supported)
                        uint32_t targetFps = m_fps;
                        bool foundRequestedFps = false;
                        double maxFps = 0.0;
                        
                        for (AVFrameRateRange* range in frameRateRanges)
                        {
                            if (range.maxFrameRate > maxFps)
                            {
                                maxFps = range.maxFrameRate;
                            }
                            
                            // Check if requested fps is supported
                            if (m_fps >= range.minFrameRate && m_fps <= range.maxFrameRate)
                            {
                                targetFps = m_fps;
                                foundRequestedFps = true;
                                break;
                            }
                        }
                        
                        // If requested fps not supported, use max
                        if (!foundRequestedFps && maxFps > 0.0)
                        {
                            targetFps = static_cast<uint32_t>(maxFps);
                        }
                        
                        // Configure framerate on device (while still locked)
                        CMTimeScale timescale = static_cast<CMTimeScale>(targetFps);
                        CMTime frameDuration = CMTimeMake(1, timescale);
                        m_captureDevice.activeVideoMinFrameDuration = frameDuration;
                        m_captureDevice.activeVideoMaxFrameDuration = frameDuration;
                        LOG_INFO("=== CONFIGURING FRAMERATE WITH FORMAT ===");
                        LOG_INFO("Format: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                        LOG_INFO("Device framerate configured: " + std::to_string(targetFps) + " fps (requested: " + 
                                 std::to_string(m_fps) + " fps, found requested: " + (foundRequestedFps ? "yes" : "no") + ")");
                    }
                }
                
                [m_captureDevice unlockForConfiguration];
                
                // IMPORTANT: OBS Studio approach - if session is already running, use beginConfiguration/commitConfiguration
                // WITHOUT removing inputs/outputs. Just reconfigure the format directly.
                if (m_captureSession && [m_captureSession isRunning])
                {
                    LOG_INFO("Session is running - reconfiguring format using OBS Studio approach");
                    
                    // IMPORTANT: Stop session BEFORE beginConfiguration
                    [m_captureSession stopRunning];
                    usleep(50000); // 50ms delay
                    
                    // IMPORTANT: OBS Studio approach - beginConfiguration BEFORE locking device
                    [m_captureSession beginConfiguration];
                    
                    // IMPORTANT: OBS Studio approach - lock device and reconfigure format/framerate
                    // WITHOUT removing inputs/outputs
                    NSError* configError = nil;
                    if ([m_captureDevice lockForConfiguration:&configError])
                    {
                        // Format was already set above, but we need to ensure framerate is also set
                        if (m_fps > 0)
                        {
                            NSArray* frameRateRanges = [m_captureDevice.activeFormat videoSupportedFrameRateRanges];
                            if (frameRateRanges && [frameRateRanges count] > 0)
                            {
                                uint32_t targetFps = m_fps;
                                bool foundRequestedFps = false;
                                double maxFps = 0.0;
                                
                                for (AVFrameRateRange* range in frameRateRanges)
                                {
                                    if (range.maxFrameRate > maxFps)
                                    {
                                        maxFps = range.maxFrameRate;
                                    }
                                    if (m_fps >= range.minFrameRate && m_fps <= range.maxFrameRate)
                                    {
                                        targetFps = m_fps;
                                        foundRequestedFps = true;
                                        break;
                                    }
                                }
                                
                                if (!foundRequestedFps && maxFps > 0.0)
                                {
                                    targetFps = static_cast<uint32_t>(maxFps);
                                }
                                
                                // OBS Studio approach: set framerate directly on device
                                CMTimeScale timescale = static_cast<CMTimeScale>(targetFps);
                                CMTime frameDuration = CMTimeMake(1, timescale);
                                m_captureDevice.activeVideoMinFrameDuration = frameDuration;
                                m_captureDevice.activeVideoMaxFrameDuration = frameDuration;
                            }
                        }
                        
                        [m_captureDevice unlockForConfiguration];
                    }
                    
                    // IMPORTANT: Commit configuration changes atomically (OBS Studio approach)
                    [m_captureSession commitConfiguration];
                    
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
    // Store requested framerate
    m_fps = fps;
    
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
            // IMPORTANT: Do NOT stop/start the session here
            // The session should only be started/stopped in startCapture()/stopCapture()
            // This prevents AVFoundation from renegotiating the stream and falling back to defaults
            
            NSError* error = nil;
            if ([m_captureDevice lockForConfiguration:&error])
            {
                // Log framerate ranges suportados para diagnóstico
                NSArray* frameRateRanges = [m_captureDevice.activeFormat videoSupportedFrameRateRanges];
                LOG_INFO("=== FRAMERATE CONFIGURATION ===");
                LOG_INFO("Requested framerate: " + std::to_string(fps) + " fps");
                LOG_INFO("Supported framerate ranges for current format:");
                for (AVFrameRateRange* range in frameRateRanges)
                {
                    LOG_INFO("  " + std::to_string(range.minFrameRate) + " - " + 
                             std::to_string(range.maxFrameRate) + " fps");
                }
                
                // Configurar framerate no device
                AVFrameRateRange* bestRange = nil;
                for (AVFrameRateRange* range in frameRateRanges)
                {
                    if (fps >= range.minFrameRate && fps <= range.maxFrameRate)
                    {
                        bestRange = range;
                        break;
                    }
                }
                
                if (bestRange)
                {
                    // IMPORTANT: Use high timescale to avoid rounding issues
                    // For exact fps like 30, use CMTime(value: 1, timescale: 30)
                    CMTimeScale timescale = static_cast<CMTimeScale>(fps);
                    CMTime frameDuration = CMTimeMake(1, timescale);
                    
                    m_captureDevice.activeVideoMinFrameDuration = frameDuration;
                    m_captureDevice.activeVideoMaxFrameDuration = frameDuration;
                    LOG_INFO("Device framerate configured: " + std::to_string(fps) + " fps (timescale: " + std::to_string(timescale) + ")");
                    
                    // Verificar se foi aplicado corretamente
                    CMTime actualMinDuration = m_captureDevice.activeVideoMinFrameDuration;
                    CMTime actualMaxDuration = m_captureDevice.activeVideoMaxFrameDuration;
                    double actualMinFps = 1.0 / (CMTimeGetSeconds(actualMinDuration));
                    double actualMaxFps = 1.0 / (CMTimeGetSeconds(actualMaxDuration));
                    LOG_INFO("Actual device framerate range: " + std::to_string(actualMinFps) + " - " + 
                             std::to_string(actualMaxFps) + " fps");
                    
                    if (std::abs(actualMinFps - fps) > 0.1 || std::abs(actualMaxFps - fps) > 0.1)
                    {
                        LOG_WARN("Device framerate may not have been applied correctly!");
                        LOG_WARN("Requested: " + std::to_string(fps) + " fps");
                        LOG_WARN("Actual: " + std::to_string(actualMinFps) + " - " + std::to_string(actualMaxFps) + " fps");
                    }
                }
                else
                {
                    LOG_WARN("Framerate " + std::to_string(fps) + " fps não suportado pelo formato atual");
                    LOG_WARN("Usando padrão do dispositivo");
                    
                    // Tentar usar o framerate máximo suportado mais próximo
                    double bestFps = 0.0;
                    for (AVFrameRateRange* range in frameRateRanges)
                    {
                        if (range.maxFrameRate > bestFps)
                        {
                            bestFps = range.maxFrameRate;
                        }
                    }
                    
                    if (bestFps > 0.0)
                    {
                        uint32_t targetFps = static_cast<uint32_t>(bestFps);
                        CMTimeScale timescale = static_cast<CMTimeScale>(targetFps);
                        CMTime frameDuration = CMTimeMake(1, timescale);
                        m_captureDevice.activeVideoMinFrameDuration = frameDuration;
                        m_captureDevice.activeVideoMaxFrameDuration = frameDuration;
                        LOG_INFO("Usando framerate máximo suportado: " + std::to_string(targetFps) + " fps");
                    }
                }
                
                LOG_INFO("==============================");
                
                [m_captureDevice unlockForConfiguration];
                
                // IMPORTANT: Also configure framerate on the connection
                // Many devices (especially UVC capture cards) require framerate to be set on the connection
                // in addition to the device, otherwise the effective framerate may be stuck
                if (m_videoOutput)
                {
                    AVCaptureConnection* conn = [m_videoOutput connectionWithMediaType:AVMediaTypeVideo];
                    if (conn && [conn respondsToSelector:@selector(setVideoMinFrameDuration:)])
                    {
                        uint32_t targetFps = fps;
                        if (bestRange)
                        {
                            targetFps = fps;
                        }
                        else
                        {
                            // Use max supported if requested not supported
                            double bestFps = 0.0;
                            for (AVFrameRateRange* range in frameRateRanges)
                            {
                                if (range.maxFrameRate > bestFps)
                                {
                                    bestFps = range.maxFrameRate;
                                }
                            }
                            if (bestFps > 0.0)
                            {
                                targetFps = static_cast<uint32_t>(bestFps);
                            }
                        }
                        
                        CMTimeScale timescale = static_cast<CMTimeScale>(targetFps);
                        CMTime frameDuration = CMTimeMake(1, timescale);
                        conn.videoMinFrameDuration = frameDuration;
                        conn.videoMaxFrameDuration = frameDuration;
                        LOG_INFO("Connection framerate configured: " + std::to_string(targetFps) + " fps");
                    }
                }
            }
            else
            {
                LOG_ERROR("Falha ao bloquear dispositivo para configuração de framerate: " + 
                         std::string([[error localizedDescription] UTF8String]));
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
    CVPixelBufferRef pb = nullptr;
    
    // IMPORTANT: Get the buffer pointer quickly (short lock)
    // Do NOT hold the lock during conversion - this blocks the delegate!
    {
    std::lock_guard<std::mutex> lock(m_bufferMutex);
        pb = m_latestPixelBuffer;
        if (pb)
        {
            CVPixelBufferRetain(pb);
        }
    }
    
    // Convert outside the lock (doesn't block the delegate)
    bool ok = false;
    if (pb)
    {
        // Measure conversion time to diagnose performance issues
        auto startTime = std::chrono::high_resolution_clock::now();
        ok = convertPixelBufferToFrame(pb, frame);
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        // Log conversion time (only occasionally to avoid spam)
        static int conversionLogCount = 0;
        if (conversionLogCount++ % 150 == 0) // Log every 150 frames (~5 seconds at 30fps)
        {
            LOG_INFO("=== CONVERSION PERFORMANCE ===");
            LOG_INFO("BGRA→YUYV conversion time: " + std::to_string(duration) + " ms");
            LOG_INFO("Frame size: " + std::to_string(m_width) + "x" + std::to_string(m_height));
            if (duration > 33) // More than 1 frame at 30fps
            {
                LOG_WARN("Conversion is taking too long! This will limit framerate.");
                LOG_WARN("At " + std::to_string(duration) + " ms per frame, max FPS is ~" + 
                         std::to_string(1000 / duration) + " fps");
            }
            LOG_INFO("=============================");
        }
        
        CVPixelBufferRelease(pb);
    }
    return ok;
#else
    return false;
#endif
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
            
            if (controlName == "Brightness" || controlName == "brightness")
            {
                // Brightness não é diretamente controlável via AVFoundation no macOS
                // Alguns dispositivos podem ter extensões customizadas, mas não há API padrão
                success = false;
            }
            else if (controlName == "Contrast" || controlName == "contrast")
            {
                // Contrast não é diretamente controlável via AVFoundation no macOS
                success = false;
            }
            else if (controlName == "Saturation" || controlName == "saturation")
            {
                // Saturation não é diretamente controlável via AVFoundation no macOS
                success = false;
            }
            else if (controlName == "Hue" || controlName == "hue")
            {
                // Hue não é diretamente controlável via AVFoundation no macOS
                success = false;
            }
            else if (controlName == "Gain" || controlName == "gain")
            {
                // Gain pode estar disponível através de ISO em alguns dispositivos
                // Mas não há API direta para gain no AVFoundation
                success = false;
            }
            else if (controlName == "Exposure" || controlName == "exposure")
            {
                // Exposure controls não estão disponíveis no macOS via AVFoundation
                // Apenas iOS/iPadOS suportam controle manual de exposição
                success = false;
            }
            else if (controlName == "Sharpness" || controlName == "sharpness")
            {
                // Sharpness não é diretamente controlável via AVFoundation no macOS
                success = false;
            }
            else if (controlName == "Gamma" || controlName == "gamma")
            {
                // Gamma não é diretamente controlável via AVFoundation no macOS
                success = false;
            }
            else if (controlName == "White Balance" || controlName == "white balance" || controlName == "WhiteBalance")
            {
                // White Balance controls are NOT available on macOS via AVFoundation
                // These APIs are only available on iOS/iPadOS
                // macOS AVFoundation does not expose white balance controls
                LOG_WARN("White Balance control is not available on macOS via AVFoundation");
                success = false;
            }
            
            [m_captureDevice unlockForConfiguration];
            return success;
        }
        else if (error)
        {
            LOG_ERROR("Failed to lock device for configuration: " + std::string([[error localizedDescription] UTF8String]));
        }
    }
#endif
    
    return false;
}

bool VideoCaptureAVFoundation::getControl(const std::string &controlName, int32_t &value)
{
#ifdef __APPLE__
    if (!m_captureDevice)
    {
        return false;
    }
    
    @autoreleasepool {
        if (controlName == "White Balance" || controlName == "white balance" || controlName == "WhiteBalance")
        {
            // White Balance controls are NOT available on macOS via AVFoundation
            // Return default value for UI consistency
            value = 4000; // Default temperature
            return false; // Indicate control is not available
        }
        // Outros controles não estão disponíveis no AVFoundation
    }
#endif
    
    return false;
}

bool VideoCaptureAVFoundation::getControlMin(const std::string &controlName, int32_t &minValue)
{
#ifdef __APPLE__
    if (!m_captureDevice)
    {
        return false;
    }
    
    @autoreleasepool {
        if (controlName == "White Balance" || controlName == "white balance" || controlName == "WhiteBalance")
        {
            // Range típico de temperatura de white balance
            minValue = 2800;
            return true;
        }
        // Para outros controles, retornar valores padrão da UI
        else if (controlName == "Brightness" || controlName == "brightness" ||
                 controlName == "Contrast" || controlName == "contrast" ||
                 controlName == "Saturation" || controlName == "saturation" ||
                 controlName == "Hue" || controlName == "hue")
        {
            minValue = -100;
            return true;
        }
        else if (controlName == "Gain" || controlName == "gain")
        {
            minValue = 0;
            return true;
        }
        else if (controlName == "Exposure" || controlName == "exposure")
        {
            minValue = -13;
            return true;
        }
        else if (controlName == "Sharpness" || controlName == "sharpness")
        {
            minValue = 0;
            return true;
        }
        else if (controlName == "Gamma" || controlName == "gamma")
        {
            minValue = 100;
            return true;
        }
    }
#endif
    
    return false;
}

bool VideoCaptureAVFoundation::getControlMax(const std::string &controlName, int32_t &maxValue)
{
#ifdef __APPLE__
    if (!m_captureDevice)
    {
        return false;
    }
    
    @autoreleasepool {
        if (controlName == "White Balance" || controlName == "white balance" || controlName == "WhiteBalance")
        {
            // Range típico de temperatura de white balance
            maxValue = 6500;
            return true;
        }
        // Para outros controles, retornar valores padrão da UI
        else if (controlName == "Brightness" || controlName == "brightness" ||
                 controlName == "Contrast" || controlName == "contrast" ||
                 controlName == "Saturation" || controlName == "saturation" ||
                 controlName == "Hue" || controlName == "hue")
        {
            maxValue = 100;
            return true;
        }
        else if (controlName == "Gain" || controlName == "gain")
        {
            maxValue = 100;
            return true;
        }
        else if (controlName == "Exposure" || controlName == "exposure")
        {
            maxValue = 1;
            return true;
        }
        else if (controlName == "Sharpness" || controlName == "sharpness")
        {
            maxValue = 6;
            return true;
        }
        else if (controlName == "Gamma" || controlName == "gamma")
        {
            maxValue = 300;
            return true;
        }
    }
#endif
    
    return false;
}

bool VideoCaptureAVFoundation::getControlDefault(const std::string &controlName, int32_t &defaultValue)
{
#ifdef __APPLE__
    if (!m_captureDevice)
    {
        return false;
    }
    
    @autoreleasepool {
        if (controlName == "White Balance" || controlName == "white balance" || controlName == "WhiteBalance")
        {
            defaultValue = 4000; // Temperatura padrão
            return true;
        }
        // Para outros controles, retornar valores padrão da UI
        else if (controlName == "Brightness" || controlName == "brightness" ||
                 controlName == "Contrast" || controlName == "contrast" ||
                 controlName == "Saturation" || controlName == "saturation" ||
                 controlName == "Hue" || controlName == "hue")
        {
            defaultValue = 0;
            return true;
        }
        else if (controlName == "Gain" || controlName == "gain")
        {
            defaultValue = 0;
            return true;
        }
        else if (controlName == "Exposure" || controlName == "exposure")
        {
            defaultValue = 0;
            return true;
        }
        else if (controlName == "Sharpness" || controlName == "sharpness")
        {
            defaultValue = 0;
            return true;
        }
        else if (controlName == "Gamma" || controlName == "gamma")
        {
            defaultValue = 100;
            return true;
        }
    }
#endif
    
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
                
                // CRITICAL: Do NOT set sessionPreset - leave it as default
                // Even Low preset can interfere with activeFormat on some devices
                // Just log if a preset is found (shouldn't happen, but log for debugging)
                if (![preset isEqualToString:@""] && preset != nil)
                {
                    LOG_WARN("Session preset is set to: " + std::string([preset UTF8String]) + 
                             " - this may override activeFormat!");
                    LOG_WARN("Note: We are NOT setting any preset to allow manual format control");
                    // Do NOT change the preset - just log it
                }
                
                // CRITICAL: Verify format matches requested dimensions BEFORE starting
                // If format doesn't match, try to fix it using beginConfiguration/commitConfiguration
                if (m_width > 0 && m_height > 0)
                {
                    if ((int32_t)activeDims.width != (int32_t)m_width || (int32_t)activeDims.height != (int32_t)m_height)
                    {
                        LOG_WARN("ActiveFormat dimensions (" + std::to_string(activeDims.width) + "x" + std::to_string(activeDims.height) + 
                                 ") differ from requested (" + std::to_string(m_width) + "x" + std::to_string(m_height) + 
                                 ") - attempting to fix before starting session...");
                        
                        // IMPORTANT: Fix format BEFORE starting session (OBS Studio approach)
                        // CRITICAL: Do NOT set sessionPreset - leave it as default
                        [m_captureSession beginConfiguration];
                        
                        NSError* fixError = nil;
                        if ([m_captureDevice lockForConfiguration:&fixError])
                        {
                            // IMPORTANT: OBS Studio approach - prefer native formats (NV12/YUY2)
                            AVCaptureDeviceFormat* exactFormat = nil;
                            AVCaptureDeviceFormat* exactFormatNative = nil;
                            
                            for (AVCaptureDeviceFormat* format in [m_captureDevice formats])
                            {
                                CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
                                if (dims.width == static_cast<int32_t>(m_width) && 
                                    dims.height == static_cast<int32_t>(m_height))
                                {
                                    // Check pixel format - prefer native formats (NV12, YUY2)
                                    CMFormatDescriptionRef formatDesc = [format formatDescription];
                                    FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(formatDesc);
                                    
                                    // IMPORTANT: OBS Studio approach - prefer native formats
                                    // NV12 (420v) = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                    // YUY2 (yuvs) = kCVPixelFormatType_422YpCbCr8_yuvs
                                    if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                                        pixelFormat == kCVPixelFormatType_422YpCbCr8_yuvs ||
                                        pixelFormat == kCVPixelFormatType_422YpCbCr8 ||
                                        pixelFormat == 0x79757673 || // 'yuvs' (YUY2)
                                        pixelFormat == 0x34323076)   // '420v' (NV12)
                                    {
                                        exactFormatNative = format;
                                    }
                                    else if (!exactFormat)
                                    {
                                        exactFormat = format;
                                    }
                                }
                            }
                            
                            if (exactFormatNative)
                            {
                                exactFormat = exactFormatNative;
                            }
                            
                            if (exactFormat)
                            {
                                m_captureDevice.activeFormat = exactFormat;
                                LOG_INFO("Fixed activeFormat to: " + std::to_string(m_width) + "x" + std::to_string(m_height) + " before starting");
                                
                                // Also reconfigure framerate if needed
                                if (m_fps > 0)
                                {
                                    NSArray* frameRateRanges = [exactFormat videoSupportedFrameRateRanges];
                                    if (frameRateRanges && [frameRateRanges count] > 0)
                                    {
                                        uint32_t targetFps = m_fps;
                                        bool foundRequestedFps = false;
                                        double maxFps = 0.0;
                                        
                                        for (AVFrameRateRange* range in frameRateRanges)
                                        {
                                            if (range.maxFrameRate > maxFps)
                                            {
                                                maxFps = range.maxFrameRate;
                                            }
                                            if (m_fps >= range.minFrameRate && m_fps <= range.maxFrameRate)
                                            {
                                                targetFps = m_fps;
                                                foundRequestedFps = true;
                                                break;
                                            }
                                        }
                                        
                                        if (!foundRequestedFps && maxFps > 0.0)
                                        {
                                            targetFps = static_cast<uint32_t>(maxFps);
                                        }
                                        
                                        CMTimeScale timescale = static_cast<CMTimeScale>(targetFps);
                                        CMTime frameDuration = CMTimeMake(1, timescale);
                                        m_captureDevice.activeVideoMinFrameDuration = frameDuration;
                                        m_captureDevice.activeVideoMaxFrameDuration = frameDuration;
                                    }
                                }
                            }
                            else
                            {
                                LOG_WARN("Could not find exact format " + std::to_string(m_width) + "x" + std::to_string(m_height) + 
                                         " - format may not be supported");
                            }
                            
                            [m_captureDevice unlockForConfiguration];
                        }
                        
                        [m_captureSession commitConfiguration];
                        
                        // Verify again after fix
                        CMVideoDimensions activeDimsAfterFix = CMVideoFormatDescriptionGetDimensions([m_captureDevice.activeFormat formatDescription]);
                        if (activeDimsAfterFix.width == (int32_t)m_width && activeDimsAfterFix.height == (int32_t)m_height)
                        {
                            LOG_INFO("Format fixed successfully - ActiveFormat now matches requested: " + 
                                     std::to_string(m_width) + "x" + std::to_string(m_height));
                        }
                        else
                        {
                            LOG_WARN("Format fix failed - ActiveFormat is still: " + 
                                     std::to_string(activeDimsAfterFix.width) + "x" + std::to_string(activeDimsAfterFix.height) + 
                                     " (requested: " + std::to_string(m_width) + "x" + std::to_string(m_height) + ")");
                            LOG_WARN("Aspect ratio will use requested dimensions");
                        }
                    }
                    else
                    {
                        LOG_INFO("ActiveFormat matches requested dimensions - format should be respected");
                    }
                }
            }
            
            // IMPORTANT: Ensure framerate is configured on connection before starting
            // This MUST be done AFTER the output is added to the session
            // and BEFORE starting the session, with the device locked
            if (m_videoOutput && m_fps > 0)
            {
                AVCaptureConnection* conn = [m_videoOutput connectionWithMediaType:AVMediaTypeVideo];
                if (conn && [conn respondsToSelector:@selector(setVideoMinFrameDuration:)])
                {
                    // Find best framerate for current format
                    uint32_t targetFps = m_fps;
                    NSArray* frameRateRanges = [m_captureDevice.activeFormat videoSupportedFrameRateRanges];
                    if (frameRateRanges && [frameRateRanges count] > 0)
                    {
                        bool foundRequestedFps = false;
                        double maxFps = 0.0;
                        
                        for (AVFrameRateRange* range in frameRateRanges)
                        {
                            if (range.maxFrameRate > maxFps)
                            {
                                maxFps = range.maxFrameRate;
                            }
                            if (m_fps >= range.minFrameRate && m_fps <= range.maxFrameRate)
                            {
                                targetFps = m_fps;
                                foundRequestedFps = true;
                                break;
                            }
                        }
                        
                        if (!foundRequestedFps && maxFps > 0.0)
                        {
                            targetFps = static_cast<uint32_t>(maxFps);
                        }
                    }
                    
                    // IMPORTANT: Lock the device before configuring connection framerate
                    // This ensures the framerate is set atomically with the device configuration
                    // OBS and other professional software do this
                    NSError* error = nil;
                    if ([m_captureDevice lockForConfiguration:&error])
                    {
                        CMTimeScale timescale = static_cast<CMTimeScale>(targetFps);
                        CMTime frameDuration = CMTimeMake(1, timescale);
                        conn.videoMinFrameDuration = frameDuration;
                        conn.videoMaxFrameDuration = frameDuration;
                        [m_captureDevice unlockForConfiguration];
                        
                        // Verify connection framerate
                        CMTime connMin = conn.videoMinFrameDuration;
                        CMTime connMax = conn.videoMaxFrameDuration;
                        double connMinFps = 1.0 / CMTimeGetSeconds(connMin);
                        double connMaxFps = 1.0 / CMTimeGetSeconds(connMax);
                        
                        LOG_INFO("=== FRAMERATE BEFORE START ===");
                        LOG_INFO("Requested: " + std::to_string(m_fps) + " fps");
                        LOG_INFO("Connection configured: " + std::to_string(targetFps) + " fps");
                        LOG_INFO("Connection actual: " + std::to_string(connMinFps) + " - " + std::to_string(connMaxFps) + " fps");
                        
                        // Also check device framerate
                        CMTime devMin = m_captureDevice.activeVideoMinFrameDuration;
                        CMTime devMax = m_captureDevice.activeVideoMaxFrameDuration;
                        double devMinFps = 1.0 / CMTimeGetSeconds(devMin);
                        double devMaxFps = 1.0 / CMTimeGetSeconds(devMax);
                        LOG_INFO("Device framerate: " + std::to_string(devMinFps) + " - " + std::to_string(devMaxFps) + " fps");
                        LOG_INFO("==============================");
                    }
                    else
                    {
                        LOG_WARN("Could not lock device to configure connection framerate: " + 
                                 std::string([[error localizedDescription] UTF8String]));
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
                
                // IMPORTANT: Verify framerate after starting session
                if (m_videoOutput)
                {
                    AVCaptureConnection* conn = [m_videoOutput connectionWithMediaType:AVMediaTypeVideo];
                    if (conn)
                    {
                        CMTime connMin = conn.videoMinFrameDuration;
                        CMTime connMax = conn.videoMaxFrameDuration;
                        double connMinFps = 1.0 / CMTimeGetSeconds(connMin);
                        double connMaxFps = 1.0 / CMTimeGetSeconds(connMax);
                        LOG_INFO("Connection framerate after start: " + std::to_string(connMinFps) + " - " + std::to_string(connMaxFps) + " fps");
                    }
                    
                    CMTime devMin = m_captureDevice.activeVideoMinFrameDuration;
                    CMTime devMax = m_captureDevice.activeVideoMaxFrameDuration;
                    double devMinFps = 1.0 / CMTimeGetSeconds(devMin);
                    double devMaxFps = 1.0 / CMTimeGetSeconds(devMax);
                    LOG_INFO("Device framerate after start: " + std::to_string(devMinFps) + " - " + std::to_string(devMaxFps) + " fps");
                }
                
                // Se o formato mudou após iniciar, tentar corrigir usando a abordagem do OBS Studio
                // OBS Studio: NÃO remove inputs/outputs, apenas reconfigura o formato diretamente
                // dentro de beginConfiguration/commitConfiguration com o device locked
                if (activeDimsAfter.width != (int32_t)m_width || activeDimsAfter.height != (int32_t)m_height)
                {
                    LOG_WARN("ActiveFormat changed after starting session!");
                    LOG_WARN("Requested: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                    LOG_WARN("Actual: " + std::to_string(activeDimsAfter.width) + "x" + std::to_string(activeDimsAfter.height));
                    LOG_WARN("Attempting to fix using OBS Studio approach: reconfiguring format directly...");
                    
                    // IMPORTANT: Stop session BEFORE beginConfiguration
                    // stopRunning cannot be called between beginConfiguration and commitConfiguration
                    [m_captureSession stopRunning];
                    usleep(50000); // 50ms delay
                    
                    // IMPORTANT: Use beginConfiguration/commitConfiguration for atomic changes (OBS Studio approach)
                    [m_captureSession beginConfiguration];
                    
                    // IMPORTANT: Ensure sessionPreset is not interfering
                    // CRITICAL: Do NOT set sessionPreset - leave it as default
                    // Setting any preset can interfere with activeFormat
                    
                    // IMPORTANT: OBS Studio approach - configure format directly WITHOUT removing inputs/outputs
                    // Lock device and configure format/framerate within the configuration block
                    NSError* configError = nil;
                    if ([m_captureDevice lockForConfiguration:&configError])
                    {
                        // IMPORTANT: OBS Studio approach - prefer native formats (NV12/YUY2)
                        AVCaptureDeviceFormat* exactFormat = nil;
                        AVCaptureDeviceFormat* exactFormatNative = nil;
                        
                        for (AVCaptureDeviceFormat* format in [m_captureDevice formats])
                        {
                            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
                            if (dims.width == static_cast<int32_t>(m_width) && 
                                dims.height == static_cast<int32_t>(m_height))
                            {
                                // Check pixel format - prefer native formats (NV12, YUY2)
                                CMFormatDescriptionRef formatDesc = [format formatDescription];
                                FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(formatDesc);
                                
                                // IMPORTANT: OBS Studio approach - prefer native formats
                                // NV12 (420v) = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                // YUY2 (yuvs) = kCVPixelFormatType_422YpCbCr8_yuvs
                                if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                                    pixelFormat == kCVPixelFormatType_422YpCbCr8_yuvs ||
                                    pixelFormat == kCVPixelFormatType_422YpCbCr8 ||
                                    pixelFormat == 0x79757673 || // 'yuvs' (YUY2)
                                    pixelFormat == 0x34323076)   // '420v' (NV12)
                                {
                                    exactFormatNative = format;
                                }
                                else if (!exactFormat)
                                {
                                    exactFormat = format;
                                }
                            }
                        }
                        
                        if (exactFormatNative)
                        {
                            exactFormat = exactFormatNative;
                        }
                        
                        if (exactFormat)
                        {
                            // OBS Studio approach: set format and framerate directly, atomically
                            m_captureDevice.activeFormat = exactFormat;
                            LOG_INFO("Reconfigured activeFormat to: " + std::to_string(m_width) + "x" + std::to_string(m_height));
                            
                            // IMPORTANT: Reconfigure framerate after format change (OBS Studio approach)
                            NSArray* frameRateRanges = [exactFormat videoSupportedFrameRateRanges];
                            if (frameRateRanges && [frameRateRanges count] > 0 && m_fps > 0)
                            {
                                uint32_t targetFps = m_fps;
                                double maxFps = 0.0;
                                bool foundRequestedFps = false;
                                
                                for (AVFrameRateRange* range in frameRateRanges)
                                {
                                    if (range.maxFrameRate > maxFps)
                                    {
                                        maxFps = range.maxFrameRate;
                                    }
                                    if (m_fps >= range.minFrameRate && m_fps <= range.maxFrameRate)
                                    {
                                        targetFps = m_fps;
                                        foundRequestedFps = true;
                                        break;
                                    }
                                }
                                
                                if (!foundRequestedFps && maxFps > 0.0)
                                {
                                    targetFps = static_cast<uint32_t>(maxFps);
                                }
                                
                                // OBS Studio approach: set framerate directly on device
                                CMTimeScale timescale = static_cast<CMTimeScale>(targetFps);
                                CMTime frameDuration = CMTimeMake(1, timescale);
                                m_captureDevice.activeVideoMinFrameDuration = frameDuration;
                                m_captureDevice.activeVideoMaxFrameDuration = frameDuration;
                                
                                LOG_INFO("Reconfigured framerate to: " + std::to_string(targetFps) + " fps (requested: " + 
                                         std::to_string(m_fps) + " fps, found requested: " + (foundRequestedFps ? "yes" : "no") + ")");
                            }
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
                    
                    // IMPORTANT: Commit configuration changes atomically (OBS Studio approach)
                    [m_captureSession commitConfiguration];
                    
                    // Reiniciar a sessão
                    usleep(100000); // 100ms delay - importante para o formato ser aplicado
                    [m_captureSession startRunning];
                    
                    // Verificar novamente após reiniciar
                    usleep(100000); // 100ms delay
                    CMVideoDimensions activeDimsFinal = CMVideoFormatDescriptionGetDimensions([m_captureDevice.activeFormat formatDescription]);
                    LOG_INFO("=== AFTER RESTARTING SESSION ===");
                    LOG_INFO("ActiveFormat: " + std::to_string(activeDimsFinal.width) + "x" + std::to_string(activeDimsFinal.height));
                    
                    // Verificar se o formato ainda não está correto
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
    
    // OPTIMIZED: Converter BGRA para YUYV usando inteiros fixos para melhor performance
    // Usar multiplicadores inteiros em vez de floats para evitar conversões custosas
    // Y = (77*R + 150*G + 29*B) >> 8  (aproximação de 0.299, 0.587, 0.114)
    // U = (-43*R - 85*G + 128*B) >> 8 + 128  (aproximação)
    // V = (128*R - 107*G - 21*B) >> 8 + 128  (aproximação)
    
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
            
            // Converter RGB para YUV usando inteiros (muito mais rápido que float)
            // Y = (77*R + 150*G + 29*B) >> 8
            int y1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8;
            int y2 = (77 * r2 + 150 * g2 + 29 * b2) >> 8;
            
            // U e V calculados a partir do primeiro pixel (YUYV compartilha U/V entre 2 pixels)
            // U = (-43*R - 85*G + 128*B) >> 8 + 128
            int u = ((-43 * r1 - 85 * g1 + 128 * b1) >> 8) + 128;
            // V = (128*R - 107*G - 21*B) >> 8 + 128
            int v = ((128 * r1 - 107 * g1 - 21 * b1) >> 8) + 128;
            
            // Clamp valores (mais rápido que std::max/min)
            y1 = (y1 < 0) ? 0 : ((y1 > 255) ? 255 : y1);
            y2 = (y2 < 0) ? 0 : ((y2 > 255) ? 255 : y2);
            u = (u < 0) ? 0 : ((u > 255) ? 255 : u);
            v = (v < 0) ? 0 : ((v > 255) ? 255 : v);
            
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

#ifdef __APPLE__
AVCaptureDevice* VideoCaptureAVFoundation::getCaptureDevice() const
{
    return m_captureDevice;
}

AVCaptureVideoDataOutput* VideoCaptureAVFoundation::getVideoOutput() const
{
    return m_videoOutput;
}

// Helper function to get pixel format name
static std::string getPixelFormatName(FourCharCode pixelFormat)
{
    if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        return "NV12 (420v)";
    else if (pixelFormat == kCVPixelFormatType_422YpCbCr8_yuvs)
        return "YUY2 (yuvs)";
    else if (pixelFormat == kCVPixelFormatType_32BGRA)
        return "BGRA";
    else if (pixelFormat == kCVPixelFormatType_422YpCbCr8)
        return "UYVY";
    else
        return "Unknown (0x" + std::to_string(pixelFormat) + ")";
}

// Helper function to get color space name
static std::string getColorSpaceName(CMFormatDescriptionRef formatDesc)
{
    CFStringRef colorPrimaries = (CFStringRef)CMFormatDescriptionGetExtension(formatDesc, kCMFormatDescriptionExtension_ColorPrimaries);
    if (colorPrimaries)
    {
        NSString* nsColorPrimaries = (__bridge NSString*)colorPrimaries;
        if ([nsColorPrimaries isEqualToString:(__bridge NSString*)kCMFormatDescriptionColorPrimaries_ITU_R_709_2])
            return "CS 709";
        else if ([nsColorPrimaries isEqualToString:@"ITU_R_601_4"] || 
                 [nsColorPrimaries isEqualToString:@"SMPTE_C"])
            return "CS 601";
    }
    return "CS Unknown";
}

// Helper function to format aspect ratio
static std::string formatAspectRatio(uint32_t width, uint32_t height)
{
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    if (std::abs(aspect - 16.0f/9.0f) < 0.01f)
        return "16:9";
    else if (std::abs(aspect - 4.0f/3.0f) < 0.01f)
        return "4:3";
    else if (std::abs(aspect - 5.0f/4.0f) < 0.01f)
        return "5:4";
    else
    {
        // Find GCD for aspect ratio
        uint32_t a = width, b = height;
        while (b != 0)
        {
            uint32_t temp = b;
            b = a % b;
            a = temp;
        }
        uint32_t gcd = a;
        return std::to_string(width / gcd) + ":" + std::to_string(height / gcd);
    }
}

std::vector<AVFoundationFormatInfo> VideoCaptureAVFoundation::listFormats(const std::string &deviceId)
{
    std::vector<AVFoundationFormatInfo> formats;
    
#ifdef __APPLE__
    @autoreleasepool {
        AVCaptureDevice* device = nil;
        
        // Find device by ID
        if (!deviceId.empty())
        {
            device = [AVCaptureDevice deviceWithUniqueID:[NSString stringWithUTF8String:deviceId.c_str()]];
        }
        else if (m_captureDevice)
        {
            device = m_captureDevice;
        }
        else
        {
            // Use default device
            device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        }
        
        if (!device)
        {
            LOG_WARN("Device not found for format enumeration: " + deviceId);
            return formats;
        }
        
        NSArray* deviceFormats = [device formats];
        LOG_INFO("Enumerating " + std::to_string([deviceFormats count]) + " formats for device: " + 
                 std::string([[device localizedName] UTF8String]));
        
        for (AVCaptureDeviceFormat* format in deviceFormats)
        {
            CMFormatDescriptionRef formatDesc = [format formatDescription];
            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(formatDesc);
            FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(formatDesc);
            
            AVFoundationFormatInfo info;
            info.width = dims.width;
            info.height = dims.height;
            
            // Get FPS ranges
            NSArray* frameRateRanges = [format videoSupportedFrameRateRanges];
            if (frameRateRanges && [frameRateRanges count] > 0)
            {
                AVFrameRateRange* firstRange = frameRateRanges[0];
                info.minFps = firstRange.minFrameRate;
                info.maxFps = firstRange.maxFrameRate;
                
                // Check if there are multiple ranges
                for (AVFrameRateRange* range in frameRateRanges)
                {
                    if (range.minFrameRate < info.minFps)
                        info.minFps = range.minFrameRate;
                    if (range.maxFrameRate > info.maxFps)
                        info.maxFps = range.maxFrameRate;
                }
            }
            
            info.pixelFormat = getPixelFormatName(pixelFormat);
            info.colorSpace = getColorSpaceName(formatDesc);
            
            // Create unique ID based on format properties (NOT object pointer)
            // This ensures the ID is stable across reloads
            // Format: widthxheight-pixelFormat-colorSpace-minFps-maxFps
            NSString* formatDescStr = [NSString stringWithFormat:@"%dx%d-%@-%@-%.2f-%.2f",
                dims.width, dims.height,
                info.pixelFormat.c_str() ? [NSString stringWithUTF8String:info.pixelFormat.c_str()] : @"Unknown",
                info.colorSpace.c_str() ? [NSString stringWithUTF8String:info.colorSpace.c_str()] : @"Unknown",
                info.minFps, info.maxFps];
            info.id = std::string([formatDescStr UTF8String]);
            
            // Create display name (similar to OBS Studio format)
            std::string aspectRatio = formatAspectRatio(info.width, info.height);
            std::string fpsStr;
            if (std::abs(info.minFps - info.maxFps) < 0.01f)
            {
                fpsStr = std::to_string(static_cast<int>(info.minFps)) + " FPS";
            }
            else
            {
                fpsStr = std::to_string(static_cast<int>(info.minFps)) + "-" + 
                        std::to_string(static_cast<int>(info.maxFps)) + " FPS";
            }
            
            info.displayName = std::to_string(info.width) + "x" + std::to_string(info.height) + 
                              " (" + aspectRatio + ") - " + fpsStr + " - " + 
                              info.colorSpace + " - " + info.pixelFormat;
            
            formats.push_back(info);
        }
        
        // Sort formats by resolution (largest first), then by pixel format preference
        std::sort(formats.begin(), formats.end(), [](const AVFoundationFormatInfo& a, const AVFoundationFormatInfo& b) {
            // First sort by total pixels (largest first)
            uint32_t aPixels = a.width * a.height;
            uint32_t bPixels = b.width * b.height;
            if (aPixels != bPixels)
                return aPixels > bPixels;
            
            // Then by pixel format preference (NV12/YUY2 first)
            bool aIsNative = (a.pixelFormat.find("NV12") != std::string::npos || 
                             a.pixelFormat.find("YUY2") != std::string::npos);
            bool bIsNative = (b.pixelFormat.find("NV12") != std::string::npos || 
                             b.pixelFormat.find("YUY2") != std::string::npos);
            if (aIsNative != bIsNative)
                return aIsNative;
            
            // Then by max FPS (highest first)
            return a.maxFps > b.maxFps;
        });
    }
#endif
    
    return formats;
}

bool VideoCaptureAVFoundation::setFormatByIndex(int formatIndex, const std::string &deviceId)
{
    auto formats = listFormats(deviceId);
    if (formatIndex < 0 || formatIndex >= static_cast<int>(formats.size()))
    {
        LOG_ERROR("Invalid format index: " + std::to_string(formatIndex) + " (available: 0-" + 
                 std::to_string(formats.size() - 1) + ")");
        return false;
    }
    
    return setFormatById(formats[formatIndex].id, deviceId);
}

bool VideoCaptureAVFoundation::setFormatById(const std::string &formatId, const std::string &deviceId)
{
#ifdef __APPLE__
    @autoreleasepool {
        AVCaptureDevice* device = nil;
        
        // Find device by ID
        if (!deviceId.empty())
        {
            device = [AVCaptureDevice deviceWithUniqueID:[NSString stringWithUTF8String:deviceId.c_str()]];
        }
        else if (m_captureDevice)
        {
            device = m_captureDevice;
        }
        else
        {
            LOG_ERROR("No device available for format selection");
            return false;
        }
        
        if (!device)
        {
            LOG_ERROR("Device not found: " + deviceId);
            return false;
        }
        
        // Parse format ID to find matching format
        // Format ID contains: width, height, pixel format, color space
        // We'll search for a format that matches these properties
        NSArray* deviceFormats = [device formats];
        AVCaptureDeviceFormat* matchingFormat = nil;
        
        for (AVCaptureDeviceFormat* format in deviceFormats)
        {
            CMFormatDescriptionRef formatDesc = [format formatDescription];
            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(formatDesc);
            FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(formatDesc);
            
            // Get FPS ranges (same logic as listFormats)
            NSArray* frameRateRanges = [format videoSupportedFrameRateRanges];
            float minFps = 0.0f;
            float maxFps = 0.0f;
            if (frameRateRanges && [frameRateRanges count] > 0)
            {
                AVFrameRateRange* firstRange = frameRateRanges[0];
                minFps = firstRange.minFrameRate;
                maxFps = firstRange.maxFrameRate;
                
                // Check if there are multiple ranges
                for (AVFrameRateRange* range in frameRateRanges)
                {
                    if (range.minFrameRate < minFps)
                        minFps = range.minFrameRate;
                    if (range.maxFrameRate > maxFps)
                        maxFps = range.maxFrameRate;
                }
            }
            
            // Generate format ID using same format as listFormats
            NSString* formatDescStr = [NSString stringWithFormat:@"%dx%d-%@-%@-%.2f-%.2f",
                dims.width, dims.height,
                [NSString stringWithUTF8String:getPixelFormatName(pixelFormat).c_str()],
                [NSString stringWithUTF8String:getColorSpaceName(formatDesc).c_str()],
                minFps, maxFps];
            std::string currentFormatId = std::string([formatDescStr UTF8String]);
            
            // Exact match on format ID
            if (currentFormatId == formatId)
            {
                matchingFormat = format;
                break;
            }
        }
        
        if (!matchingFormat)
        {
            LOG_ERROR("Format not found: " + formatId);
            return false;
        }
        
        // CRITICAL: Mark that a format was selected via UI
        // This prevents setFormat() from overwriting the user's selection
        m_formatSelectedViaUI = true;
        m_selectedFormatId = formatId;
        
        // Set the format
        CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([matchingFormat formatDescription]);
        m_width = dims.width;
        m_height = dims.height;
        
        // If device is open, apply format immediately with proper session reconfiguration
        if (m_isOpen && m_captureDevice == device)
        {
            // Use centralized method to apply format and framerate atomically
            return applyFormatAndFramerate(matchingFormat, m_fps, true);
        }
        else
        {
            // Format will be applied when device is opened
            LOG_INFO("Format will be applied when device is opened: " + std::to_string(m_width) + "x" + std::to_string(m_height));
            return true;
        }
    }
#else
    (void)formatId;
    (void)deviceId;
    return false;
#endif
}

// Centralized method to apply format and framerate atomically
// This ensures format and framerate are always configured together, preventing conflicts
bool VideoCaptureAVFoundation::applyFormatAndFramerate(AVCaptureDeviceFormat* format, uint32_t fps, bool stopSessionIfRunning)
{
#ifdef __APPLE__
    if (!format || !m_captureDevice || !m_captureSession)
    {
        LOG_ERROR("applyFormatAndFramerate: Invalid parameters");
        return false;
    }
    
    @autoreleasepool {
        // Stop session if requested and running
        bool wasRunning = [m_captureSession isRunning];
        if (stopSessionIfRunning && wasRunning)
        {
            [m_captureSession stopRunning];
            usleep(50000); // 50ms delay for session to stabilize
        }
        
        // CRITICAL: Use beginConfiguration/commitConfiguration for atomic changes
        [m_captureSession beginConfiguration];
        
        NSError* error = nil;
        if ([m_captureDevice lockForConfiguration:&error])
        {
            // Step 1: Set the format
            m_captureDevice.activeFormat = format;
            
            // Step 2: Get framerate ranges for the NEW format
            NSArray* frameRateRanges = [format videoSupportedFrameRateRanges];
            if (frameRateRanges && [frameRateRanges count] > 0)
            {
                // Find best framerate (requested if supported, otherwise max)
                AVFrameRateRange* bestRange = nil;
                float maxFps = 0.0f;
                float targetFps = static_cast<float>(fps);
                
                for (AVFrameRateRange* range in frameRateRanges)
                {
                    if (range.maxFrameRate > maxFps)
                    {
                        maxFps = range.maxFrameRate;
                        if (!bestRange)
                            bestRange = range;
                    }
                    
                    // If requested FPS is within this range, use it
                    if (fps > 0 && fps >= range.minFrameRate && fps <= range.maxFrameRate)
                    {
                        bestRange = range;
                        targetFps = static_cast<float>(fps);
                        break;
                    }
                }
                
                // If requested FPS not supported, use max
                if (fps > 0 && bestRange && (fps < bestRange.minFrameRate || fps > bestRange.maxFrameRate))
                {
                    targetFps = maxFps;
                }
                
                // Step 3: Configure framerate on DEVICE (while still locked)
                CMTimeScale timescale = static_cast<CMTimeScale>(targetFps);
                CMTime frameDuration = CMTimeMake(1, timescale);
                m_captureDevice.activeVideoMinFrameDuration = frameDuration;
                m_captureDevice.activeVideoMaxFrameDuration = frameDuration;
                
                LOG_INFO("=== APPLYING FORMAT AND FRAMERATE ATOMICALLY ===");
                CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
                LOG_INFO("Format: " + std::to_string(dims.width) + "x" + std::to_string(dims.height));
                LOG_INFO("Framerate: " + std::to_string(targetFps) + " fps (requested: " + std::to_string(fps) + 
                        ", supported: " + std::to_string(bestRange.minFrameRate) + " - " + 
                        std::to_string(bestRange.maxFrameRate) + " fps)");
                
                // Step 4: Configure framerate on CONNECTION (while device is still locked)
                if (m_videoOutput)
                {
                    AVCaptureConnection* conn = [m_videoOutput connectionWithMediaType:AVMediaTypeVideo];
                    if (conn)
                    {
                        conn.videoMinFrameDuration = frameDuration;
                        conn.videoMaxFrameDuration = frameDuration;
                        LOG_INFO("Connection framerate configured: " + std::to_string(targetFps) + " fps");
                    }
                }
                
                LOG_INFO("================================================");
            }
            
            [m_captureDevice unlockForConfiguration];
        }
        else
        {
            [m_captureSession commitConfiguration];
            LOG_ERROR("Failed to lock device: " + std::string([[error localizedDescription] UTF8String]));
            if (wasRunning)
            {
                [m_captureSession startRunning];
            }
            return false;
        }
        
        // CRITICAL: Commit all changes atomically
        [m_captureSession commitConfiguration];
        
        // Restart session if it was running
        if (wasRunning)
        {
            [m_captureSession startRunning];
        }
        
        // Update internal state
        CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions([format formatDescription]);
        m_width = dims.width;
        m_height = dims.height;
        
        // Verify what was actually applied
        CMVideoDimensions actualDims = CMVideoFormatDescriptionGetDimensions([m_captureDevice.activeFormat formatDescription]);
        FourCharCode actualPixelFormat = CMFormatDescriptionGetMediaSubType([m_captureDevice.activeFormat formatDescription]);
        LOG_INFO("Format applied: " + std::to_string(actualDims.width) + "x" + std::to_string(actualDims.height) + 
                " (pixel format: " + getPixelFormatName(actualPixelFormat) + ")");
        
        return true;
    }
#else
    (void)format;
    (void)fps;
    (void)stopSessionIfRunning;
    return false;
#endif
}
#endif
