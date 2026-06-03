#pragma once

// macOS-only. Coordinates ScreenCaptureKit system-audio so there is only
// ever ONE SCStream alive — running two (the screen-capture video stream
// + a separate audio stream) makes the audio one go silent (#109).
//
// - The screen-capture SCStream (VideoCaptureScreen_mac) captures audio
//   too and feeds it here (onScreenAudio + setScreenProducerActive).
// - The system-audio capture (AudioCaptureCoreAudio) asks the hub for
//   audio (requestSystemAudio). The hub routes the screen stream's audio
//   to it when screen capture is active; otherwise it spins up its own
//   audio-only SCStream (the camera + system-audio case, which works).
//
// Silence keepalive: system output can be genuinely silent, and SCK does
// not always deliver buffers while nothing is playing. The mic path never
// hit this because a mic always produces samples. Without a steady audio
// timeline the stream's MediaSynchronizer holds video waiting for audio,
// overflows, and the encoder serves nothing (the "stream dies after a
// while" symptom). So whenever a system-audio consumer is active and no
// real audio has arrived recently, the hub injects silence into the bus
// to keep the timeline advancing exactly like a mic would.
#ifdef __APPLE__

#include "SystemAudioCaptureSCK.h"

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

class AudioBus;

class SckSystemAudioHub
{
public:
    static SckSystemAudioHub &instance();

    // Screen-capture side (the video SCStream that also captures audio).
    void setScreenProducerActive(bool active);
    void onScreenAudio(const int16_t *samples, std::size_t sampleCount);

    // System-audio consumer side (the audio capture wanting system audio).
    void requestSystemAudio(AudioBus *bus, uint32_t sampleRate, uint32_t channels);
    void releaseSystemAudio();

private:
    SckSystemAudioHub() = default;
    ~SckSystemAudioHub();

    // Forward real captured audio to the consumer and mark the timeline
    // alive so the silence keepalive backs off. Caller must hold m_mutex.
    void forwardRealAudioLocked(const int16_t *samples, std::size_t sampleCount);

    void startKeepaliveLocked();
    void stopKeepaliveAndJoin();
    void keepaliveLoop();

    std::mutex                              m_mutex;
    AudioBus                               *m_consumer = nullptr;
    uint32_t                                m_rate = 48000;
    uint32_t                                m_channels = 2;
    bool                                    m_screenActive = false;
    std::unique_ptr<SystemAudioCaptureSCK>  m_ownStream; // audio-only fallback

    // Silence keepalive.
    std::thread                             m_keepalive;
    std::atomic<bool>                       m_keepaliveRun{false};
    int64_t                                 m_lastRealPushUs = 0;
    bool                                    m_loggedFirstReal = false;
};

#endif // __APPLE__
