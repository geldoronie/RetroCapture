#include "FrameCapturePipeline.h"
#include "Application.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"
#include "../capture/IVideoCapture.h"
#include "../capture/VideoCaptureFactory.h"
#include "../capture/VideoCaptureRemote.h"
#include "../capture/VideoCaptureScreen.h"
#include "../capture/VideoCaptureTestPattern.h"
#include "../streaming/RemoteMetaSync.h"
#include "../encoding/MediaEncoder.h"
#ifdef PLATFORM_LINUX
#include "../v4l2/V4L2ControlMapper.h"
#endif
// FrameProcessor and OpenGLRenderer work on all platforms
#include "../processing/FrameProcessor.h"
#include "../renderer/OpenGLRenderer.h"
#include "../renderer/PBOManager.h"
#ifdef USE_SDL2
#include "../output/WindowManagerSDL.h"
#else
#include "../output/WindowManager.h"
#endif
#include "../shader/ShaderEngine.h"
#include "../ui/UIManager.h"
#include "../osd/QuickActionsOverlay.h"
#include "../osd/OSDChat.h"
#include "../chat/ChatClient.h"
#include "../identity/ChatIdentity.h"
#include "../identity/OwnedRooms.h"
#if defined(__linux__)
#  include "../output/VirtualCameraOutput.h"
#elif defined(_WIN32)
#  include "../output/VirtualCameraOutputWin.h"
#elif defined(__APPLE__)
#  include "../output/VirtualCameraOutputMac.h"
#endif
#include "../tray/ISystemTray.h"
#include "../ui/UIRemoteConnection.h"
#include "../ui/UICapturePresets.h"
#include "../ui/UIRecordings.h"
#include "../renderer/glad_loader.h"
#include "../streaming/StreamManager.h"
#include "../streaming/DirectoryClient.h"
#include "../streaming/DirectoryBrowser.h"
#include "../ui/UIDirectoryBrowser.h"
#include "../streaming/CloudflaredManager.h"
#include "../utils/PasswordHash.h"

#ifndef RETROCAPTURE_VERSION
#define RETROCAPTURE_VERSION "0.0.0-dev"
#endif
#include "../streaming/HTTPTSStreamer.h"
#include "../audio/IAudioCapture.h"
#include "../audio/AudioCaptureFactory.h"
#ifdef __linux__
#include "../audio/AudioCapturePulse.h"
#endif
#include "../recording/RecordingManager.h"
#include "../recording/RecordingSettings.h"
#include "../recording/RecordingMetadata.h"
#include "../utils/PresetManager.h"
#include "../utils/ThumbnailGenerator.h"
#ifdef USE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_keyboard.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif
#ifdef PLATFORM_LINUX
#include <linux/videodev2.h>
#endif
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
#include <unistd.h>
#endif
#include "../utils/FilesystemCompat.h"
#include <time.h>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

FrameCapturePipeline::FrameCapturePipeline(Application &app) : m_app(app)
{
}

bool FrameCapturePipeline::renderAndDistributeFrame()
{
    // #152 - the per-frame render + shader + output-resolution + window draw +
    // capture-for-streaming/recording/virtcam pipeline, extracted verbatim from
    // run(). All state is members or locals born and consumed here; the capture
    // half is a candidate for a dedicated FrameCapturePipeline class (#157).
    // #107 — publish the live capture texture for the screen
    // region selector (it draws the current frame to pick on).
    if (m_app.m_ui)
    {
        m_app.m_ui->setCaptureTexture(m_app.m_frameProcessor->getTexture(),
                                m_app.m_frameProcessor->getTextureWidth(),
                                m_app.m_frameProcessor->getTextureHeight());
    }
    // Log resolução de captura original (antes de qualquer processamento)
    static int originalCaptureLogCount = 0;
    if (originalCaptureLogCount++ < 3)
    {
        LOG_DEBUG("=== ORIGINAL CAPTURE TEXTURE ===");
        LOG_DEBUG("Original capture texture: " + std::to_string(m_app.m_frameProcessor->getTexture()) +
                 ", Size: " + std::to_string(m_app.m_frameProcessor->getTextureWidth()) + "x" + 
                 std::to_string(m_app.m_frameProcessor->getTextureHeight()));
        LOG_DEBUG("================================");
    }
    
    // Apply shader if active
    GLuint textureToRender = m_app.m_frameProcessor->getTexture();
    bool isShaderTexture = false;

    // Master toggle: when the user disables the shader pipeline
    // from the Shader tab we keep the loaded preset / parameters
    // intact but stop running the chain on the live render. The
    // captured texture for stream/recording then reflects the
    // raw source — same path the user sees on the window.
    const bool shaderPipelineOn = (!m_app.m_ui || m_app.m_ui->getShaderPipelineEnabled());
    if (shaderPipelineOn && m_app.m_shaderEngine && m_app.m_shaderEngine->isShaderActive())
    {
        // IMPORTANT: Update viewport with window dimensions before applying shader
        // This ensures the last pass renders to the correct window size
        // IMPORTANT: Always use current dimensions, especially when entering fullscreen
        // IMPORTANT: Validate dimensions before updating viewport to avoid issues during resize
        uint32_t currentWidth = m_app.m_window ? m_app.m_window->getWidth() : m_app.m_windowWidth;
        uint32_t currentHeight = m_app.m_window ? m_app.m_window->getHeight() : m_app.m_windowHeight;

        // Validate dimensions before updating viewport
        if (currentWidth > 0 && currentHeight > 0 && currentWidth <= 7680 && currentHeight <= 4320)
        {
            m_app.m_shaderEngine->setViewport(currentWidth, currentHeight);
        }

        // Source para o shader chain. Por default, usamos a textura full-res
        // do FrameProcessor. Se o usuário pediu uma resolução LÓGICA menor que
        // a que o V4L2 entregou (driver ajustou pra mais próxima suportada),
        // fazemos um downscale com filter=NEAREST aqui — assim shaders CRT
        // recebem entrada "pixelada" baixa-res como foram desenhados, em vez
        // de 1080p suave onde scanlines viram sub-pixel e somem.
        GLuint shaderSrcTex = m_app.m_frameProcessor->getTexture();
        uint32_t shaderSrcW = m_app.m_frameProcessor->getTextureWidth();
        uint32_t shaderSrcH = m_app.m_frameProcessor->getTextureHeight();

        // Lê overscan antes de decidir o caminho — se há overscan, mesmo
        // que não precise de downscale, ainda fazemos o pass do FBO pra
        // aplicar o crop via viewport offset.
        const float overscanXPctRead = m_app.m_ui ? m_app.m_ui->getSourceOverscanPercentX() : 0.0f;
        const float overscanYPctRead = m_app.m_ui ? m_app.m_ui->getSourceOverscanPercentY() : 0.0f;
        const bool needsOverscan = (overscanXPctRead > 0.001f || overscanYPctRead > 0.001f);

        const bool needsDownscale = (m_app.m_logicalCaptureWidth > 0 &&
                                     m_app.m_logicalCaptureHeight > 0 &&
                                     m_app.m_logicalCaptureWidth < shaderSrcW &&
                                     m_app.m_logicalCaptureHeight < shaderSrcH &&
                                     shaderSrcTex != 0);

        if ((needsDownscale || needsOverscan) && shaderSrcTex != 0)
        {
            // FBO size: logical quando há downscale, source dims caso contrário.
            const uint32_t fboW = needsDownscale ? m_app.m_logicalCaptureWidth : shaderSrcW;
            const uint32_t fboH = needsDownscale ? m_app.m_logicalCaptureHeight : shaderSrcH;

            if (m_app.m_shaderSourceFBO == 0 ||
                m_app.m_shaderSourceFBOWidth != fboW ||
                m_app.m_shaderSourceFBOHeight != fboH)
            {
                if (m_app.m_shaderSourceTexture != 0)
                {
                    glDeleteTextures(1, &m_app.m_shaderSourceTexture);
                    m_app.m_shaderSourceTexture = 0;
                }
                if (m_app.m_shaderSourceFBO != 0)
                {
                    glDeleteFramebuffers(1, &m_app.m_shaderSourceFBO);
                    m_app.m_shaderSourceFBO = 0;
                }

                glGenTextures(1, &m_app.m_shaderSourceTexture);
                glBindTexture(GL_TEXTURE_2D, m_app.m_shaderSourceTexture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                             static_cast<GLsizei>(fboW),
                             static_cast<GLsizei>(fboH),
                             0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                glGenFramebuffers(1, &m_app.m_shaderSourceFBO);
                glBindFramebuffer(GL_FRAMEBUFFER, m_app.m_shaderSourceFBO);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, m_app.m_shaderSourceTexture, 0);

                m_app.m_shaderSourceFBOWidth = fboW;
                m_app.m_shaderSourceFBOHeight = fboH;
            }

            glBindFramebuffer(GL_FRAMEBUFFER, m_app.m_shaderSourceFBO);

            // Overscan: amplia o viewport de modo que apenas a região
            // central (1 - 2*overscan) do source caia dentro do FBO.
            // X e Y independentes; 0 = sem corte, 0.45 = corta 45% de cada lado.
            // std::clamp é C++17; o MinGW antigo do build Windows não tem.
            const float overscanX = std::max(0.0f, std::min(0.45f, overscanXPctRead / 100.0f));
            const float overscanY = std::max(0.0f, std::min(0.45f, overscanYPctRead / 100.0f));
            const float visibleFracX = 1.0f - 2.0f * overscanX;
            const float visibleFracY = 1.0f - 2.0f * overscanY;
            const float vpW = static_cast<float>(fboW) / visibleFracX;
            const float vpH = static_cast<float>(fboH) / visibleFracY;
            const GLint vpX = static_cast<GLint>((static_cast<float>(fboW) - vpW) / 2.0f);
            const GLint vpY = static_cast<GLint>((static_cast<float>(fboH) - vpH) / 2.0f);
            glViewport(vpX, vpY,
                       static_cast<GLsizei>(vpW),
                       static_cast<GLsizei>(vpH));

            // Forçar NEAREST na textura source pra preservar look pixelado
            // no downscale, e restaurar pra config do FrameProcessor depois.
            const GLint restoreFilter = m_app.m_frameProcessor->getTextureFilterLinear()
                                            ? GL_LINEAR
                                            : GL_NEAREST;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, shaderSrcTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            m_app.m_renderer->renderTexture(shaderSrcTex,
                                      fboW, fboH,
                                      false, false, 1.0f, 1.0f, false,
                                      shaderSrcW, shaderSrcH,
                                      /*preserveViewport=*/true);

            glBindTexture(GL_TEXTURE_2D, shaderSrcTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, restoreFilter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, restoreFilter);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            shaderSrcTex = m_app.m_shaderSourceTexture;
            shaderSrcW = fboW;
            shaderSrcH = fboH;
        }

        textureToRender = m_app.m_shaderEngine->applyShader(shaderSrcTex, shaderSrcW, shaderSrcH);
        isShaderTexture = true;
        
        // Log saída do shader
        static int shaderOutputLogCount = 0;
        if (shaderOutputLogCount++ < 3)
        {
            LOG_DEBUG("=== SHADER OUTPUT ===");
            LOG_DEBUG("Shader output texture: " + std::to_string(textureToRender) +
                     ", Output size: " + std::to_string(m_app.m_shaderEngine->getOutputWidth()) + "x" + 
                     std::to_string(m_app.m_shaderEngine->getOutputHeight()));
            LOG_DEBUG("=====================");
        }

        // DEBUG: Check returned texture
        if (textureToRender == 0)
        {
            LOG_WARN("Shader returned invalid texture (0), using original texture");
            textureToRender = m_app.m_frameProcessor->getTexture();
            isShaderTexture = false;
        }
        else
        {
        }
    }

    // Clear window framebuffer before rendering
    // IMPORTANT: Framebuffer 0 is the window (default framebuffer)
    // IMPORTANT: Lock mutex to protect during resize
    std::lock_guard<std::mutex> resizeLock(m_app.m_resizeMutex);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // IMPORTANT: Reset viewport to full window size
    // This ensures texture is rendered to entire window
    // IMPORTANT: Always update viewport with current window dimensions
    // This is especially important when entering fullscreen
    uint32_t currentWidth = m_app.m_window ? m_app.m_window->getWidth() : m_app.m_windowWidth;
    uint32_t currentHeight = m_app.m_window ? m_app.m_window->getHeight() : m_app.m_windowHeight;

    // Validate dimensions before continuing
    if (currentWidth == 0 || currentHeight == 0 || currentWidth > 7680 || currentHeight > 4320)
    {
        // Invalid dimensions, skip this frame. Return true so run() skips the
        // rest of the loop iteration (this branch already presented the frame),
        // matching the original `continue;` here.
        if (m_app.m_ui)
        {
            m_app.m_ui->endFrame();
        }
        m_app.m_window->swapBuffers();
        return true;
    }

    // DEBUG: Log to check if dimensions changed
    static uint32_t lastViewportWidth = 0, lastViewportHeight = 0;
    if (currentWidth != lastViewportWidth || currentHeight != lastViewportHeight)
    {
        lastViewportWidth = currentWidth;
        lastViewportHeight = currentHeight;
    }

    // Usar resolução da janela (sem limitações hardcoded)
    // O usuário pode controlar a resolução de saída via setOutputResolution()
    glViewport(0, 0, currentWidth, currentHeight);

    // IMPORTANT: For shaders with alpha (like Game Boy), don't clear with opaque black
    // Clear with transparent black so blending works correctly
    if (isShaderTexture)
    {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Transparent for shaders with alpha
    }
    else
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Opaque for normal capture
    }
    glClear(GL_COLOR_BUFFER_BIT);

    // For shader textures (framebuffer), invert Y (shader renders inverted)
    // For original texture (camera), don't invert Y (already correct)
    // IMPORTANT: If shader texture, may need blending for alpha
    // Get texture dimensions to calculate aspect ratio
    // IMPORTANT: For maintainAspect, always use ORIGINAL CAPTURE dimensions
    // because shader processes image but maintains same aspect ratio
    // Shader output texture may have window (viewport) dimensions, not image dimensions
    uint32_t renderWidth, renderHeight;
    if (isShaderTexture && m_app.m_maintainAspect)
    {
        // For maintainAspect with shader, use original capture dimensions
        // Shader processes but maintains original image aspect ratio
        renderWidth = m_app.m_frameProcessor->getTextureWidth();
        renderHeight = m_app.m_frameProcessor->getTextureHeight();
    }
    else if (isShaderTexture)
    {
        // Without maintainAspect, use shader output dimensions
        renderWidth = m_app.m_shaderEngine->getOutputWidth();
        renderHeight = m_app.m_shaderEngine->getOutputHeight();
        if (renderWidth == 0 || renderHeight == 0)
        {
            LOG_WARN("Shader output dimensions invalid (0x0), using capture dimensions");
            renderWidth = m_app.m_frameProcessor->getTextureWidth();
            renderHeight = m_app.m_frameProcessor->getTextureHeight();
        }
    }
    else
    {
        // Without shader, use capture dimensions
        renderWidth = m_app.m_frameProcessor->getTextureWidth();
        renderHeight = m_app.m_frameProcessor->getTextureHeight();
    }

    // NOVO: Aplicar resolução de saída configurável (se definida)
    // Isso permite que o usuário controle a resolução final antes de esticar para a janela
    // 0 = automático (usar resolução do source)
    GLuint finalTexture = textureToRender;
    uint32_t finalRenderWidth = renderWidth;
    uint32_t finalRenderHeight = renderHeight;
    
    // Log detalhado das resoluções no início do pipeline
    static int pipelineResLogCount = 0;
    if (pipelineResLogCount++ < 3)
    {
        LOG_DEBUG("=== PIPELINE RESOLUTIONS ===");
        LOG_DEBUG("Original capture: " + 
                 std::to_string(m_app.m_frameProcessor->getTextureWidth()) + "x" + std::to_string(m_app.m_frameProcessor->getTextureHeight()));
        LOG_DEBUG("Shader output (renderWidth/Height): " + std::to_string(renderWidth) + "x" + std::to_string(renderHeight));
        if (isShaderTexture)
        {
            LOG_DEBUG("Shader engine output: " + std::to_string(m_app.m_shaderEngine->getOutputWidth()) + "x" + 
                     std::to_string(m_app.m_shaderEngine->getOutputHeight()));
        }
        LOG_DEBUG("Output resolution (m_app.m_outputWidth/Height): " + std::to_string(m_app.m_outputWidth) + "x" + std::to_string(m_app.m_outputHeight));
        LOG_DEBUG("textureToRender: " + std::to_string(textureToRender) + ", isShaderTexture: " + std::string(isShaderTexture ? "yes" : "no"));
        LOG_DEBUG("===========================");
    }
    
    // Garantir que renderWidth e renderHeight são válidos (não 0)
    if (finalRenderWidth == 0 || finalRenderHeight == 0)
    {
        static int renderDimensionWarningCount = 0;
        if (renderDimensionWarningCount++ < 3)
        {
            LOG_WARN("Frame render: Invalid render dimensions (" +
                     std::to_string(finalRenderWidth) + "x" + std::to_string(finalRenderHeight) +
                     "), using capture dimensions");
        }
        // Fallback: usar dimensões de captura
        if (m_app.m_frameProcessor)
        {
            finalRenderWidth = m_app.m_frameProcessor->getTextureWidth();
            finalRenderHeight = m_app.m_frameProcessor->getTextureHeight();
        }
        // Se ainda for 0, usar dimensões padrão
        if (finalRenderWidth == 0 || finalRenderHeight == 0)
        {
            finalRenderWidth = 1920;
            finalRenderHeight = 1080;
        }
    }

    if (m_app.m_outputWidth > 0 && m_app.m_outputHeight > 0)
    {
        // Resolução de saída configurada - fazer downscale/upscale da textura
        // Criar framebuffer temporário para redimensionar
        static GLuint outputFramebuffer = 0;
        static GLuint outputTexture = 0;
        static uint32_t lastOutputWidth = 0;
        static uint32_t lastOutputHeight = 0;

        // Recriar framebuffer se necessário
        if (outputFramebuffer == 0 || outputTexture == 0 ||
            lastOutputWidth != m_app.m_outputWidth || lastOutputHeight != m_app.m_outputHeight)
        {
            // Limpar recursos antigos
            if (outputFramebuffer != 0)
            {
                glDeleteFramebuffers(1, &outputFramebuffer);
                outputFramebuffer = 0;
            }
            if (outputTexture != 0)
            {
                glDeleteTextures(1, &outputTexture);
                outputTexture = 0;
            }

            // Criar nova textura
            glGenTextures(1, &outputTexture);
            glBindTexture(GL_TEXTURE_2D, outputTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_app.m_outputWidth, m_app.m_outputHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Criar framebuffer
            glGenFramebuffers(1, &outputFramebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, outputFramebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0);

            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            {
                LOG_ERROR("Failed to create output resolution framebuffer");
                glDeleteFramebuffers(1, &outputFramebuffer);
                glDeleteTextures(1, &outputTexture);
                outputFramebuffer = 0;
                outputTexture = 0;
            }
            else
            {
                lastOutputWidth = m_app.m_outputWidth;
                lastOutputHeight = m_app.m_outputHeight;
                LOG_INFO("Output resolution framebuffer created: " +
                         std::to_string(m_app.m_outputWidth) + "x" + std::to_string(m_app.m_outputHeight));
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // Renderizar textura original para o framebuffer de saída (redimensionando)
        if (outputFramebuffer != 0 && outputTexture != 0)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, outputFramebuffer);
            glViewport(0, 0, m_app.m_outputWidth, m_app.m_outputHeight);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // Renderizar textura original redimensionada.
            // enableBlend=false: shaders RetroArch frequentemente escrevem
            // vec4(rgb, 0.0); com blending habilitado (SRC_ALPHA × 0) o destino
            // limpo a (0,0,0,0) gera preto. Reproduzimos o comportamento do
            // RetroArch que ignora o alpha e mostra o RGB direto.
            m_app.m_renderer->renderTexture(textureToRender, m_app.m_outputWidth, m_app.m_outputHeight,
                                      false, false, 1.0f, 1.0f,
                                      false, renderWidth, renderHeight);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // Usar textura redimensionada
            finalTexture = outputTexture;
            finalRenderWidth = m_app.m_outputWidth;
            finalRenderHeight = m_app.m_outputHeight;
            
            static int outputResizeLogCount = 0;
            if (outputResizeLogCount++ < 3)
            {
                LOG_INFO("Output resolution applied - Before: " + 
                         std::to_string(renderWidth) + "x" + std::to_string(renderHeight) +
                         ", After: " + std::to_string(finalRenderWidth) + "x" + std::to_string(finalRenderHeight) +
                         ", outputTexture: " + std::to_string(outputTexture));
            }
        }
    }
    
    // Log final das resoluções após processamento
    static int finalResLogCount = 0;
    if (finalResLogCount++ < 3)
    {
        LOG_DEBUG("Final texture resolutions - finalTexture: " + std::to_string(finalTexture) +
                 ", finalRenderWidth: " + std::to_string(finalRenderWidth) +
                 ", finalRenderHeight: " + std::to_string(finalRenderHeight));
    }

    // Camera image and shader output both need Y inversion in
    // the general case — that's why flipY defaults to true.
    //
    // Exception: remote source consumed without a client-side
    // shader. The /raw wire data goes through one fewer Y
    // inversion than a locally-captured frame (no FrameProcessor
    // upload→shader→sample chain on the client), so the
    // renderer's implicit flip overshoots and the image lands
    // upside-down. When the user disables the client-side
    // shader pipeline on a Remote source, drop the flip so the
    // picture stays right-side-up (#67).
    const bool remoteWithoutShader =
        (m_app.m_ui && m_app.m_ui->getSourceType() == UIManager::SourceType::Remote &&
         !isShaderTexture);
    bool shouldFlipY = !remoteWithoutShader;

    // Calculate viewport where capture will be rendered (may be smaller than window if maintainAspect is active)
    uint32_t windowWidth = m_app.m_window->getWidth();
    uint32_t windowHeight = m_app.m_window->getHeight();
    GLint viewportX = 0;
    GLint viewportY = 0;
    GLsizei viewportWidth = windowWidth;
    GLsizei viewportHeight = windowHeight;

    if (m_app.m_maintainAspect && finalRenderWidth > 0 && finalRenderHeight > 0)
    {
        // Calculate texture and window aspect ratio (same as renderTexture)
        float textureAspect = static_cast<float>(finalRenderWidth) / static_cast<float>(finalRenderHeight);
        float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);

        if (textureAspect > windowAspect)
        {
            // Texture is wider: adjust height (letterboxing)
            viewportHeight = static_cast<GLsizei>(windowWidth / textureAspect);
            viewportY = (windowHeight - viewportHeight) / 2;
        }
        else
        {
            // Texture is taller: adjust width (pillarboxing)
            viewportWidth = static_cast<GLsizei>(windowHeight * textureAspect);
            viewportX = (windowWidth - viewportWidth) / 2;
        }
    }

    // IMPORTANTE: Garantir que estamos renderizando no framebuffer padrão (janela)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Renderizar textura final na janela (sempre preenche a janela completamente).
    // enableBlend=false pelo mesmo motivo do resize acima.
    m_app.m_renderer->renderTexture(finalTexture, m_app.m_window->getWidth(), m_app.m_window->getHeight(),
                              shouldFlipY, false, m_app.m_brightness, m_app.m_contrast,
                              m_app.m_maintainAspect, finalRenderWidth, finalRenderHeight);

    // IMPORTANTE: Para streaming e recording, capturar diretamente da textura final ao invés do framebuffer
    // Isso evita problemas com back/front buffer e garante que capturamos a imagem renderizada
    bool needsFrameCapture = (m_app.m_streamManager && m_app.m_streamManager->isActive()) ||
                            (m_app.m_recordingManager && m_app.m_recordingManager->isRecording())
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
                            || (m_app.m_virtcam && m_app.m_virtcam->isRunning())
#endif
                            ;

    // Log para debug: verificar tamanho da textura final antes da captura
    static int finalTextureSizeLogCount = 0;
    if (needsFrameCapture && finalTextureSizeLogCount++ < 3)
    {
        LOG_DEBUG("Frame capture: finalTexture=" + std::to_string(finalTexture) + 
                 ", finalRenderWidth=" + std::to_string(finalRenderWidth) + 
                 ", finalRenderHeight=" + std::to_string(finalRenderHeight) +
                 ", renderWidth=" + std::to_string(renderWidth) +
                 ", renderHeight=" + std::to_string(renderHeight) +
                 ", outputWidth=" + std::to_string(m_app.m_outputWidth) +
                 ", outputHeight=" + std::to_string(m_app.m_outputHeight) +
                 ", textureToRender=" + std::to_string(textureToRender));
    }

    if (needsFrameCapture)
    {

        // Capturar do viewport (o que está sendo renderizado)
        uint32_t captureWidth = static_cast<uint32_t>(viewportWidth);
        uint32_t captureHeight = static_cast<uint32_t>(viewportHeight);
        size_t captureDataSize = static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * 3;

        if (captureDataSize > 0 && captureDataSize <= (7680 * 4320 * 3) &&
            captureWidth > 0 && captureHeight > 0 && captureWidth <= 7680 && captureHeight <= 4320)
        {
            // SOLUÇÃO PARA DIRECTFB: Capturar diretamente da textura final usando FBO
            // Isso evita problemas com back/front buffer do framebuffer padrão
            // que não funciona corretamente com DirectFB

            // Salvar FBO atual
            GLint previousFBO = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO);

                // Verificar se a textura é válida e as dimensões são válidas antes de tentar capturar
                if (finalTexture == 0)
                {
                    static int textureWarningCount = 0;
                    if (textureWarningCount++ < 3)
                    {
                        LOG_WARN("Frame capture: finalTexture is 0, cannot capture");
                    }
                }
                else if (finalRenderWidth == 0 || finalRenderHeight == 0)
                {
                    static int dimensionWarningCount = 0;
                    if (dimensionWarningCount++ < 3)
                    {
                        LOG_WARN("Frame capture: Invalid dimensions (" +
                                 std::to_string(finalRenderWidth) + "x" + std::to_string(finalRenderHeight) +
                                 "), cannot capture");
                    }
                }
                else
                {
                    // Criar FBO temporário para ler da textura
                GLuint captureFBO = 0;
                glGenFramebuffers(1, &captureFBO);
                glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
                
                // IMPORTANTE: Para gravação, capturar da textura APÓS o shader e APÓS o redimensionamento de saída (se configurado)
                // Isso garante que capturamos a imagem completa processada com o shader aplicado
                // O MediaEncoder fará o redimensionamento final para a resolução de gravação se necessário
                GLuint textureToCapture = finalTexture;
                uint32_t captureTextureWidth = finalRenderWidth;
                uint32_t captureTextureHeight = finalRenderHeight;
                
                // Se estamos gravando ou fazendo streaming, determinar qual textura e dimensões usar
                // IMPORTANTE: Se há resolução de saída configurada, usar finalTexture
                // Se não há resolução de saída mas há shader, usar textureToRender com dimensões do shader
                // Isso garante que capturamos a textura completa processada tanto para streaming quanto para gravação
                bool needsFrameCapture = (m_app.m_recordingManager && m_app.m_recordingManager->isRecording()) ||
                                        (m_app.m_streamManager && m_app.m_streamManager->isActive())
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
                                        || (m_app.m_virtcam && m_app.m_virtcam->isRunning())
#endif
                                        ;
                
                if (needsFrameCapture)
                {
                    // IMPORTANTE: Se há uma resolução de saída configurada, usar finalTexture que já foi redimensionada
                    // Se não há resolução de saída, mas há shader, usar textureToRender com as dimensões reais do shader
                    // Isso garante que capturamos a textura completa processada
                    if (m_app.m_outputWidth > 0 && m_app.m_outputHeight > 0)
                    {
                        // Resolução de saída configurada: usar finalTexture que já foi redimensionada
                        textureToCapture = finalTexture;
                        captureTextureWidth = finalRenderWidth;
                        captureTextureHeight = finalRenderHeight;
                    }
                    else if (isShaderTexture)
                    {
                        // Sem resolução de saída, mas com shader: usar textureToRender com dimensões reais do shader
                        textureToCapture = textureToRender;
                        captureTextureWidth = m_app.m_shaderEngine->getOutputWidth();
                        captureTextureHeight = m_app.m_shaderEngine->getOutputHeight();
                        
                        // Se dimensões do shader são inválidas, usar renderWidth/renderHeight
                        if (captureTextureWidth == 0 || captureTextureHeight == 0)
                        {
                            captureTextureWidth = renderWidth;
                            captureTextureHeight = renderHeight;
                        }
                    }
                    else
                    {
                        // Sem shader e sem resolução de saída: usar finalTexture
                        textureToCapture = finalTexture;
                        captureTextureWidth = finalRenderWidth;
                        captureTextureHeight = finalRenderHeight;
                    }
                    
                    static int captureSourceLogCount = 0;
                    if (captureSourceLogCount++ < 3)
                    {
                        LOG_DEBUG("=== FRAME CAPTURE DEBUG ===");
                        LOG_DEBUG("Original capture: " + 
                                 std::to_string(m_app.m_frameProcessor->getTextureWidth()) + "x" + std::to_string(m_app.m_frameProcessor->getTextureHeight()));
                        if (isShaderTexture)
                        {
                            LOG_DEBUG("Shader engine output: " + 
                                     std::to_string(m_app.m_shaderEngine->getOutputWidth()) + "x" + std::to_string(m_app.m_shaderEngine->getOutputHeight()));
                        }
                        LOG_INFO("renderWidth/Height: " + std::to_string(renderWidth) + "x" + std::to_string(renderHeight));
                        LOG_INFO("finalRenderWidth/Height: " + std::to_string(finalRenderWidth) + "x" + std::to_string(finalRenderHeight));
                        LOG_DEBUG("Output resolution: " + std::to_string(m_app.m_outputWidth) + "x" + std::to_string(m_app.m_outputHeight));
                        
                        if (m_app.m_recordingManager && m_app.m_recordingManager->isRecording())
                        {
                            RecordingSettings recSettings = m_app.m_recordingManager->getRecordingSettings();
                            LOG_INFO("Recording resolution: " + std::to_string(recSettings.width) + "x" + std::to_string(recSettings.height));
                        }
                        if (m_app.m_streamManager && m_app.m_streamManager->isActive() && m_app.m_ui)
                        {
                            LOG_INFO("Streaming resolution: " + std::to_string(m_app.m_ui->getStreamingWidth()) + "x" + std::to_string(m_app.m_ui->getStreamingHeight()));
                        }
                        
                        LOG_DEBUG("Selected - textureToCapture: " + std::to_string(textureToCapture) +
                                 ", size: " + std::to_string(captureTextureWidth) + "x" + std::to_string(captureTextureHeight));
                        LOG_DEBUG("Textures - textureToRender: " + std::to_string(textureToRender) +
                                 ", finalTexture: " + std::to_string(finalTexture));
                        LOG_DEBUG("===========================");
                    }
                }
                
                // #85 — Apply Image-tab adjustments (brightness,
                // contrast) to the capture frame. The window
                // render path at the bottom of this loop runs
                // them via m_renderer->renderTexture, but the
                // texture we'd otherwise attach here is the
                // pre-adjustment finalTexture. Render it once
                // more into a side framebuffer with the
                // adjustments baked in, then capture from THAT.
                // Single extra render pass per frame; the
                // texture is RGBA so the downstream readback
                // always pulls 4 bpp.
                //
                // Without this, virtcam consumers + recordings
                // + streamed frames all reflect the un-adjusted
                // image — user tweaks brightness expecting it
                // to land in OBS and it doesn't.
                static GLuint postImageFBO     = 0;
                static GLuint postImageTex     = 0;
                static uint32_t postImageW     = 0;
                static uint32_t postImageH     = 0;
                const bool postImageActive =
                    (m_app.m_brightness != 1.0f) || (m_app.m_contrast != 1.0f);

                GLuint fboTextureToAttach = textureToCapture;
                if (postImageActive)
                {
                    if (postImageFBO == 0 || postImageTex == 0 ||
                        postImageW != captureTextureWidth ||
                        postImageH != captureTextureHeight)
                    {
                        if (postImageFBO) glDeleteFramebuffers(1, &postImageFBO);
                        if (postImageTex) glDeleteTextures(1, &postImageTex);
                        postImageFBO = 0; postImageTex = 0;
                        glGenTextures(1, &postImageTex);
                        glBindTexture(GL_TEXTURE_2D, postImageTex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                     captureTextureWidth,
                                     captureTextureHeight, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        glGenFramebuffers(1, &postImageFBO);
                        glBindFramebuffer(GL_FRAMEBUFFER, postImageFBO);
                        glFramebufferTexture2D(GL_FRAMEBUFFER,
                                               GL_COLOR_ATTACHMENT0,
                                               GL_TEXTURE_2D, postImageTex, 0);
                        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
                            GL_FRAMEBUFFER_COMPLETE)
                        {
                            glDeleteFramebuffers(1, &postImageFBO);
                            glDeleteTextures(1, &postImageTex);
                            postImageFBO = 0; postImageTex = 0;
                        }
                        else
                        {
                            postImageW = captureTextureWidth;
                            postImageH = captureTextureHeight;
                        }
                    }
                    if (postImageFBO && postImageTex)
                    {
                        glBindFramebuffer(GL_FRAMEBUFFER, postImageFBO);
                        glViewport(0, 0, postImageW, postImageH);
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        m_app.m_renderer->renderTexture(
                            textureToCapture,
                            postImageW, postImageH,
                            /*flipY=*/false, /*enableBlend=*/false,
                            m_app.m_brightness, m_app.m_contrast,
                            /*maintainAspect=*/false,
                            captureTextureWidth, captureTextureHeight,
                            /*preserveViewport=*/true);
                        fboTextureToAttach = postImageTex;
                        // Re-bind the original captureFBO so the
                        // attach-and-read below operates on it
                        // unchanged.
                        glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
                    }
                }

                // Anexar a textura escolhida ao FBO
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTextureToAttach, 0);

                    // Verificar se o FBO está completo
                    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                    if (status != GL_FRAMEBUFFER_COMPLETE)
                    {
                        LOG_WARN("Frame capture: FBO incomplete, falling back to default framebuffer");
                        glBindFramebuffer(GL_FRAMEBUFFER, previousFBO);
                        glDeleteFramebuffers(1, &captureFBO);

                        // Fallback: tentar capturar do framebuffer padrão
                        glBindFramebuffer(GL_FRAMEBUFFER, 0);
                        glViewport(0, 0, windowWidth, windowHeight);
                        GLint readY = static_cast<GLint>(windowHeight) - viewportY - static_cast<GLint>(viewportHeight);

                        uint32_t actualCaptureWidth = static_cast<uint32_t>(viewportWidth);
                        uint32_t actualCaptureHeight = static_cast<uint32_t>(viewportHeight);
                        size_t rgbDataSize = static_cast<size_t>(actualCaptureWidth) * static_cast<size_t>(actualCaptureHeight) * 3;
                        size_t readRowSizeUnpadded = static_cast<size_t>(actualCaptureWidth) * 3;
                        size_t readRowSizePadded = ((readRowSizeUnpadded + 3) / 4) * 4;
                        size_t totalSizeWithPadding = readRowSizePadded * static_cast<size_t>(actualCaptureHeight);

                        auto &frameDataWithPadding = m_app.m_captureSyncPadded;
                        frameDataWithPadding.resize(totalSizeWithPadding);

                        glReadPixels(viewportX, readY, static_cast<GLsizei>(actualCaptureWidth), static_cast<GLsizei>(actualCaptureHeight),
                                     GL_RGB, GL_UNSIGNED_BYTE, frameDataWithPadding.data());

                        // Converter dados (mesmo código abaixo)
                        auto &frameData = m_app.m_captureFrameData;
                        frameData.resize(rgbDataSize);
                        for (uint32_t row = 0; row < actualCaptureHeight; row++)
                        {
                            uint32_t srcRow = actualCaptureHeight - 1 - row;
                            uint32_t dstRow = row;
                            const uint8_t *srcPtr = frameDataWithPadding.data() + (srcRow * readRowSizePadded);
                            uint8_t *dstPtr = frameData.data() + (dstRow * readRowSizeUnpadded);
                            memcpy(dstPtr, srcPtr, readRowSizeUnpadded);
                        }

                        // Push every frame produced by this iteration. The main
                        // loop is already paced (see the render-pace blocks at the
                        // end of the loop), so the per-iteration rate matches the
                        // configured streaming FPS. The earlier dedicated push
                        // throttle here was meant to protect against an uncapped
                        // main loop on a high-refresh display, but with main-loop
                        // pacing in place it just rejected legitimate frames whose
                        // arrival jittered slightly below 1/fps (observed as
                        // "push throttle skips=20-24/s" while the encoder sat
                        // idle waiting for input).
                        if (m_app.m_streamManager && m_app.m_streamManager->isActive())
                        {
                            m_app.m_streamManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                        }
                        if (m_app.m_recordingManager && m_app.m_recordingManager->isRecording())
                        {
                            m_app.m_recordingManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                        }
                    }
                    else
                    {
                        // FBO está completo, continuar com captura da textura

                        // IMPORTANTE: ShaderEngine cria texturas em GL_RGBA, não GL_RGB
                        // Precisamos ler como RGBA e converter para RGB depois
                        // Master pipeline toggle off → applyShader was skipped above and
                        // textureToCapture is the source RGB texture, so treat as RGB.
                        const bool pipelineEnabled = (!m_app.m_ui || m_app.m_ui->getShaderPipelineEnabled());
                        bool isShaderTexture = (pipelineEnabled && m_app.m_shaderEngine && m_app.m_shaderEngine->isShaderActive());
                        // #85 — When the post-image render pass
                        // ran above, what's attached to captureFBO
                        // is RGBA regardless of source. Otherwise
                        // keep the original RGB/RGBA decision.
                        const bool readAsRgba = isShaderTexture ||
                            (postImageActive && fboTextureToAttach == postImageTex);
                        GLenum readFormat = readAsRgba ? GL_RGBA : GL_RGB;
                        uint32_t bytesPerPixel = readAsRgba ? 4 : 3;

                        static int formatLogCount = 0;
                        if (formatLogCount++ < 3)
                        {
                            LOG_DEBUG("Frame capture: Using format " + std::string(isShaderTexture ? "RGBA" : "RGB") +
                                     " for texture " + std::to_string(finalTexture) +
                                     " (shader active: " + std::string(isShaderTexture ? "yes" : "no") + ")");
                        }

                    // IMPORTANTE: Capturar a textura COMPLETA, não apenas uma parte
                    // Usar as dimensões e textura determinadas acima (pode ser finalTexture ou textureToRender)
                    uint32_t textureWidth = captureTextureWidth;
                    uint32_t textureHeight = captureTextureHeight;
                    
                    // Log detalhado: verificar tamanho da textura vs resolução de gravação/streaming
                    static int textureSizeLogCount = 0;
                    bool shouldLog = (textureSizeLogCount++ < 3) && 
                                    ((m_app.m_recordingManager && m_app.m_recordingManager->isRecording()) ||
                                     (m_app.m_streamManager && m_app.m_streamManager->isActive()));
                    if (shouldLog)
                    {
                        LOG_DEBUG("=== CAPTURE DETAILS ===");
                        LOG_DEBUG("Capturing from texture: " + std::to_string(textureToCapture) +
                                 ", Size: " + std::to_string(textureWidth) + "x" + std::to_string(textureHeight));
                        if (m_app.m_recordingManager && m_app.m_recordingManager->isRecording())
                        {
                            RecordingSettings recSettings = m_app.m_recordingManager->getRecordingSettings();
                            LOG_INFO("Recording target: " + 
                                     std::to_string(recSettings.width) + "x" + std::to_string(recSettings.height));
                            LOG_DEBUG("Will resize for recording: " + std::string(
                                (textureWidth != recSettings.width || textureHeight != recSettings.height) ? "YES" : "NO"));
                        }
                        if (m_app.m_streamManager && m_app.m_streamManager->isActive() && m_app.m_ui)
                        {
                            LOG_DEBUG("Streaming target: " + 
                                     std::to_string(m_app.m_ui->getStreamingWidth()) + "x" + std::to_string(m_app.m_ui->getStreamingHeight()));
                            LOG_DEBUG("Will resize for streaming: " + std::string(
                                (textureWidth != m_app.m_ui->getStreamingWidth() || textureHeight != m_app.m_ui->getStreamingHeight()) ? "YES" : "NO"));
                        }
                        LOG_DEBUG("======================");
                    }
                    
                    size_t rgbDataSize = static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight) * 3;

                    // glReadPixels lê do FBO atual (com textura anexada). O viewport
                    // do FBO precisa ser exatamente o tamanho da textura.
                    glViewport(0, 0, textureWidth, textureHeight);

                    static int viewportCheckCount = 0;
                    if (viewportCheckCount++ < 3)
                    {
                        GLint currentViewport[4];
                        glGetIntegerv(GL_VIEWPORT, currentViewport);
                        LOG_DEBUG("Frame capture: Viewport set to " +
                                     std::to_string(textureWidth) + "x" + std::to_string(textureHeight) +
                                     ", actual viewport: [" + std::to_string(currentViewport[0]) + "," +
                                     std::to_string(currentViewport[1]) + "," +
                                     std::to_string(currentViewport[2]) + "x" + std::to_string(currentViewport[3]) + "]");
                    }

                    auto &frameData = m_app.m_captureFrameData;
                    bool frameDataReady = false;

                    // Decidir entre PBO async e leitura síncrona ANTES de tocar no FBO.
                    // O PBO precisa do FBO bound durante startAsyncRead; o sync precisa
                    // do FBO bound durante glReadPixels. Os dois caminhos divergem aqui.
                    //
                    // #129 — on Windows the on-screen render is correct but the
                    // streamed/recorded frames come out black: the async-PBO
                    // readback (FBO -> PBO -> getReadData) returns zeros on the
                    // Windows GL driver while the synchronous glReadPixels path
                    // works. Force the sync path on Windows. Can be overridden
                    // either way with RETROCAPTURE_PBO=1/0 for A/B testing.
                    bool allowPBO;
                    {
                        const char *pboEnv = std::getenv("RETROCAPTURE_PBO");
                        if (pboEnv)      allowPBO = (pboEnv[0] == '1');
#ifdef _WIN32
                        else             allowPBO = false; // sync readback on Windows (#129)
#else
                        else             allowPBO = true;
#endif
                    }
                    bool useAsyncPBO = (allowPBO && m_app.m_pboManager &&
                                        m_app.m_pboManager->init(textureWidth, textureHeight, readFormat) &&
                                        m_app.m_pboManager->isInitialized());

                    if (useAsyncPBO)
                    {
                        // Agenda glReadPixels no PBO (não bloqueia).
                        m_app.m_pboManager->startAsyncRead(0, 0,
                                                     static_cast<GLsizei>(textureWidth),
                                                     static_cast<GLsizei>(textureHeight));

                        // FBO já não é mais necessário — glReadPixels foi agendado.
                        glBindFramebuffer(GL_FRAMEBUFFER, previousFBO);
                        glDeleteFramebuffers(1, &captureFBO);

                        // Tenta obter os dados do frame ANTERIOR (já transferidos da GPU).
                        // Os primeiros 1-2 frames pós-init não têm dados ainda — drop.
                        if (bytesPerPixel == 3)
                        {
                            frameData.resize(rgbDataSize);
                            frameDataReady = m_app.m_pboManager->getReadData(
                                frameData.data(), textureWidth, textureHeight, /*flipY=*/false);
                            // #85 — Virtual camera also gets fed from
                            // the RGB path (no-shader / direct capture
                            // passthrough). Without this branch the
                            // consumer sees uninitialised buffer
                            // contents (black / static) whenever a
                            // shader is off, which was the symptom
                            // user hit on first end-to-end test.
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
                            if (frameDataReady && m_app.m_virtcam &&
                                m_app.m_virtcam->isRunning())
                            {
                                m_app.m_virtcam->pushFrame(
                                    frameData.data(),
                                    textureWidth, textureHeight,
                                    Application::VirtcamSinkT::SourceFormat::RGB);
                            }
#endif
                        }
                        else
                        {
                            auto &rgbaData = m_app.m_captureRgbaScratch;
                            rgbaData.resize(static_cast<size_t>(textureWidth) *
                                            static_cast<size_t>(textureHeight) * 4);
                            if (m_app.m_pboManager->getReadData(rgbaData.data(),
                                                          textureWidth, textureHeight,
                                                          /*flipY=*/false))
                            {
                                // #85 — Virtual camera piggybacks on
                                // this RGBA readback (shader path).
                                // Push happens BEFORE the RGB strip
                                // below so the sink keeps the alpha
                                // channel intact (sws RGBA → YUYV).
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
                                if (m_app.m_virtcam && m_app.m_virtcam->isRunning())
                                {
                                    m_app.m_virtcam->pushFrame(
                                        rgbaData.data(),
                                        textureWidth, textureHeight,
                                        Application::VirtcamSinkT::SourceFormat::RGBA);
                                }
#endif
                                frameData.resize(rgbDataSize);
                                for (uint32_t row = 0; row < textureHeight; row++)
                                {
                                    const uint8_t *src = rgbaData.data() + (row * textureWidth * 4);
                                    uint8_t *dst = frameData.data() + (row * textureWidth * 3);
                                    for (uint32_t col = 0; col < textureWidth; col++)
                                    {
                                        dst[col * 3 + 0] = src[col * 4 + 0];
                                        dst[col * 3 + 1] = src[col * 4 + 1];
                                        dst[col * 3 + 2] = src[col * 4 + 2];
                                    }
                                }
                                frameDataReady = true;
                            }
                        }
                    }
                    else
                    {
                        // Caminho síncrono (fallback quando PBO não está disponível).
                        size_t syncRowUnpadded = static_cast<size_t>(textureWidth) * bytesPerPixel;
                        size_t syncRowPadded = ((syncRowUnpadded + 3) / 4) * 4;
                        auto &frameDataWithPadding = m_app.m_captureSyncPadded;
                        frameDataWithPadding.resize(syncRowPadded * static_cast<size_t>(textureHeight));

                        glReadPixels(0, 0,
                                     static_cast<GLsizei>(textureWidth),
                                     static_cast<GLsizei>(textureHeight),
                                     readFormat, GL_UNSIGNED_BYTE,
                                     frameDataWithPadding.data());

                        glBindFramebuffer(GL_FRAMEBUFFER, previousFBO);
                        glDeleteFramebuffers(1, &captureFBO);

                        frameData.resize(rgbDataSize);
                        for (uint32_t row = 0; row < textureHeight; row++)
                        {
                            const uint8_t *srcPtr = frameDataWithPadding.data() + (row * syncRowPadded);
                            uint8_t *dstPtr = frameData.data() + (row * textureWidth * 3);

                            if (isShaderTexture)
                            {
                                for (uint32_t col = 0; col < textureWidth; col++)
                                {
                                    dstPtr[col * 3 + 0] = srcPtr[col * 4 + 0];
                                    dstPtr[col * 3 + 1] = srcPtr[col * 4 + 1];
                                    dstPtr[col * 3 + 2] = srcPtr[col * 4 + 2];
                                }
                            }
                            else
                            {
                                memcpy(dstPtr, srcPtr, textureWidth * 3);
                            }
                        }
                        frameDataReady = true;
                    }

                    // Usar dimensões originais da textura
                    // O MediaEncoder fará o redimensionamento para a resolução de gravação/streaming se necessário
                    uint32_t actualCaptureWidth = textureWidth;
                    uint32_t actualCaptureHeight = textureHeight;
                    
                    // Verificar se o frame capturado está vazio/preto
                    // Isso ajuda a diagnosticar problemas com DirectFB
                    static int frameCheckCount = 0;
                        if (frameCheckCount++ < 10 || frameCheckCount % 60 == 0)
                        {
                            // Verificar se todos os pixels são pretos (0,0,0) ou se há dados válidos
                            size_t blackPixelCount = 0;
                            size_t totalPixels = static_cast<size_t>(actualCaptureWidth) * static_cast<size_t>(actualCaptureHeight);
                            size_t sampleSize = std::min(totalPixels, static_cast<size_t>(1000)); // Amostrar até 1000 pixels

                            for (size_t i = 0; i < sampleSize; i++)
                            {
                                size_t pixelIdx = (i * totalPixels) / sampleSize; // Amostragem uniforme
                                size_t byteIdx = pixelIdx * 3;
                                if (byteIdx + 2 < frameData.size())
                                {
                                    if (frameData[byteIdx] == 0 &&
                                        frameData[byteIdx + 1] == 0 &&
                                        frameData[byteIdx + 2] == 0)
                                    {
                                        blackPixelCount++;
                                    }
                                }
                            }

                            double blackRatio = static_cast<double>(blackPixelCount) / static_cast<double>(sampleSize);
                            if (blackRatio > 0.95 && frameCheckCount <= 10)
                            {
                                LOG_WARN("Frame capture: " + std::to_string(static_cast<int>(blackRatio * 100)) +
                                         "% of sampled pixels are black (may indicate DirectFB/framebuffer issue)");
                                LOG_WARN("Capture params: texture=" + std::to_string(textureToCapture) +
                                         ", size=" + std::to_string(actualCaptureWidth) + "x" + std::to_string(actualCaptureHeight));
                            }
                        }

                        // Per-pipeline shader override: when the master is on AND
                        // the shader is active, individual pipelines can request the
                        // raw pre-shader source instead. We do a sync readback of
                        // m_frameProcessor->getTexture() (raw V4L2 RGB upload) only
                        // when at least one active pipeline asked for it — keeps the
                        // common case (both pipelines follow the master) on the fast
                        // single-readback path.
                        const bool masterOn = (m_app.m_ui && m_app.m_ui->getShaderPipelineEnabled());
                        const bool shaderActive = (m_app.m_shaderEngine && m_app.m_shaderEngine->isShaderActive());
                        const bool streamWantsSource = m_app.m_ui && m_app.m_streamManager && m_app.m_streamManager->isActive() &&
                                                       !m_app.m_ui->getStreamingApplyShader();
                        const bool recordWantsSource = m_app.m_ui && m_app.m_recordingManager && m_app.m_recordingManager->isRecording() &&
                                                       !m_app.m_ui->getRecordingApplyShader();
                        // Phase 2 of #47: /raw is by-contract pre-shader, so any connected
                        // /raw client also triggers the source-frame capture below.
                        const bool rawWantsSource = m_app.m_streamManager && m_app.m_streamManager->isActive() &&
                                                    m_app.m_streamManager->hasRawClients();
                        const bool needSourceCapture = masterOn && shaderActive &&
                                                       (streamWantsSource || recordWantsSource || rawWantsSource);

                        bool sourceFrameReady = false;
                        uint32_t sourceFrameW = 0, sourceFrameH = 0;
                        if (needSourceCapture && m_app.m_frameProcessor)
                        {
                            GLuint srcTex = m_app.m_frameProcessor->getTexture();
                            sourceFrameW = m_app.m_frameProcessor->getTextureWidth();
                            sourceFrameH = m_app.m_frameProcessor->getTextureHeight();
                            if (srcTex && sourceFrameW > 0 && sourceFrameH > 0)
                            {
                                GLuint tmpFBO = 0;
                                glGenFramebuffers(1, &tmpFBO);
                                GLint prevFBO = 0;
                                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
                                glBindFramebuffer(GL_FRAMEBUFFER, tmpFBO);
                                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);
                                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
                                {
                                    const size_t rowUnpadded = static_cast<size_t>(sourceFrameW) * 3;
                                    const size_t rowPadded = ((rowUnpadded + 3) / 4) * 4;
                                    m_app.m_captureSourcePadded.resize(rowPadded * sourceFrameH);
                                    glReadPixels(0, 0, static_cast<GLsizei>(sourceFrameW),
                                                 static_cast<GLsizei>(sourceFrameH),
                                                 GL_RGB, GL_UNSIGNED_BYTE, m_app.m_captureSourcePadded.data());
                                    // glReadPixels returns rows bottom-up; flip + strip row padding.
                                    m_app.m_captureSourceFrameData.resize(rowUnpadded * sourceFrameH);
                                    for (uint32_t row = 0; row < sourceFrameH; ++row)
                                    {
                                        const uint32_t srcRow = sourceFrameH - 1 - row;
                                        const uint8_t *s = m_app.m_captureSourcePadded.data() + srcRow * rowPadded;
                                        uint8_t *d = m_app.m_captureSourceFrameData.data() + row * rowUnpadded;
                                        memcpy(d, s, rowUnpadded);
                                    }
                                    sourceFrameReady = true;
                                }
                                glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
                                glDeleteFramebuffers(1, &tmpFBO);
                            }
                        }

                        // Share frame data between streaming and recording.
                        // Pula push se o PBO async ainda não tem dados do frame anterior.
                        // Push every frame the FBO path produces. Main-loop pacing
                        // (added in cd7b13a / 4b69c72) already caps the iteration rate
                        // at the configured streaming FPS, so the per-frame interval
                        // matches the target and there's no risk of overshooting the
                        // way an uncapped 240 Hz refresh free-run did. An earlier
                        // dedicated throttle here ended up rejecting ~30 % of
                        // legitimate frames every second because per-iteration work
                        // time jittered slightly below 1/fps — visible in the log as
                        // "push throttle: pushes=37/s skips=24/s" while VAAPI sat idle
                        // waiting for input.
                        if (frameDataReady && m_app.m_streamManager && m_app.m_streamManager->isActive())
                        {
                            const bool useSource = streamWantsSource && sourceFrameReady;
                            static int streamPushLogCount = 0;
                            if (streamPushLogCount++ < 3 && m_app.m_ui)
                            {
                                LOG_DEBUG("--- PUSHING FRAME TO STREAMING ---");
                                LOG_DEBUG("Frame size being pushed: " + std::to_string(useSource ? sourceFrameW : actualCaptureWidth) + "x" + std::to_string(useSource ? sourceFrameH : actualCaptureHeight) +
                                         (useSource ? " (raw source — shader bypassed)" : ""));
                                LOG_DEBUG("Streaming target resolution: " + std::to_string(m_app.m_ui->getStreamingWidth()) + "x" + std::to_string(m_app.m_ui->getStreamingHeight()));
                                LOG_DEBUG("----------------------------------");
                            }
                            // #109 — gate the /stream feed on having a
                            // /stream client, exactly as /raw is gated by
                            // hasRawClients() below. Without this the host
                            // ran a second h264_vaapi 720p60 encode for the
                            // shader-processed /stream output even when only
                            // a remote /raw client was connected and nobody
                            // was watching /stream — two VAAPI encodes
                            // competing for the GPU, which starved the /raw
                            // encode and left the remote client's video
                            // lagging the audio (and overflowed the /stream
                            // synchronizer continuously). Idle the /stream
                            // encoder when no one is watching it.
                            if (m_app.m_streamManager->hasClients())
                            {
                                if (useSource)
                                {
                                    m_app.m_streamManager->pushFrame(m_app.m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH);
                                }
                                else
                                {
                                    m_app.m_streamManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                                }
                            }

                            // Phase 2 of #47: also feed the /raw output (pre-shader, always).
                            // hasRawClients() gates this so the encoder idles when nothing
                            // is listening — the CPU cost only shows up when a remote
                            // client is actually consuming the raw feed.
                            //
                            // Two code paths because the pre-shader pixels live in
                            // different buffers depending on whether the shader chain
                            // is running this frame:
                            //   - shader active → m_captureSourceFrameData (an extra
                            //     readback from the FrameProcessor texture).
                            //   - shader off (master toggle disabled or no preset
                            //     loaded) → frameData IS already the pre-shader pixels,
                            //     so we feed /raw from there. Without this branch,
                            //     disabling the shader from the UI silently kills
                            //     /raw transmission (#67 — client log showed video
                            //     stop while audio kept flowing).
                            if (m_app.m_streamManager->hasRawClients())
                            {
                                if (sourceFrameReady)
                                {
                                    m_app.m_streamManager->pushRawFrame(m_app.m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH);
                                }
                                else if (!masterOn || !shaderActive)
                                {
                                    m_app.m_streamManager->pushRawFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                                }
                            }
                        }
                        if (frameDataReady && m_app.m_recordingManager && m_app.m_recordingManager->isRecording())
                        {
                            const bool useSource = recordWantsSource && sourceFrameReady;
                            static int recordingPushLogCount = 0;
                            if (recordingPushLogCount++ < 3)
                            {
                                LOG_DEBUG("=== PUSHING FRAME TO RECORDING ===");
                                LOG_DEBUG("Frame size being pushed: " + std::to_string(useSource ? sourceFrameW : actualCaptureWidth) + "x" + std::to_string(useSource ? sourceFrameH : actualCaptureHeight) +
                                         (useSource ? " (raw source — shader bypassed)" : ""));
                                RecordingSettings recSettings = m_app.m_recordingManager->getRecordingSettings();
                                LOG_INFO("Recording target resolution: " + std::to_string(recSettings.width) + "x" + std::to_string(recSettings.height));
                                LOG_DEBUG("===================================");
                            }
                            if (useSource)
                            {
                                m_app.m_recordingManager->pushFrame(m_app.m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH);
                            }
                            else
                            {
                                m_app.m_recordingManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                            }
                        }
                } // fim do else (textura válida)
            } // fim do else (FBO completo)
        } // fim do if (needsFrameCapture)
    } // fim do if (captureDataSize > 0)

    auto streamManager = m_app.m_streamManager.get();
    if (m_app.m_ui && streamManager && streamManager->isActive())
    {
        m_app.m_ui->setStreamingActive(true);
        auto urls = streamManager->getStreamUrls();
        if (!urls.empty())
        {
            m_app.m_ui->setStreamUrl(urls[0]);
        }
        uint32_t clientCount = streamManager->getTotalClientCount();
        m_app.m_ui->setStreamClientCount(clientCount);

        // Update cooldown (if active, can start and there's no cooldown)
        m_app.m_ui->setCanStartStreaming(true);
        m_app.m_ui->setStreamingCooldownRemainingMs(0);

        // Update SSL certificate information if HTTPS is active
        std::string foundCert = streamManager->getFoundSSLCertificatePath();
        std::string foundKey = streamManager->getFoundSSLKeyPath();

        if (m_app.m_webPortalHTTPSEnabled && !foundCert.empty())
        {
            m_app.m_foundSSLCertPath = foundCert;
            m_app.m_foundSSLKeyPath = foundKey;
            m_app.m_ui->setFoundSSLCertificatePath(foundCert);
            m_app.m_ui->setFoundSSLKeyPath(foundKey);
        }
        else
        {
            // Clear found paths if HTTPS is not active
            m_app.m_foundSSLCertPath.clear();
            m_app.m_foundSSLKeyPath.clear();
            m_app.m_ui->setFoundSSLCertificatePath("");
            m_app.m_ui->setFoundSSLKeyPath("");
        }
    }
    else if (m_app.m_ui)
    {
        m_app.m_ui->setStreamingActive(false);
        m_app.m_ui->setStreamUrl("");
        m_app.m_ui->setStreamClientCount(0);

        // Update StreamManager cooldown if available
        if (streamManager)
        {
            bool canStart = streamManager->canStartStreaming();
            int64_t cooldownMs = streamManager->getStreamingCooldownRemainingMs();
            m_app.m_ui->setCanStartStreaming(canStart);
            m_app.m_ui->setStreamingCooldownRemainingMs(cooldownMs);
        }
        else
        {
            // If no StreamManager, can start
            m_app.m_ui->setCanStartStreaming(true);
            m_app.m_ui->setStreamingCooldownRemainingMs(0);
        }
    }

    // Update recording status
    if (m_app.m_ui && m_app.m_recordingManager)
    {
        bool isRecording = m_app.m_recordingManager->isRecording();
        m_app.m_ui->setRecordingActive(isRecording);
        if (isRecording)
        {
            m_app.m_ui->setRecordingDurationUs(m_app.m_recordingManager->getCurrentDurationUs());
            m_app.m_ui->setRecordingFileSize(m_app.m_recordingManager->getCurrentFileSize());
            m_app.m_ui->setRecordingFilename(m_app.m_recordingManager->getCurrentFilename());
        }
    }

    // Render UI (after capturing capture area)
    if (m_app.m_ui)
    {
        m_app.m_ui->render();
        // IMPORTANT: endFrame() must be called BEFORE swapBuffers()
        // so UI is rendered to correct buffer
        m_app.m_ui->endFrame();
    }

    // Check if window is still valid before swapping
    if (m_app.m_window && !m_app.m_window->shouldClose())
    {
        m_app.m_window->swapBuffers();
    }

    // Pacing policy:
    //  - Remote source: vsync drives the loop at the panel's
    //    display refresh rate. Vsync is toggled on focus —
    //    when the window is backgrounded a lot of compositors
    //    park vsync at 0 Hz, so swapBuffers would block forever
    //    and the main loop would stop draining the frame queue
    //    (the user-observed "consumed=0 drops=60/s queueDepth=20"
    //    stall). When unfocused we disable vsync and add a
    //    small sleep to avoid burning CPU on hidden refreshes.
    //  - Local source streaming: keep the existing software
    //    pace at the configured streaming FPS so we don't
    //    burn GPU + glReadPixels for frames the encoder will
    //    just throttle.
    if (m_app.m_ui && m_app.m_ui->getSourceType() == UIManager::SourceType::Remote)
    {
        const bool focused = m_app.m_window && m_app.m_window->isFocused();
        if (m_app.m_window && focused != m_app.m_remoteWindowFocused)
        {
            m_app.m_window->setVsync(focused);
            m_app.m_remoteWindowFocused = focused;
        }
        if (!focused)
        {
            // Hidden / backgrounded — sleep a frame's worth so
            // captureLatestFrame still drains the queue at a
            // reasonable rate without spinning the CPU.
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            usleep(16000);
#else
            Sleep(16);
#endif
        }
        // When focused, vsync does the pacing.
    }
    else
    {
        bool streamingActive = (m_app.m_streamManager && m_app.m_streamManager->isActive());
        if (streamingActive && m_app.m_ui)
        {
            const uint32_t targetFps = m_app.m_ui->getStreamingFps() > 0 ? m_app.m_ui->getStreamingFps() : 60;
            const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(targetFps);
            if (m_app.m_lastFrameSwapUs == 0) m_app.m_lastFrameSwapUs = nowUs;
            const int64_t elapsedUs = nowUs - m_app.m_lastFrameSwapUs;
            if (elapsedUs < targetIntervalUs)
            {
                const int64_t sleepUs = targetIntervalUs - elapsedUs;
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                usleep(static_cast<useconds_t>(sleepUs));
#else
                Sleep(static_cast<DWORD>(sleepUs / 1000));
#endif
            }
            m_app.m_lastFrameSwapUs = nowUs + std::max<int64_t>(0, targetIntervalUs - elapsedUs);
        }
    }
    return false; // normal path: run() proceeds to the UI render + pacing tail
}
