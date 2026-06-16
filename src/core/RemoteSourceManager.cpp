#include "RemoteSourceManager.h"
#include "Application.h"
#include "FrameCapturePipeline.h"
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

RemoteSourceManager::RemoteSourceManager(Application &app) : m_app(app)
{
}

void RemoteSourceManager::startWorker(const std::string &devicePath, const std::string *authToken)
{
    m_app.m_remoteMetaSync = std::make_unique<RemoteMetaSync>();
    if (authToken)
        m_app.m_remoteMetaSync->setAuthToken(*authToken);
    m_app.m_remoteMetaSync->start(devicePath,
        [this](const RemoteMetaSync::Snapshot &snap) { stageSnapshot(snap); });
}

void RemoteSourceManager::stageSnapshot(const RemoteMetaSync::Snapshot &snap)
{
    std::lock_guard<std::mutex> lock(m_app.m_pendingRemoteMutex);
    m_app.m_pendingRemotePreset          = snap.preset;
    m_app.m_pendingRemotePresetHash      = snap.presetHash;
    m_app.m_pendingRemotePipelineEnabled = snap.pipelineEnabled;
    m_app.m_pendingRemoteParams.clear();
    m_app.m_pendingRemoteParams.reserve(snap.parameters.size());
    for (const auto &p : snap.parameters)
    {
        m_app.m_pendingRemoteParams.emplace_back(p.name, p.value);
    }
    m_app.m_pendingRemoteSourceWidth  = snap.sourceWidth;
    m_app.m_pendingRemoteSourceHeight = snap.sourceHeight;
    // Image-tab values: seed only once per connection — see
    // applyPendingRemoteMeta for the gate. Stash the values
    // from this snapshot; if it's the first one we'll apply
    // them on the GL thread, otherwise the apply gate skips.
    if (!m_app.m_remoteImageSeeded)
    {
        m_app.m_pendingRemoteImageBrightness     = snap.imageBrightness;
        m_app.m_pendingRemoteImageContrast       = snap.imageContrast;
        m_app.m_pendingRemoteImageMaintainAspect = snap.imageMaintainAspect;
        m_app.m_pendingRemoteImageOutputWidth    = snap.imageOutputWidth;
        m_app.m_pendingRemoteImageOutputHeight   = snap.imageOutputHeight;
        m_app.m_hasPendingRemoteImageSeed        = true;
    }
    // Plain assignment — uint32_t is atomic on every platform
    // we target, and a momentarily stale read in the main
    // loop is harmless (just one extra render iteration at
    // most).
    if (snap.sourceFps > 0) m_app.m_remoteSourceFps = snap.sourceFps;
    m_app.m_hasPendingRemoteMeta.store(true);
    // Push the host's viewer count straight onto UIManager
    // (#68) — the OSD quick-actions widget reads it every
    // frame to render "watching with N others" in client
    // mode. Bypasses the pending-snapshot apply path
    // because there's no GL state involved.
    if (m_app.m_ui) m_app.m_ui->setRemoteUpstreamClientCount(snap.upstreamClientCount);
    // #84 — Chat: if the host advertised a roomSlug in
    // /meta, bind the local chat client to that room.
    // Empty roomSlug means the host has chat disabled or
    // hasn't picked a slug yet — close any session we
    // might have had from a previous host.
    if (m_app.m_chatClient)
    {
        const std::string &slug = snap.chatRoomSlug;
        if (!slug.empty() && slug != m_app.m_chatBoundSlug)
        {
            m_app.m_chatBoundSlug = slug;
            // Viewer nick: persistent chat name only.
            const std::string nick = m_app.m_ui
                ? m_app.m_ui->getChatNickname()
                : std::string{};
            m_app.m_chatClient->connectBySlug(slug, nick);
        }
        else if (slug.empty() && !m_app.m_chatBoundSlug.empty())
        {
            m_app.m_chatBoundSlug.clear();
            m_app.m_chatClient->disconnect();
        }
    }
}

void RemoteSourceManager::applyPendingRemoteMeta()
{
    if (!m_app.m_hasPendingRemoteMeta.load()) return;

    // Snapshot the pending values out from under the polling thread.
    std::string preset;
    std::string presetHash;
    bool pipelineEnabled = true;
    std::vector<std::pair<std::string, float>> params;
    uint32_t sourceW = 0, sourceH = 0;
    bool seedImage = false;
    float imgBrightness = 1.0f, imgContrast = 1.0f;
    bool imgMaintainAspect = true;
    uint32_t imgOutW = 0, imgOutH = 0;
    {
        std::lock_guard<std::mutex> lock(m_app.m_pendingRemoteMutex);
        preset           = std::move(m_app.m_pendingRemotePreset);
        presetHash       = std::move(m_app.m_pendingRemotePresetHash);
        pipelineEnabled  = m_app.m_pendingRemotePipelineEnabled;
        params           = std::move(m_app.m_pendingRemoteParams);
        sourceW          = m_app.m_pendingRemoteSourceWidth;
        sourceH          = m_app.m_pendingRemoteSourceHeight;
        if (m_app.m_hasPendingRemoteImageSeed)
        {
            seedImage         = true;
            imgBrightness     = m_app.m_pendingRemoteImageBrightness;
            imgContrast       = m_app.m_pendingRemoteImageContrast;
            imgMaintainAspect = m_app.m_pendingRemoteImageMaintainAspect;
            imgOutW           = m_app.m_pendingRemoteImageOutputWidth;
            imgOutH           = m_app.m_pendingRemoteImageOutputHeight;
            m_app.m_hasPendingRemoteImageSeed = false;
        }
        m_app.m_hasPendingRemoteMeta.store(false);
    }

    // Seed the local Image tab from the host on the very first snapshot
    // per connection. m_remoteImageSeeded flips true here and gates the
    // callback so subsequent snapshots leave these values alone — the
    // user is free to tweak the local Image controls after the initial
    // sync.
    if (seedImage && m_app.m_ui)
    {
        m_app.m_ui->setBrightness(imgBrightness);
        m_app.m_ui->setContrast(imgContrast);
        m_app.m_ui->setMaintainAspect(imgMaintainAspect);
        m_app.m_ui->setOutputResolution(imgOutW, imgOutH);
        m_app.m_remoteImageSeeded = true;
        LOG_INFO("RemoteMetaSync: seeded local Image from host — brightness=" +
                 std::to_string(imgBrightness) + " contrast=" +
                 std::to_string(imgContrast) + " maintainAspect=" +
                 std::to_string(imgMaintainAspect) + " output=" +
                 std::to_string(imgOutW) + "x" + std::to_string(imgOutH));
    }

    // Tell the remote capture to rescale to the host's source dims (if the
    // stream is encoded at a different size) and sync our render-size view
    // so downstream FBO / viewport calculations use the right values.
    if (sourceW > 0 && sourceH > 0)
    {
        if (auto *remote = dynamic_cast<VideoCaptureRemote *>(m_app.m_capture.get()))
        {
            remote->setTargetResolution(sourceW, sourceH);
        }
        if (sourceW != m_app.m_captureWidth || sourceH != m_app.m_captureHeight)
        {
            LOG_INFO("Remote source dims from /meta: " +
                     std::to_string(sourceW) + "x" + std::to_string(sourceH) +
                     " (was " + std::to_string(m_app.m_captureWidth) + "x" + std::to_string(m_app.m_captureHeight) + ")");
            m_app.m_captureWidth  = sourceW;
            m_app.m_captureHeight = sourceH;
        }
    }

    if (!m_app.m_shaderEngine || !m_app.m_ui)
    {
        return;
    }

    // Phase 4 minimum: resolve preset by name in the local shader library.
    // If the preset isn't there, log and keep whatever shader the client
    // already has — Phase 4b will fetch the bundle from /meta/shader-bundle
    // and cache it locally for these cases.
    bool reloaded = false;
    if (!preset.empty() && presetHash != m_app.m_appliedRemotePresetHash)
    {
        const std::string fullPath = m_app.resolveShaderPath(preset);
        if (!fullPath.empty())
        {
            LOG_INFO("RemoteMetaSync: applying host preset '" + preset + "' (hash " + presetHash + ")");
            if (m_app.m_shaderEngine->loadPreset(fullPath))
            {
                m_app.m_ui->setCurrentShader(preset);
                m_app.m_appliedRemotePresetHash = presetHash;
                reloaded = true;
            }
            else
            {
                LOG_WARN("RemoteMetaSync: failed to load preset locally — bundle fetch is a Phase 4b TODO");
            }
        }
    }

    // Master pipeline toggle: mirror the host's "Apply shader pipeline".
    // Log every time it changes vs the local state, so a "shader vanishes
    // after N seconds" symptom can be traced back to whichever snapshot
    // flipped the value.
    const bool prevPipelineEnabled = m_app.m_ui->getShaderPipelineEnabled();
    m_app.m_ui->setShaderPipelineEnabled(pipelineEnabled);
    if (prevPipelineEnabled != pipelineEnabled)
    {
        LOG_INFO(std::string("RemoteMetaSync: pipelineEnabled changed ") +
                 (prevPipelineEnabled ? "true" : "false") + " → " +
                 (pipelineEnabled ? "true" : "false") +
                 " (preset='" + preset + "' hash=" + presetHash + ")");
    }

    // Parameter overrides apply on top of whatever preset is now active.
    // After a preset reload, the engine's parameters are at their preset
    // defaults; layering the host's overrides on top reproduces the look.
    if (!params.empty())
    {
        for (const auto &kv : params)
        {
            m_app.m_shaderEngine->setShaderParameter(kv.first, kv.second);
        }
        if (reloaded)
        {
            LOG_INFO("RemoteMetaSync: applied " + std::to_string(params.size()) +
                     " parameter override(s)");
        }
    }
}
