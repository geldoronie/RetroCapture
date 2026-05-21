#pragma once

#include <cstdint>
#include <cstddef>

/**
 * IAudioPlayback — abstract sink for the remote-stream audio path.
 *
 * Owned and driven by VideoCaptureRemote: the decode loop pulls audio
 * packets out of the MPEG-TS demuxer, decodes them into interleaved
 * float32 PCM, and hands the samples here via submit(). The platform
 * impl writes them to a system sink (PulseAudio on Linux, WASAPI on
 * Windows).
 *
 * The implementation is responsible for buffering. submit() is
 * non-blocking and returns the number of samples accepted (may be less
 * than requested if the buffer is full — caller decides whether to
 * drop or retry).
 *
 * Audio is the A/V master clock for the client: getClockUs() returns
 * the PTS (microseconds, stream-origin-relative) of the sample that's
 * currently being played out of the hardware, which the video render
 * gates against to stay in sync.
 */
class IAudioPlayback
{
public:
    virtual ~IAudioPlayback() = default;

    /**
     * Open the sink with the given PCM format. Samples submitted later
     * must match this format. Returns false on failure.
     *
     * @param sampleRate samples per second per channel (e.g. 44100)
     * @param channels   1 = mono, 2 = stereo
     */
    virtual bool open(uint32_t sampleRate, uint32_t channels) = 0;

    /**
     * Tear down the sink and discard any buffered samples.
     */
    virtual void close() = 0;

    /**
     * True between a successful open() and the matching close().
     */
    virtual bool isOpen() const = 0;

    /**
     * Submit interleaved float32 PCM samples. sampleCount is the
     * number of frames (groups of `channels` floats), not the number
     * of floats. firstPtsUs is the stream-origin-relative timestamp
     * of the first frame in this submission, used to advance the
     * playback clock.
     *
     * Returns the number of frames actually accepted (may be less
     * than sampleCount if the buffer is full; caller may drop the
     * leftovers).
     */
    virtual size_t submit(const float *interleaved,
                          size_t sampleCount,
                          int64_t firstPtsUs) = 0;

    /**
     * Approximate PTS (stream-origin-relative microseconds) of the
     * sample currently being played by the hardware. Returns the
     * last PTS we submitted minus the hardware-side buffered samples
     * worth of time. Used by the video path as the master clock.
     *
     * Returns 0 if no audio has been submitted yet.
     */
    virtual int64_t getClockUs() const = 0;

    /**
     * Drop any samples currently buffered in the impl-side queue.
     * Hardware buffer drains on its own. Used on disconnect /
     * reconnect to avoid playing stale audio from the previous
     * session.
     */
    virtual void flush() = 0;
};
