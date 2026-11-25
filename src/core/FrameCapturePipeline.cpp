#include "FrameCapturePipeline.h"
#include "../utils/LoopProfiler.h"
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
#include "../processing/SourceCrop.h"
#include "../processing/UpscalerStage.h"
#include "../renderer/OpenGLRenderer.h"
#include "../renderer/PBOManager.h"
#include "../renderer/OutputGeometry.h"
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

namespace
{
// (Re)create an RGB colour render target of the given size. Used by the
// source pre-pass (crop) and the shader-source rescale pass.
void ensureColorTarget(GLuint &fbo, GLuint &tex, uint32_t &curW, uint32_t &curH,
                       uint32_t w, uint32_t h)
{
    if (fbo != 0 && tex != 0 && curW == w && curH == h)
        return;
    if (tex != 0)
    {
        glDeleteTextures(1, &tex);
        tex = 0;
    }
    if (fbo != 0)
    {
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                 static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    curW = w;
    curH = h;
}
} // namespace

void FrameCapturePipeline::getSourceCropFractions(uint32_t srcW, uint32_t srcH,
                                                  float &left, float &right,
                                                  float &top, float &bottom) const
{
    left = right = top = bottom = 0.0f;
    if (!m_app.m_ui || srcW == 0 || srcH == 0)
        return;
    m_app.m_ui->getSourceCrop().fractions(srcW, srcH, left, right, top, bottom);
    // A Remote source's texture is stored the other way up (see the
    // shouldFlipY note below): its row 0 is the BOTTOM of the picture.
    if (m_app.m_ui->getSourceType() == UIManager::SourceType::Remote)
        std::swap(top, bottom);
}

void FrameCapturePipeline::runAutoCropSampling()
{
    // #225 — auto-crop: sample a few frames of the UNCROPPED source, detect
    // the black borders on each, and keep the smallest border per side (a
    // letterbox is black on every frame; a dark scene is not a border).
    if (!m_app.m_ui || !m_app.m_frameProcessor)
        return;
    int threshold = 0;
    if (m_app.m_ui->takeSourceAutoCropRequest(threshold))
    {
        m_autoCrop = AutoCropSampler{};
        m_autoCrop.active = true;
        m_autoCrop.threshold = threshold;
    }
    if (!m_autoCrop.active)
        return;

    const GLuint tex = m_app.m_frameProcessor->getTexture();
    const uint32_t w = m_app.m_frameProcessor->getTextureWidth();
    const uint32_t h = m_app.m_frameProcessor->getTextureHeight();
    const uint64_t seq = m_app.m_frameProcessor->getFrameSeq();
    if (tex == 0 || w == 0 || h == 0)
        return;
    if (m_autoCrop.samples > 0 && (w != m_autoCrop.w || h != m_autoCrop.h))
    {
        // Source resized mid-sampling: start over on the new size.
        const int t = m_autoCrop.threshold;
        m_autoCrop = AutoCropSampler{};
        m_autoCrop.active = true;
        m_autoCrop.threshold = t;
    }
    // Space the samples out a little so they see different frames.
    if (m_autoCrop.samples > 0 && seq < m_autoCrop.lastSeq + AutoCropSampler::kSeqSpacing)
        return;
    m_autoCrop.lastSeq = seq;
    m_autoCrop.w = w;
    m_autoCrop.h = h;

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool ok = false;
    BlackBorders b;
    bool found = false;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
    {
        const size_t stride = ((static_cast<size_t>(w) * 3 + 3) / 4) * 4;
        m_autoCrop.pixels.resize(stride * h);
        glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                     GL_RGB, GL_UNSIGNED_BYTE, m_autoCrop.pixels.data());
        found = detectBlackBorders(m_autoCrop.pixels.data(), w, h, stride,
                                   m_autoCrop.threshold, b);
        ok = true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glDeleteFramebuffers(1, &fbo);
    if (!ok)
        return;

    ++m_autoCrop.samples;
    if (found)
    {
        // Readback row 0 is texture row 0: the top of the picture, except
        // for a Remote source (stored bottom-up).
        if (m_app.m_ui->getSourceType() == UIManager::SourceType::Remote)
            std::swap(b.top, b.bottom);
        if (m_autoCrop.found == 0)
        {
            m_autoCrop.result = b;
        }
        else
        {
            m_autoCrop.result.top = std::min(m_autoCrop.result.top, b.top);
            m_autoCrop.result.bottom = std::min(m_autoCrop.result.bottom, b.bottom);
            m_autoCrop.result.left = std::min(m_autoCrop.result.left, b.left);
            m_autoCrop.result.right = std::min(m_autoCrop.result.right, b.right);
        }
        ++m_autoCrop.found;
    }

    if (m_autoCrop.samples >= AutoCropSampler::kSamples)
    {
        const BlackBorders &r = m_autoCrop.result;
        LOG_INFO("Auto-crop: " + std::to_string(m_autoCrop.found) + "/" +
                 std::to_string(m_autoCrop.samples) + " frames with picture, borders top=" +
                 std::to_string(r.top) + " bottom=" + std::to_string(r.bottom) +
                 " left=" + std::to_string(r.left) + " right=" + std::to_string(r.right) +
                 " px on " + std::to_string(w) + "x" + std::to_string(h));
        m_app.m_ui->applySourceAutoCrop(m_autoCrop.found > 0, r.top, r.bottom, r.left, r.right, w, h);
        m_autoCrop.active = false;
        m_autoCrop.pixels.clear();
        m_autoCrop.pixels.shrink_to_fit();
    }
}

void FrameCapturePipeline::upscalerTarget(uint32_t refW, uint32_t refH,
                                          uint32_t &tw, uint32_t &th) const
{
    // Auto = the output resolution when set, else the window.
    uint32_t autoW = m_app.m_outputWidth, autoH = m_app.m_outputHeight;
    if (autoW == 0 || autoH == 0)
    {
        autoW = m_app.m_window ? m_app.m_window->getWidth() : m_app.m_windowWidth;
        autoH = m_app.m_window ? m_app.m_window->getHeight() : m_app.m_windowHeight;
    }
    const bool fitAspect = m_app.m_maintainAspect ||
                           (m_app.m_ui && m_app.m_ui->getOutputGeometry().forcesAspect());
    m_upscalerSettings.targetSize(refW, refH, autoW, autoH, fitAspect, tw, th);
}

GLuint FrameCapturePipeline::runUpscaler(UpscalerSettings::Position where, GLuint tex,
                                         uint32_t &w, uint32_t &h,
                                         uint32_t refW, uint32_t refH)
{
    if (!m_app.m_upscaler || !UpscalerStage::runsAt(m_upscalerSettings, where))
        return tex;
    uint32_t tw = 0, th = 0;
    upscalerTarget(refW, refH, tw, th);
    // Custom target: the stage clamps it the same way; pass it as "auto"
    // so the stage computes exactly the size chosen here.
    UpscalerSettings s = m_upscalerSettings;
    s.target = UpscalerSettings::Target::Auto;
    const GLuint out = m_app.m_upscaler->process(s, where, tex, w, h, refW, refH,
                                                 tw, th, /*fitAspect=*/false);
    if (out != tex)
        m_upscalerRan = true;
    return out;
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
    
    // #225 — auto-crop sampling reads the uncropped capture texture.
    runAutoCropSampling();

    // #224 — source pre-pass: the crop (overscan) is a property of the
    // SOURCE, so it runs here, before and independently of the shader
    // chain. The cropped texture then feeds the shader (when one is
    // active) or is rendered directly (when not), and the /raw source
    // readback below reads it too — so the preview, /stream, recording,
    // virtual camera and /raw all honour the crop with the shader on or
    // off. The crop zooms the visible region back to the source size, so
    // downstream dimensions don't change.
    GLuint sourceTex = m_app.m_frameProcessor->getTexture();
    uint32_t sourceW = m_app.m_frameProcessor->getTextureWidth();
    uint32_t sourceH = m_app.m_frameProcessor->getTextureHeight();
    {
        float cropL = 0.0f, cropR = 0.0f, cropT = 0.0f, cropB = 0.0f;
        getSourceCropFractions(sourceW, sourceH, cropL, cropR, cropT, cropB);
        const bool needsCrop = (cropL > 0.0f || cropR > 0.0f || cropT > 0.0f || cropB > 0.0f);
        if (needsCrop && sourceTex != 0 && sourceW > 0 && sourceH > 0)
        {
            ensureColorTarget(m_app.m_sourceCropFBO, m_app.m_sourceCropTexture,
                              m_app.m_sourceCropFBOWidth, m_app.m_sourceCropFBOHeight,
                              sourceW, sourceH);
            glBindFramebuffer(GL_FRAMEBUFFER, m_app.m_sourceCropFBO);

            // Enlarge the viewport so only the visible region of the source
            // lands inside the FBO. FBO row 0 is texture row 0, i.e. the
            // TOP of the picture (the window render flips Y), so the top
            // crop pushes the viewport below y=0 and the bottom crop past
            // the FBO height.
            const float visibleFracX = 1.0f - cropL - cropR;
            const float visibleFracY = 1.0f - cropT - cropB;
            const float vpW = static_cast<float>(sourceW) / visibleFracX;
            const float vpH = static_cast<float>(sourceH) / visibleFracY;
            const GLint vpX = static_cast<GLint>(-cropL * vpW);
            const GLint vpY = static_cast<GLint>(-cropT * vpH);
            glViewport(vpX, vpY, static_cast<GLsizei>(vpW), static_cast<GLsizei>(vpH));

            // NEAREST keeps the pixelated look of retro sources (same
            // filter the pre-#224 shader-branch crop used); the
            // FrameProcessor's filter is restored afterwards.
            const GLint restoreFilter = m_app.m_frameProcessor->getTextureFilterLinear()
                                            ? GL_LINEAR
                                            : GL_NEAREST;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sourceTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            m_app.m_renderer->renderTexture(sourceTex, sourceW, sourceH,
                                            false, false, 1.0f, 1.0f, false,
                                            sourceW, sourceH,
                                            /*preserveViewport=*/true);

            glBindTexture(GL_TEXTURE_2D, sourceTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, restoreFilter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, restoreFilter);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sourceTex = m_app.m_sourceCropTexture;
        }
    }

    // #251 — the built-in upscaler's settings, one copy for every pass of
    // this iteration (live and generated run the same stage).
    m_upscalerSettings = m_app.m_ui ? m_app.m_ui->getUpscalerSettings() : UpscalerSettings();

    // #198 — identity + capture time of the live frame.
    const FrameMeta liveMeta{m_app.m_frameProcessor ? m_app.m_frameProcessor->getFrameSeq() : 0,
                             m_app.m_frameProcessor ? m_app.m_frameProcessor->getFrameTimestampUs() : 0};

    // #247 — frame generation. Off (the default) or idle: one pass over the
    // live frame feeding the window and every output, exactly as before.
    // Active: the outputs that get generated frames are fed by a pass over
    // the output timeline; the others (and /raw, the remote clients' own
    // source) keep the live frame, so a latency-sensitive window can stay
    // on real frames while the stream / recording get the generated ones.
    PassSpec live;
    live.srcTex = sourceTex;
    live.srcW = sourceW;
    live.srcH = sourceH;
    live.meta = liveMeta;
    live.pbo = m_app.m_pboManager.get();
    live.pboInFlight = &m_pboInFlight;

    FrameGenPlan plan;
    {
        LoopProfiler::Scope prof(LoopProfiler::FrameGen);
        plan = planFrameGeneration(sourceTex, sourceW, sourceH, liveMeta);
    }
    if (!plan.active)
    {
        if (renderPass(live))
            return true;
    }
    else
    {
        PassSpec gen;
        gen.srcTex = plan.frame.tex;
        gen.srcW = plan.frame.w;
        gen.srcH = plan.frame.h;
        gen.meta = FrameMeta{plan.frame.seq, plan.frame.tsUs, plan.streamPhaseOk, plan.recordPhaseOk};
        gen.window = plan.genWindow;
        gen.stream = plan.genStream;
        gen.record = plan.genRecord;
        gen.virtcam = plan.genVirtcam;
        gen.raw = false;
        if (!m_genPbo)
            m_genPbo = std::make_unique<PBOManager>();
        gen.pbo = m_genPbo.get();
        gen.pboInFlight = &m_genPboInFlight;

        live.window = !plan.genWindow;
        live.stream = plan.liveStream;
        live.record = plan.liveRecord;
        live.virtcam = plan.liveVirtcam;
        live.raw = true;
        const bool liveNeeded = live.window || plan.liveOutputsActive;

        // The chain's temporal state (FrameCount, feedback, history) follows
        // the window's sequence; the other run of this iteration is frozen.
        // #251 — the upscaler's own engine is frozen in lockstep, so its
        // FrameCount doesn't advance twice per iteration either.
        ShaderEngine *se = m_app.m_shaderEngine.get();
        UpscalerStage *us = m_app.m_upscaler.get();
        auto freeze = [se, us](bool frozen) {
            if (se) se->setStateFrozen(frozen);
            if (us) us->setStateFrozen(frozen);
        };
        bool early = false;
        if (liveNeeded)
        {
            if (!live.window) freeze(true);
            early = renderPass(live);
            freeze(false);
        }
        if (!early)
        {
            if (!gen.window) freeze(true);
            early = renderPass(gen);
            freeze(false);
        }
        if (early)
            return true;
    }

    return finishFrame();
}

bool FrameCapturePipeline::renderPass(const PassSpec &spec)
{
    // #247 — one render of a source frame through the shader chain to the
    // window (spec.window) and/or the outputs the spec selects. With every
    // output selected and the live frame this is the pre-#247 body as is.
    const GLuint sourceTex = spec.srcTex;
    const uint32_t sourceW = spec.srcW;
    const uint32_t sourceH = spec.srcH;
    const bool passStream = spec.stream && m_app.m_streamManager && m_app.m_streamManager->isActive();
    const bool passRaw = spec.raw && m_app.m_streamManager && m_app.m_streamManager->isActive();
    const bool passRecord = spec.record && m_app.m_recordingManager && m_app.m_recordingManager->isRecording();
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    const bool passVirtcam = spec.virtcam && m_app.m_virtcam && m_app.m_virtcam->isRunning();
#else
    const bool passVirtcam = false;
#endif
    PBOManager *const pbo = spec.pbo;
    FrameMeta &pboInFlight = *spec.pboInFlight;

    // Apply shader if active
    GLuint textureToRender = sourceTex;
    bool isShaderTexture = false;
    // Size of textureToRender when isShaderTexture (shader chain and / or
    // the #251 upscaler produced it).
    uint32_t processedW = 0, processedH = 0;

    // #251 — set by runUpscaler() when the upscaler produced this pass's image.
    m_upscalerRan = false;

    // Master toggle: when the user disables the shader pipeline
    // from the Shader tab we keep the loaded preset / parameters
    // intact but stop running the chain on the live render. The
    // captured texture for stream/recording then reflects the
    // (cropped) source — same path the user sees on the window.
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

        // Source para o shader chain: a textura (já cortada) do pre-pass.
        // Se o usuário pediu uma resolução LÓGICA menor que a que o V4L2
        // entregou (driver ajustou pra mais próxima suportada), fazemos um
        // downscale com filter=NEAREST aqui — assim shaders CRT recebem
        // entrada "pixelada" baixa-res como foram desenhados, em vez de
        // 1080p suave onde scanlines viram sub-pixel e somem.
        GLuint shaderSrcTex = sourceTex;
        uint32_t shaderSrcW = sourceW;
        uint32_t shaderSrcH = sourceH;

        // Logical size the shader should see. Local capture: what the user
        // asked for (the driver may have picked something bigger). Remote
        // source: the host's source size from /meta (#209) — the client
        // decoder only downscales, so a larger host source is upscaled
        // here on the GPU instead of per frame on the CPU.
        uint32_t wantW = m_app.m_logicalCaptureWidth;
        uint32_t wantH = m_app.m_logicalCaptureHeight;
        const bool remoteSource = m_app.m_ui && m_app.m_ui->getSourceType() == UIManager::SourceType::Remote;
        if (remoteSource)
        {
            wantW = wantH = 0;
            if (auto *remote = dynamic_cast<VideoCaptureRemote *>(m_app.m_capture.get()))
            {
                remote->getTargetResolution(wantW, wantH);
            }
        }
        const bool needsDownscale = (wantW > 0 && wantH > 0 &&
                                     wantW < shaderSrcW &&
                                     wantH < shaderSrcH &&
                                     shaderSrcTex != 0);
        const bool needsUpscale = (remoteSource && wantW > 0 && wantH > 0 &&
                                   (wantW > shaderSrcW || wantH > shaderSrcH) &&
                                   shaderSrcTex != 0);

        if ((needsDownscale || needsUpscale) && shaderSrcTex != 0)
        {
            const uint32_t fboW = wantW;
            const uint32_t fboH = wantH;
            ensureColorTarget(m_app.m_shaderSourceFBO, m_app.m_shaderSourceTexture,
                              m_app.m_shaderSourceFBOWidth, m_app.m_shaderSourceFBOHeight,
                              fboW, fboH);

            glBindFramebuffer(GL_FRAMEBUFFER, m_app.m_shaderSourceFBO);
            glViewport(0, 0, static_cast<GLsizei>(fboW), static_cast<GLsizei>(fboH));

            // Forçar NEAREST na textura source pra preservar look pixelado
            // no downscale, e restaurar o filtro original depois (o da
            // FrameProcessor, ou NEAREST pra textura do crop pre-pass).
            // Remote upscale (#209) uses LINEAR, matching the bilinear CPU
            // upscale it replaces.
            const GLint scaleFilter = needsUpscale ? GL_LINEAR : GL_NEAREST;
            const GLint restoreFilter = (shaderSrcTex == m_app.m_frameProcessor->getTexture() &&
                                         m_app.m_frameProcessor->getTextureFilterLinear())
                                            ? GL_LINEAR
                                            : GL_NEAREST;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, shaderSrcTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, scaleFilter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, scaleFilter);

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

        // #251 — upscaler. Before: the chain runs on the upscaled image.
        // After: the chain sees the native source and renders at the
        // largest whole multiple of it that fits the upscale target; the
        // upscaler then takes the chain's output to the target.
        const uint32_t nativeW = shaderSrcW, nativeH = shaderSrcH;
        shaderSrcTex = runUpscaler(UpscalerSettings::Position::Before, shaderSrcTex,
                                   shaderSrcW, shaderSrcH, nativeW, nativeH);
        if (m_app.m_upscaler && nativeW > 0 && nativeH > 0 &&
            UpscalerStage::runsAt(m_upscalerSettings, UpscalerSettings::Position::After))
        {
            uint32_t tw = 0, th = 0;
            upscalerTarget(nativeW, nativeH, tw, th);
            const uint32_t k = std::max<uint32_t>(1, std::min(tw / nativeW, th / nativeH));
            m_app.m_shaderEngine->setViewport(nativeW * k, nativeH * k);
        }

        textureToRender = m_app.m_shaderEngine->applyShader(shaderSrcTex, shaderSrcW, shaderSrcH);
        isShaderTexture = true;
        processedW = m_app.m_shaderEngine->getOutputWidth();
        processedH = m_app.m_shaderEngine->getOutputHeight();
        
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
            LOG_WARN("Shader returned invalid texture (0), using source texture");
            textureToRender = sourceTex;
            isShaderTexture = false;
        }
        else if (processedW > 0 && processedH > 0)
        {
            // #251 — upscaler after the chain.
            textureToRender = runUpscaler(UpscalerSettings::Position::After, textureToRender,
                                          processedW, processedH, nativeW, nativeH);
        }
    }
    else if (m_upscalerSettings.active() && sourceTex != 0 && sourceW > 0 && sourceH > 0)
    {
        // #251 — no shader chain: the upscaler alone (either position).
        uint32_t w = sourceW, h = sourceH;
        const GLuint up = runUpscaler(m_upscalerSettings.position, sourceTex, w, h, sourceW, sourceH);
        if (m_upscalerRan)
        {
            textureToRender = up;
            isShaderTexture = true;
            processedW = w;
            processedH = h;
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
        {
            LoopProfiler::Scope prof(LoopProfiler::Swap);
            m_app.m_window->swapBuffers();
        }
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
    if (spec.window)
    {
        if (isShaderTexture)
        {
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Transparent for shaders with alpha
        }
        else
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Opaque for normal capture
        }
        glClear(GL_COLOR_BUFFER_BIT);
    }

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
        // Without maintainAspect, use shader (/ upscaler, #251) output dimensions
        renderWidth = processedW;
        renderHeight = processedH;
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
    // Exception: a Remote source. The /raw wire feed arrives
    // already inverted relative to a locally-captured frame, so
    // the renderer's implicit flip overshoots and the image lands
    // upside-down. This holds in BOTH the shader and non-shader
    // cases — the host now reads /raw bottom-up (#187), matching
    // its shaded-output orientation, so the client must drop the
    // flip for any Remote source regardless of the client-side
    // shader pipeline (#67, #187). Previously the flip was dropped
    // only when no client-side shader ran, which left a Remote +
    // client-shader frame upside-down once /raw was made consistent.
    const bool remoteSource =
        (m_app.m_ui && m_app.m_ui->getSourceType() == UIManager::SourceType::Remote);
    bool shouldFlipY = !remoteSource;

    // Calculate viewport where capture will be rendered (may be smaller than window if maintainAspect is active)
    uint32_t windowWidth = m_app.m_window->getWidth();
    uint32_t windowHeight = m_app.m_window->getHeight();

    // #227 — output geometry: integer scaling / aspect override pick the
    // window rect, rotation / mirror go in as a texcoord transform. With
    // the defaults this is the previous maintain-aspect fit (or stretch).
    const OutputGeometry geom = m_app.m_ui ? m_app.m_ui->getOutputGeometry() : OutputGeometry();
    const OutputGeometry::Rect winRect =
        geom.windowRect(finalRenderWidth, finalRenderHeight,
                        static_cast<int>(windowWidth), static_cast<int>(windowHeight),
                        m_app.m_maintainAspect);
    GLint viewportX = winRect.x;
    GLint viewportY = winRect.y;
    GLsizei viewportWidth = winRect.w;
    GLsizei viewportHeight = winRect.h;

    // #247 — only the pass that owns the window draws it (the body keeps
    // its original indentation to keep the diff small).
    if (spec.window) {
    // IMPORTANTE: Garantir que estamos renderizando no framebuffer padrão (janela)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Integer scaling is about pixel-exact presentation: sample the final
    // texture with NEAREST for this one draw and restore its filter after.
    const bool nearestPresent = geom.integerScale != OutputGeometry::IntegerScale::Off;
    GLint prevMinFilter = GL_LINEAR, prevMagFilter = GL_LINEAR;
    if (nearestPresent)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, finalTexture);
        if (glGetTexParameteriv)
        {
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &prevMinFilter);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &prevMagFilter);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (geom.hasOrientation())
    {
        float uv[4];
        geom.uvMatrix(/*yDown=*/false, uv);
        m_app.m_renderer->setUVTransform(uv);
    }

    // Renderizar textura final na janela.
    // enableBlend=false pelo mesmo motivo do resize acima.
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    m_app.m_renderer->renderTexture(finalTexture, windowWidth, windowHeight,
                              shouldFlipY, false, m_app.m_brightness, m_app.m_contrast,
                              false, finalRenderWidth, finalRenderHeight,
                              /*preserveViewport=*/true);

    m_app.m_renderer->resetUVTransform();
    if (nearestPresent)
    {
        glBindTexture(GL_TEXTURE_2D, finalTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, prevMinFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, prevMagFilter);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    } // spec.window

    // IMPORTANTE: Para streaming e recording, capturar diretamente da textura final ao invés do framebuffer
    // Isso evita problemas com back/front buffer e garante que capturamos a imagem renderizada
    bool needsFrameCapture = passStream || passRaw || passRecord || passVirtcam;

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
        // #198 — the captured frame currently on screen. Synchronous
        // readbacks are of this frame; the async PBO one is of the frame
        // read back on the previous call (see m_pboInFlight).
        const FrameMeta curMeta = spec.meta;

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
                bool needsFrameCapture = passRecord || passStream || passRaw || passVirtcam;
                
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
                        captureTextureWidth = processedW;
                        captureTextureHeight = processedH;
                        
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
                //
                // #227 — the same pass applies the output geometry
                // (aspect override, rotation / mirror). The image is
                // fitted into a canvas of the size the encoder will
                // resize to (output resolution, else the stream /
                // recording size), so the encoder's plain resize keeps
                // the rotated / re-aspected image undistorted. Integer
                // scaling is window-only and never reaches here.
                static GLuint postImageFBO     = 0;
                static GLuint postImageTex     = 0;
                static uint32_t postImageW     = 0;
                static uint32_t postImageH     = 0;
                const bool geomActive = geom.affectsOutputs();
                const bool postImageActive =
                    (m_app.m_brightness != 1.0f) || (m_app.m_contrast != 1.0f) || geomActive;

                uint32_t passW = captureTextureWidth;
                uint32_t passH = captureTextureHeight;
                OutputGeometry::Rect passRect;
                passRect.w = static_cast<int>(passW);
                passRect.h = static_cast<int>(passH);
                if (geomActive)
                {
                    uint32_t targetW = 0, targetH = 0;
                    if (m_app.m_outputWidth > 0 && m_app.m_outputHeight > 0)
                    {
                        targetW = m_app.m_outputWidth;
                        targetH = m_app.m_outputHeight;
                    }
                    else if (m_app.m_streamManager && m_app.m_streamManager->isActive() &&
                             m_app.m_streamingWidth > 0 && m_app.m_streamingHeight > 0)
                    {
                        targetW = m_app.m_streamingWidth;
                        targetH = m_app.m_streamingHeight;
                    }
                    else if (m_app.m_recordingManager && m_app.m_recordingManager->isRecording())
                    {
                        const RecordingSettings rs = m_app.m_recordingManager->getRecordingSettings();
                        targetW = rs.width;
                        targetH = rs.height;
                    }
                    geom.outputCanvas(finalRenderWidth, finalRenderHeight, targetW, targetH,
                                      passW, passH, passRect);
                }

                GLuint fboTextureToAttach = textureToCapture;
                if (postImageActive)
                {
                    if (postImageFBO == 0 || postImageTex == 0 ||
                        postImageW != passW ||
                        postImageH != passH)
                    {
                        if (postImageFBO) glDeleteFramebuffers(1, &postImageFBO);
                        if (postImageTex) glDeleteTextures(1, &postImageTex);
                        postImageFBO = 0; postImageTex = 0;
                        glGenTextures(1, &postImageTex);
                        glBindTexture(GL_TEXTURE_2D, postImageTex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                     passW,
                                     passH, 0,
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
                            postImageW = passW;
                            postImageH = passH;
                        }
                    }
                    if (postImageFBO && postImageTex)
                    {
                        glBindFramebuffer(GL_FRAMEBUFFER, postImageFBO);
                        glViewport(0, 0, postImageW, postImageH);
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        glViewport(passRect.x, passRect.y, passRect.w, passRect.h);
                        if (geom.hasOrientation())
                        {
                            // The read-back is consumed top-down (row 0
                            // = image top), so this FBO's visual space is
                            // y-down, except for a Remote frame, which is
                            // stored the other way round and flipped on
                            // read-back (#210).
                            float uv[4];
                            geom.uvMatrix(/*yDown=*/!remoteSource, uv);
                            m_app.m_renderer->setUVTransform(uv);
                        }
                        m_app.m_renderer->renderTexture(
                            textureToCapture,
                            postImageW, postImageH,
                            /*flipY=*/false, /*enableBlend=*/false,
                            m_app.m_brightness, m_app.m_contrast,
                            /*maintainAspect=*/false,
                            captureTextureWidth, captureTextureHeight,
                            /*preserveViewport=*/true);
                        m_app.m_renderer->resetUVTransform();
                        fboTextureToAttach = postImageTex;
                        captureTextureWidth = postImageW;
                        captureTextureHeight = postImageH;
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
                        // #198 — once per captured frame, with its capture time.
                        if (passStream && curMeta.streamOk && curMeta.seq != m_lastStreamSeq)
                        {
                            m_lastStreamSeq = curMeta.seq;
                            m_app.m_streamManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight, curMeta.tsUs);
                        }
                        if (passRecord && curMeta.recordOk && curMeta.seq != m_lastRecordSeq)
                        {
                            m_lastRecordSeq = curMeta.seq;
                            m_app.m_recordingManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight, curMeta.tsUs);
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
                        // #251 — the upscaler's output is RGBA too.
                        bool isShaderTexture = (pipelineEnabled && m_app.m_shaderEngine && m_app.m_shaderEngine->isShaderActive()) ||
                                               m_upscalerRan;
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
                    // Which captured frame frameData holds (#198).
                    FrameMeta dataMeta = curMeta;

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
                    bool useAsyncPBO = (allowPBO && pbo &&
                                        pbo->init(textureWidth, textureHeight, readFormat) &&
                                        pbo->isInitialized());

                    if (useAsyncPBO)
                    {
                        // #198 — getReadData() below returns the readback
                        // started on the previous call: label it with the
                        // frame that readback was of.
                        dataMeta = pboInFlight;
                        pboInFlight = curMeta;
                        // Agenda glReadPixels no PBO (não bloqueia).
                        pbo->startAsyncRead(0, 0,
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
                            // #210 — same rule as the window draw
                            // (shouldFlipY): a remote frame arrives in the
                            // opposite orientation, so flip it here too or
                            // client recordings / virtcam are upside-down.
                            frameDataReady = pbo->getReadData(
                                frameData.data(), textureWidth, textureHeight, /*flipY=*/remoteSource);
                            // #85 — Virtual camera also gets fed from
                            // the RGB path (no-shader / direct capture
                            // passthrough). Without this branch the
                            // consumer sees uninitialised buffer
                            // contents (black / static) whenever a
                            // shader is off, which was the symptom
                            // user hit on first end-to-end test.
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
                            if (frameDataReady && passVirtcam)
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
                            if (pbo->getReadData(rgbaData.data(),
                                                          textureWidth, textureHeight,
                                                          /*flipY=*/remoteSource)) // #210
                            {
                                // #85 — Virtual camera piggybacks on
                                // this RGBA readback (shader path).
                                // Push happens BEFORE the RGB strip
                                // below so the sink keeps the alpha
                                // channel intact (sws RGBA → YUYV).
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
                                if (passVirtcam)
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
                            // #210 — flip remote frames, like the window draw does.
                            const uint32_t srcRow = remoteSource ? (textureHeight - 1 - row) : row;
                            const uint8_t *srcPtr = frameDataWithPadding.data() + (srcRow * syncRowPadded);
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
                        // pre-shader source instead. We do a sync readback of the
                        // source pre-pass texture (the capture upload, cropped by
                        // the source crop when one is set — #224) only
                        // when at least one active pipeline asked for it — keeps the
                        // common case (both pipelines follow the master) on the fast
                        // single-readback path.
                        const bool masterOn = (m_app.m_ui && m_app.m_ui->getShaderPipelineEnabled());
                        const bool shaderActive = (m_app.m_shaderEngine && m_app.m_shaderEngine->isShaderActive());
                        // #251 — the output is processed (shader chain and / or the
                        // upscaler): /raw and an apply-shader-off stream / recording
                        // then need the separate pre-shader source readback.
                        const bool processed = (masterOn && shaderActive) || m_upscalerRan;
                        const bool streamWantsSource = m_app.m_ui && passStream &&
                                                       !m_app.m_ui->getStreamingApplyShader();
                        const bool recordWantsSource = m_app.m_ui && passRecord &&
                                                       !m_app.m_ui->getRecordingApplyShader();
                        // Phase 2 of #47: /raw is by-contract pre-shader, so any connected
                        // /raw client also triggers the source-frame capture below.
                        const bool rawWantsSource = passRaw && m_app.m_streamManager->hasRawClients();
                        const bool needSourceCapture = processed &&
                                                       (streamWantsSource || recordWantsSource || rawWantsSource);

                        bool sourceFrameReady = false;
                        uint32_t sourceFrameW = 0, sourceFrameH = 0;
                        if (needSourceCapture && m_app.m_frameProcessor)
                        {
                            // #224 — /raw carries the source crop: read the
                            // pre-pass texture, not the raw capture texture.
                            GLuint srcTex = sourceTex;
                            sourceFrameW = sourceW;
                            sourceFrameH = sourceH;
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
                                    // #187 — keep glReadPixels' native bottom-up order (do NOT flip),
                                    // only strip the row padding. The shaded path (frameData) is read via
                                    // PBO with flipY=false (also bottom-up) and records/streams correctly;
                                    // this raw-source buffer feeds the same encoders (recording, /stream,
                                    // /raw), so it must match that orientation. The old vertical flip here
                                    // produced upside-down recording whenever apply-shader was OFF.
                                    m_app.m_captureSourceFrameData.resize(rowUnpadded * sourceFrameH);
                                    for (uint32_t row = 0; row < sourceFrameH; ++row)
                                    {
                                        const uint32_t srcRow = row;
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
                        if (frameDataReady && (passStream || passRaw))
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
                            //
                            // #198 — push each captured frame once, stamped
                            // with its capture time (the source readback is
                            // synchronous → current frame; frameData may be
                            // the previous frame on the async PBO path).
                            const FrameMeta streamMeta = useSource ? curMeta : dataMeta;
                            if (passStream && streamMeta.streamOk && m_app.m_streamManager->hasClients() &&
                                streamMeta.seq != m_lastStreamSeq)
                            {
                                m_lastStreamSeq = streamMeta.seq;
                                if (useSource)
                                {
                                    m_app.m_streamManager->pushFrame(m_app.m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH, streamMeta.tsUs);
                                }
                                else
                                {
                                    m_app.m_streamManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight, streamMeta.tsUs);
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
                            if (passRaw && m_app.m_streamManager->hasRawClients())
                            {
                                if (sourceFrameReady && curMeta.seq != m_lastRawSeq)
                                {
                                    m_lastRawSeq = curMeta.seq;
                                    m_app.m_streamManager->pushRawFrame(m_app.m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH, curMeta.tsUs);
                                }
                                else if (!sourceFrameReady && !processed &&
                                         dataMeta.seq != m_lastRawSeq)
                                {
                                    m_lastRawSeq = dataMeta.seq;
                                    m_app.m_streamManager->pushRawFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight, dataMeta.tsUs);
                                }
                            }
                        }
                        if (frameDataReady && passRecord)
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
                            const FrameMeta recordMeta = useSource ? curMeta : dataMeta;
                            if (recordMeta.recordOk && recordMeta.seq != m_lastRecordSeq)
                            {
                                m_lastRecordSeq = recordMeta.seq;
                                if (useSource)
                                {
                                    m_app.m_recordingManager->pushFrame(m_app.m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH, recordMeta.tsUs);
                                }
                                else
                                {
                                    m_app.m_recordingManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight, recordMeta.tsUs);
                                }
                            }
                        }
                } // fim do else (textura válida)
            } // fim do else (FBO completo)
        } // fim do if (needsFrameCapture)
    } // fim do if (captureDataSize > 0)
    return false;
}

bool FrameCapturePipeline::finishFrame()
{
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

    // #226 — counters for the FPS overlay. Cheap reads (atomics + a
    // couple of streamer flags); the overlay derives the rates.
    if (m_app.m_ui)
    {
        PerfCounters pc;
        if (m_app.m_frameProcessor)
        {
            pc.captureSeq  = m_app.m_frameProcessor->getFrameSeq();
            pc.captureTsUs = m_app.m_frameProcessor->getFrameTimestampUs();
        }
        const bool remote = m_app.m_ui->getSourceType() == UIManager::SourceType::Remote;
        pc.captureNominalFps = static_cast<float>(remote ? m_app.m_remoteSourceFps
                                                         : m_app.m_ui->getCaptureFps());
        if (streamManager && streamManager->isActive())
        {
            pc.streamActive  = streamManager->hasClients();
            pc.streamEncoded = streamManager->getStreamVideoFramesEncoded();
            pc.rawActive     = streamManager->hasRawClients();
            pc.rawEncoded    = streamManager->getRawVideoFramesEncoded();
        }
        if (m_app.m_recordingManager)
        {
            pc.recordActive  = m_app.m_recordingManager->isRecording();
            pc.recordEncoded = m_app.m_recordingManager->getVideoFramesEncoded();
        }
        // #247 — output timeline, real vs generated.
        if (m_frameGen)
        {
            const FrameGenClock &clk = m_frameGen->clock();
            pc.genActive    = m_frameGen->isActive();
            pc.genReal      = clk.realEmitted();
            pc.genGenerated = clk.generatedEmitted();
            pc.genDropped   = clk.dropped();
            const UIManager::FrameGenStatus st = m_app.m_ui->getFrameGenStatus();
            pc.genTargetFps = st.outputFps;
        }
        m_app.m_ui->setPerfCounters(pc);
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
        {
            LoopProfiler::Scope prof(LoopProfiler::Swap);
            m_app.m_window->swapBuffers();
        }
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
